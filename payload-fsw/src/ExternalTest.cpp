/**
 * @file ExternalTest.cpp
 * @brief Raw + driver probes for the External sensor board on I2C1.
 *
 * 'E' runs one pass: bus scan, raw ID registers (so a wrong driver address
 * cannot hide a working part), then each vendor driver with one live reading.
 * 'W' keeps reading the same devices for a minute so warm-up behaviour (the
 * ENS160 needs about 60 s in standard mode) is visible.
 */
#include "ExternalTest.h"

#include <Adafruit_BME680.h>
#include <Adafruit_BMP3XX.h>
#include <Adafruit_SHTC3.h>
#include <SparkFun_AS7331.h>
#include <SparkFun_ENS160.h>
#include <SparkFun_TMP117.h>
#include <Wire.h>

#include "PayloadConfig.h"

namespace {

const uint8_t ADDR_TMP117 = 0x48;
const uint8_t ADDR_ENS160 = 0x53;
const uint8_t ADDR_SHTC3 = 0x70;
const uint8_t ADDR_BMP390 = 0x76;
const uint8_t ADDR_BME688 = 0x77;
const uint8_t ADDR_AS7331 = 0x74;

TwoWire& bus = STRATOSENSE_I2C;

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, uint8_t n) {
  bus.beginTransmission(addr);
  bus.write(reg);
  if (bus.endTransmission(false) != 0) return false;
  if (bus.requestFrom((int)addr, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = bus.read();
  return true;
}

bool ping(uint8_t addr) {
  bus.beginTransmission(addr);
  return bus.endTransmission() == 0;
}

void printHex(const uint8_t* buf, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) Serial.printf(" %02X", buf[i]);
}

void rawProbe() {
  Serial.println("[EXT] --- raw register probe ---");

  // TMP117 device ID: reg 0x0F, 16 bit MSB first, expect 0x0117
  uint8_t b[3] = {0, 0, 0};
  if (readRegs(ADDR_TMP117, 0x0F, b, 2)) {
    const uint16_t id = ((uint16_t)b[0] << 8) | b[1];
    Serial.printf("[EXT] 0x48 TMP117  dev_id=0x%04X %s\n", id,
                  id == 0x0117 ? "OK" : "UNEXPECTED");
  } else {
    Serial.println("[EXT] 0x48 TMP117  NO ACK");
  }

  // ENS160 part ID: reg 0x00, 16 bit as 0x60 then 0x01 -> 0x0160
  if (readRegs(ADDR_ENS160, 0x00, b, 2)) {
    Serial.printf("[EXT] 0x53 ENS160  part_id bytes=");
    printHex(b, 2);
    const bool ok = (b[0] == 0x60 && b[1] == 0x01);
    Serial.printf(" %s\n", ok ? "OK (0x0160)" : "UNEXPECTED");
  } else {
    Serial.println("[EXT] 0x53 ENS160  NO ACK");
  }

  // SHTC3 has no register map: 0xEFC8 returns two ID bytes plus a CRC. Only
  // the low 6 bits of the LSB are defined, hence the 0x083F mask (same check
  // the Adafruit driver uses). The part NACKs while asleep, so wake it first.
  bus.beginTransmission(ADDR_SHTC3);
  bus.write(0x35);
  bus.write(0x17);
  if (bus.endTransmission() != 0) {
    Serial.println("[EXT] 0x70 SHTC3   NO ACK (wake)");
  } else {
    delay(2);
    bus.beginTransmission(ADDR_SHTC3);
    bus.write(0xEF);
    bus.write(0xC8);
    if (bus.endTransmission() != 0) {
      Serial.println("[EXT] 0x70 SHTC3   NO ACK (id)");
    } else {
      delay(2);
      if (bus.requestFrom((int)ADDR_SHTC3, 3) == 3) {
        const uint8_t msb = bus.read();
        const uint8_t lsb = bus.read();
        const uint8_t crc = bus.read();
        const uint16_t id = ((uint16_t)msb << 8) | lsb;
        Serial.printf("[EXT] 0x70 SHTC3   id=0x%04X crc=0x%02X %s\n", id, crc,
                      (id & 0x083F) == 0x0807 ? "OK" : "UNEXPECTED");
      } else {
        Serial.println("[EXT] 0x70 SHTC3   read failed");
      }
    }
  }

  // BMP390 chip ID: reg 0x00, expect 0x60 (BMP388 would be 0x50)
  if (readRegs(ADDR_BMP390, 0x00, b, 1)) {
    Serial.printf("[EXT] 0x76 BMP390  chip_id=0x%02X %s\n", b[0],
                  b[0] == 0x60 ? "OK" : "UNEXPECTED");
  } else {
    Serial.println("[EXT] 0x76 BMP390  NO ACK");
  }

  // BME688 chip ID: reg 0xD0, expect 0x61
  if (readRegs(ADDR_BME688, 0xD0, b, 1)) {
    Serial.printf("[EXT] 0x77 BME688  chip_id=0x%02X %s\n", b[0],
                  b[0] == 0x61 ? "OK" : "UNEXPECTED");
  } else {
    Serial.println("[EXT] 0x77 BME688  NO ACK");
  }

  // The Adafruit driver fails init when the calibration block cannot be read
  // cleanly. Read it twice and compare: identical, plausible trim values mean
  // the bus is fine and the failure is elsewhere.
  {
    uint8_t c1[24] = {0};
    uint8_t c2[24] = {0};
    const bool r1 = readRegs(ADDR_BME688, 0x8A, c1, sizeof(c1));
    delay(5);
    const bool r2 = readRegs(ADDR_BME688, 0x8A, c2, sizeof(c2));
    bool same = r1 && r2;
    for (uint8_t i = 0; i < sizeof(c1); i++) {
      if (c1[i] != c2[i]) same = false;
    }
    Serial.printf("[EXT] 0x77 BME688  calib read1=%d read2=%d same=%d", r1, r2,
                  same ? 1 : 0);
    if (r1) {
      Serial.print(" calib=");
      printHex(c1, sizeof(c1));
    }
    Serial.println();
    uint8_t status = 0;
    if (readRegs(ADDR_BME688, 0x74, &status, 1)) {
      Serial.printf("[EXT] 0x77 BME688  ctrl_meas=0x%02X\n", status);
    }

    // Force one conversion here (temp 8x, press 4x, forced) so the pressure
    // ADC can be judged independently of the driver: BME68X_REG_FIELD0 = 0x1D
    // holds press (3 bytes), then temp (3 bytes), then humidity. The data does
    // not change until the next conversion, so repeating the read shows bus
    // errors rather than sensor noise.
    bus.beginTransmission(ADDR_BME688);
    bus.write((uint8_t)0x74);
    bus.write((uint8_t)(0x8C | 0x01));
    bus.endTransmission();
    delay(500);  // much longer than one temp+press+hum conversion
    for (uint8_t pass = 0; pass < 6; pass++) {
      if (pass == 3) {
        bus.setClock(50000);
        Serial.println("[EXT] 0x77 BME688  (I2C clock lowered to 50 kHz)");
      }
      uint8_t p[6] = {0, 0, 0, 0, 0, 0};
      if (readRegs(ADDR_BME688, 0x1D, p, 6)) {
        const uint32_t press =
            ((uint32_t)p[0] << 12) | ((uint32_t)p[1] << 4) | (p[2] >> 4);
        const uint32_t temp =
            ((uint32_t)p[3] << 12) | ((uint32_t)p[4] << 4) | (p[5] >> 4);
        Serial.printf("[EXT] 0x77 BME688  read%d press_adc=%lu temp_adc=%lu (",
                      pass, press, temp);
        printHex(p, 6);
        Serial.println(")");
      }
      delay(20);
    }
    bus.setClock(100000);
  }

  // AS7331 has no ID register. Report which strap addresses answer and dump
  // the first four config bytes. Only 0x74/0x75 are probed: 0x76/0x77 hold the
  // BMP390 and BME688, and a driver begin() writes config registers.
  for (uint8_t addr = ADDR_AS7331; addr <= ADDR_AS7331 + 1; addr++) {
    if (!ping(addr)) {
      Serial.printf("[EXT] 0x%02X AS7331  NO ACK\n", addr);
      continue;
    }
    uint8_t regs[4] = {0, 0, 0, 0};
    if (readRegs(addr, 0x00, regs, 4)) {
      Serial.printf("[EXT] 0x%02X AS7331  ACK regs[0..3]=", addr);
      printHex(regs, 4);
      Serial.println();
    } else {
      Serial.printf("[EXT] 0x%02X AS7331  ACK but read failed\n", addr);
    }
  }
}

struct Devices {
  // Adafruit_BME680's default constructor binds to Wire (I2C0), so it must be
  // constructed with the external bus explicitly. The others take the bus in
  // begin(), or are bus-agnostic.
  Adafruit_BME680 bme{&bus};
  TMP117 tmp;
  Adafruit_SHTC3 shtc3;
  Adafruit_BMP3XX bmp;
  SparkFun_ENS160 ens;
  SfeAS7331ArdI2C uv;
  bool tmp_ok = false;
  bool shtc_ok = false;
  bool bme_ok = false;
  bool bmp_ok = false;
  bool ens_ok = false;
  bool uv_ok = false;
};

void beginDevices(Devices& d) {
  d.tmp_ok = d.tmp.begin(ADDR_TMP117, bus);

  d.shtc_ok = d.shtc3.begin(&bus);

  if (d.bme.begin(ADDR_BME688)) {
    d.bme.setTemperatureOversampling(BME680_OS_8X);
    d.bme.setHumidityOversampling(BME680_OS_2X);
    d.bme.setPressureOversampling(BME680_OS_4X);
    d.bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    d.bme.setGasHeater(320, 150);
    d.bme_ok = true;
  }

  if (d.bmp.begin_I2C(ADDR_BMP390, &bus)) {
    d.bmp.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
    d.bmp.setPressureOversampling(BMP3_OVERSAMPLING_4X);
    d.bmp.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_3);
    d.bmp.setOutputDataRate(BMP3_ODR_50_HZ);
    d.bmp_ok = true;
  }

  if (d.ens.begin(bus, ADDR_ENS160)) {
    d.ens.setOperatingMode(SFE_ENS160_STANDARD);
    d.ens_ok = true;
  }

  d.uv_ok = d.uv.begin(ADDR_AS7331, bus);
}

void printPass(Devices& d) {
  Serial.printf("[EXT] TMP117  %s", d.tmp_ok ? "OK  " : "FAIL");
  if (d.tmp_ok) Serial.printf("  %.3f C", d.tmp.readTempC());
  Serial.println();

  Serial.printf("[EXT] SHTC3   %s", d.shtc_ok ? "OK  " : "FAIL");
  if (d.shtc_ok) {
    sensors_event_t humidity, temp;
    d.shtc3.getEvent(&humidity, &temp);
    Serial.printf("  %.2f C  %.2f %%RH", temp.temperature,
                  humidity.relative_humidity);
  }
  Serial.println();

  Serial.printf("[EXT] BME688  %s", d.bme_ok ? "OK  " : "FAIL");
  if (d.bme_ok) {
    if (d.bme.performReading()) {
      Serial.printf("  %.2f C  %.1f Pa  %.2f %%RH  %.0f ohm", d.bme.temperature,
                    d.bme.pressure, d.bme.humidity, d.bme.gas_resistance);
      // Raw conversion registers: press 0x1D..0x1F, temp 0x20..0x22,
      // hum 0x25..0x26, gas 0x2A..0x2B. A pressure ADC near zero with a valid
      // temperature ADC means the pressure channel is not converting.
      uint8_t raw[10] = {0};
      if (readRegs(ADDR_BME688, 0x1D, raw, sizeof(raw))) {
        Serial.print("  raw=");
        printHex(raw, sizeof(raw));
      }
    } else {
      Serial.printf("  begin OK, reading failed");
    }
  }
  Serial.println();

  Serial.printf("[EXT] BMP390  %s", d.bmp_ok ? "OK  " : "FAIL");
  if (d.bmp_ok) {
    if (d.bmp.performReading()) {
      Serial.printf("  %.2f C  %.1f Pa  %.1f m", d.bmp.temperature,
                    d.bmp.pressure, d.bmp.readAltitude(1013.25));
    } else {
      Serial.printf("  begin OK, reading failed");
    }
  }
  Serial.println();

  Serial.printf("[EXT] ENS160  %s", d.ens_ok ? "OK  " : "FAIL");
  if (d.ens_ok) {
    Serial.printf("  AQI=%u TVOC=%u ppb ECO2=%u ppm", d.ens.getAQI(),
                  d.ens.getTVOC(), d.ens.getECO2());
  }
  Serial.println();

  Serial.printf("[EXT] AS7331  %s", d.uv_ok ? "OK  " : "FAIL");
  if (d.uv_ok) {
    if (d.uv.readAllUV() == ksfTkErrOk) {
      Serial.printf("  UVA=%.3f UVB=%.3f UVC=%.3f uW/cm2", d.uv.getUVA(),
                    d.uv.getUVB(), d.uv.getUVC());
    } else {
      Serial.printf("  begin OK, read failed");
    }
  }
  Serial.println();
}

}  // namespace

void external_test() {
  Serial.println("[EXT] External board test on I2C1 (SDA=GP6 SCL=GP7)");

  // Scan first: a device that answers at an unexpected address is often a
  // strap problem, not a dead part.
  uint8_t found = 0;
  Serial.print("[EXT] scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (ping(addr)) {
      Serial.printf(" 0x%02X", addr);
      found++;
    }
  }
  if (found == 0) Serial.print(" none");
  Serial.println();

  rawProbe();

  Serial.println("[EXT] --- driver probe ---");
  Devices d;
  beginDevices(d);
  printPass(d);
  Serial.println("[EXT] done");
}

void external_watch(uint8_t samples, uint8_t period_s) {
  Serial.printf("[EXT] watch %u samples every %us\n", samples, period_s);
  Devices d;
  beginDevices(d);
  for (uint8_t i = 0; i < samples; i++) {
    Serial.printf("[EXT] --- sample %u ---\n", i);
    printPass(d);
    if (i + 1 < samples) delay((uint32_t)period_s * 1000);
  }
  Serial.println("[EXT] watch done");
}
