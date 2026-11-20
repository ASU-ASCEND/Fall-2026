#include "PayloadTelemetry.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

// pico-sdk mutex, not the Arduino wrapper: this is shared core 0/1 state.
#include "pico/mutex.h"

namespace {

// Packet bytes "ASU!" as a little-endian u32.
constexpr uint32_t kSync = 0x21555341;
constexpr uint8_t kHeaderLen = 14;  // sync(4) mask(4) length(2) millis(4)
// Block sizes per payload-fsw sensors[] index; must match
// packet-decoder/parse_payload.py for both sentinel-10 and sentinel-11.
const uint8_t kSlotBytes[PAYLOAD_SLOT_COUNT] = {4, 40, 7, 4, 16, 5,
                                                8, 12, 20, 2, 4};

mutex_t lock;
PayloadTelemetry snapshot;
bool have_packet = false;

}  // namespace

void payload_telemetry_init() {
  mutex_init(&lock);
  snapshot = PayloadTelemetry();
  have_packet = false;
}

void payload_telemetry_ingest(uint8_t node, const uint8_t* data, uint8_t len) {
  // Copy the current state out, decode off to the side, then commit.
  // Only the CAN task (core 1) writes, so copy-modify-commit is safe.
  mutex_enter_blocking(&lock);
  snapshot.frames_seen++;
  PayloadTelemetry next = snapshot;
  mutex_exit(&lock);

  // Header: "ASU!" | u32 mask | u16 length | u32 millis, checksum last.
  if (len < kHeaderLen + 1) return;
  uint32_t sync = 0;
  uint16_t mask = 0, plen = 0;
  memcpy(&sync, data, 4);
  memcpy(&mask, data + 4, 2);
  memcpy(&plen, data + 8, 2);
  if (sync != kSync || plen != len || mask == 0) return;

  // The stored byte is the two's complement of the sum of the others,
  // so the whole frame (including it) sums to 0 mod 256.
  int8_t sum = 0;
  for (uint8_t i = 0; i < len; i++) sum += (int8_t)data[i];
  if (sum != 0) return;

  // The highest set mask bit is the sentinel (compiled sensor count).
  // Only the two current builds are decoded; anything else is counted
  // as a frame but leaves the values untouched.
  uint8_t sentinel = 0;
  for (uint8_t bit = 16; bit > 0; bit--) {
    if (mask & (1u << (bit - 1))) {
      sentinel = bit - 1;
      break;
    }
  }
  if (sentinel != 10 && sentinel != 11) return;

  uint16_t offset = kHeaderLen;
  for (uint8_t i = 0; i < sentinel; i++) {
    if (!(mask & (1u << (sentinel - 1 - i)))) continue;
    if (offset + kSlotBytes[i] > (uint16_t)(len - 1)) return;  // truncated
    const uint8_t* p = data + offset;
    bool ok = true;
    switch (i) {
      case PAYLOAD_SLOT_PICO: {
        float v;
        memcpy(&v, p, 4);
        if (isfinite(v)) next.pico_temp_c = v;
        else ok = false;
        break;
      }
      case PAYLOAD_SLOT_IMU:
        // Reserved since the IMU was dropped (2026-11-20): consume the
        // block, keep no values, do not mark the slot fresh.
        ok = false;
        break;
      case PAYLOAD_SLOT_RTC:
        // Reserved slot in both current builds; nothing to track.
        break;
      case PAYLOAD_SLOT_TMP117_EXT: {
        float v;
        memcpy(&v, p, 4);
        if (isfinite(v)) next.tmp117_ext_c = v;
        else ok = false;
        break;
      }
      case PAYLOAD_SLOT_BME688_EXT: {
        float temp_c, humidity;
        uint32_t pressure, gas;
        memcpy(&temp_c, p, 4);
        memcpy(&pressure, p + 4, 4);
        memcpy(&humidity, p + 8, 4);
        memcpy(&gas, p + 12, 4);
        if (isfinite(temp_c) && isfinite(humidity)) {
          next.bme_temp_c = temp_c;
          next.bme_pressure_pa = pressure;
          next.bme_humidity_pct = humidity;
          next.bme_gas_ohm = gas;
        } else {
          ok = false;
        }
        break;
      }
      case PAYLOAD_SLOT_ENS160: {
        memcpy(&next.ens_aqi, p, 1);
        memcpy(&next.ens_tvoc_ppb, p + 1, 2);
        memcpy(&next.ens_eco2_ppm, p + 3, 2);
        break;
      }
      case PAYLOAD_SLOT_SHTC3: {
        float temp_c, humidity;
        memcpy(&temp_c, p, 4);
        memcpy(&humidity, p + 4, 4);
        if (isfinite(temp_c) && isfinite(humidity)) {
          next.shtc3_temp_c = temp_c;
          next.shtc3_humidity_pct = humidity;
        } else {
          ok = false;
        }
        break;
      }
      case PAYLOAD_SLOT_AS7331: {
        float uva, uvb, uvc;
        memcpy(&uva, p, 4);
        memcpy(&uvb, p + 4, 4);
        memcpy(&uvc, p + 8, 4);
        if (isfinite(uva) && isfinite(uvb) && isfinite(uvc)) {
          next.uv_a_uw_cm2 = uva;
          next.uv_b_uw_cm2 = uvb;
          next.uv_c_uw_cm2 = uvc;
        } else {
          ok = false;
        }
        break;
      }
      case PAYLOAD_SLOT_BMP390_SCI: {
        double temp_c, pressure;
        float altitude;
        memcpy(&temp_c, p, 8);
        memcpy(&pressure, p + 8, 8);
        memcpy(&altitude, p + 16, 4);
        if (isfinite(temp_c) && isfinite(pressure) && isfinite(altitude)) {
          next.bmp390_temp_c = temp_c;
          next.bmp390_pressure_pa = pressure;
          next.bmp390_alt_m = altitude;
        } else {
          ok = false;
        }
        break;
      }
      case PAYLOAD_SLOT_OZONE: {
        int16_t ppb;
        memcpy(&ppb, p, 2);
        next.ozone_ppb = ppb;
        break;
      }
      case PAYLOAD_SLOT_TMP117_SCI: {
        float v;
        memcpy(&v, p, 4);
        if (isfinite(v)) next.tmp117_sci_c = v;
        else ok = false;
        break;
      }
      default:
        ok = false;
        break;
    }
    // A slot that decoded cleanly keeps its freshness; one that did not
    // keeps its previous age, so the checkout page shows it going stale.
    if (ok) next.slot_ms[i] = millis();
    offset += kSlotBytes[i];
  }
  if (offset + 1 != len) return;  // trailing bytes the layout cannot explain

  memcpy(&next.payload_uptime_ms, data + 10, 4);
  next.node = node;
  next.last_mask = mask;
  next.last_length = len;
  next.packets++;
  next.last_rx_ms = millis();

  mutex_enter_blocking(&lock);
  snapshot = next;
  have_packet = true;
  mutex_exit(&lock);
}

bool payload_telemetry_get(PayloadTelemetry* out) {
  if (!out) return false;
  mutex_enter_blocking(&lock);
  bool got = have_packet;
  if (got) *out = snapshot;
  mutex_exit(&lock);
  return got;
}
