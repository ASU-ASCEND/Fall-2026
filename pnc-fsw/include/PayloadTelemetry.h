#pragma once

#include <stdint.h>

/**
 * @file PayloadTelemetry.h
 * @brief Live decode of the payload board's packets received over CAN.
 *
 * The payload board fragments its "ASU!" sensor packet over CAN 2.0
 * (CanBus PACKET frames); can_on_packet() reassembles the bytes and hands
 * them here. This module keeps the latest decoded values plus per-sensor
 * freshness so the Wi-Fi checkout page can show the payload board's
 * sensors next to the PnC's own.
 *
 * Layout contract (packet-decoder/parse_payload.py is the reference):
 * header "ASU!" | u32 mask | u16 length | u32 payload-millis, then one
 * block per mask-set sensor in sensors[] order, then an int8 checksum
 * that zeroes the byte sum. sensors[i] owns mask bit (sentinel-1-i),
 * where the sentinel (highest set mask bit) is the compiled sensor count.
 * Two builds fly and share per-index block sizes: the pre-science build
 * (sentinel 10) and the science build (sentinel 11, TMP117Sci appended).
 * The old geiger build (also sentinel 11) cannot collide: no mask subset
 * of its block sizes sums to a length the science table accepts.
 */

/** @brief Mask slots, indexed like payload-fsw sensors[]. */
enum PayloadSlot : uint8_t {
  PAYLOAD_SLOT_PICO = 0,        // float temp_c
  PAYLOAD_SLOT_IMU = 1,         // reserved: IMU dropped 2026-11-20, bit
                                // never set; its 40 bytes are consumed
  PAYLOAD_SLOT_RTC = 2,         // reserved nullptr in both current builds
  PAYLOAD_SLOT_TMP117_EXT = 3,  // External board TMP117, float temp_c
  PAYLOAD_SLOT_BME688_EXT = 4,  // External board BME688
  PAYLOAD_SLOT_ENS160 = 5,      // External board ENS160
  PAYLOAD_SLOT_SHTC3 = 6,       // External board SHTC3
  PAYLOAD_SLOT_AS7331 = 7,      // UVA/UVB/UVC, 3 floats
  PAYLOAD_SLOT_BMP390_SCI = 8,  // Science BMP390: double, double, float
  PAYLOAD_SLOT_OZONE = 9,       // int16 ppb
  PAYLOAD_SLOT_TMP117_SCI = 10, // Science TMP117, float temp_c
  PAYLOAD_SLOT_COUNT = 11,
};

/**
 * @brief Latest decoded payload telemetry; copy out via payload_telemetry_get.
 *
 * Written by core 1 (CAN service) under a mutex, read by core 0.
 */
struct PayloadTelemetry {
  uint8_t node = 0xFF;             // 0xFF until the first packet
  uint8_t last_length = 0;
  uint16_t last_mask = 0;
  uint32_t payload_uptime_ms = 0;  // payload board millis from its packet
  uint32_t last_rx_ms = 0;          // PnC millis at the last accepted packet
  uint32_t packets = 0;            // packets that passed full validation
  uint32_t frames_seen = 0;        // reassembled packets of any layout
  // PnC millis of each slot's last good update; 0 = never. A slot whose
  // block fails to decode (e.g. a non-finite float) keeps its old age so
  // the checkout page shows it going stale instead of publishing junk.
  uint32_t slot_ms[PAYLOAD_SLOT_COUNT] = {};

  float pico_temp_c = 0;
  float tmp117_ext_c = 0;
  float bme_temp_c = 0;
  uint32_t bme_pressure_pa = 0;
  float bme_humidity_pct = 0;
  uint32_t bme_gas_ohm = 0;
  uint8_t ens_aqi = 0;
  uint16_t ens_tvoc_ppb = 0;
  uint16_t ens_eco2_ppm = 0;
  float shtc3_temp_c = 0;
  float shtc3_humidity_pct = 0;
  float uv_a_uw_cm2 = 0;
  float uv_b_uw_cm2 = 0;
  float uv_c_uw_cm2 = 0;
  double bmp390_temp_c = 0;
  double bmp390_pressure_pa = 0;
  float bmp390_alt_m = 0;
  int16_t ozone_ppb = 0;
  float tmp117_sci_c = 0;
};

/** @brief Initialise the shared state; call once before CAN comes up. */
void payload_telemetry_init();

/**
 * @brief Decode one reassembled packet; called from can_on_packet (core 1).
 *
 * Cheap and non-blocking: validates the header/checksum, decodes into a
 * private copy, then commits under the mutex. Anything that fails
 * validation only bumps frames_seen.
 */
void payload_telemetry_ingest(uint8_t node, const uint8_t* data, uint8_t len);

/**
 * @brief Copy out the latest snapshot.
 * @return false until the first fully valid packet has been ingested.
 */
bool payload_telemetry_get(PayloadTelemetry* out);
