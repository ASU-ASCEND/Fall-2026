/**
 * ASCEND FA26 payload board (Internal / StratoCore) - ICM-20948 IMU test
 * =========================================================================
 *
 * Self-contained bench test for the IMU only. No CAN, no SD, no RTC driver,
 * no flight software needed.
 *
 * Arduino IDE setup
 * -----------------
 *   1. Boards Manager: "Raspberry Pi Pico/RP2040/RP2350" by Earle Philhower
 *      (package_rp2040_index.json), as already installed.
 *   2. Tools > Board > Raspberry Pi Pico/RP2040/RP2350 > Raspberry Pi Pico 2 W
 *   3. Tools > USB Stack > "Pico SDK" (default)
 *   4. Library Manager: install "Adafruit ICM20X" (it pulls in Adafruit BusIO
 *      and Adafruit Unified Sensor).
 *   5. Upload and open the Serial Monitor at 115200 baud.
 *
 * Wiring
 * ------
 * The Pico sits in the payload board socket (U100). This sketch talks to the
 * IMU on I2C0: SDA = GP4, SCL = GP5, level-shifted to the IMU's 1.8 V side.
 *
 * POWER: the IMU's VDDIO (U600 pin 8) is on +1V8, and the payload board has no
 * 1.8 V regulator. +1V8 arrives from the PnC over the stack connector J100
 * pin 3. With only USB plugged into the Pico the IMU is unpowered and cannot
 * answer. Either power the PnC stack, or inject 1.8 V on J100 pin 3 / the
 * +1V8 rail (never both at once). +3V3 for VDD comes from the Pico over USB.
 *
 * ADDRESS: the netlist ties AD0/SDO (U600 pin 9) to GND, so the IMU is at
 * 0x68 - the same address as the PCF8523 RTC. This sketch detects that case
 * and tells you to rework AD0, because two devices cannot be read reliably on
 * one address. Strap AD0 to +1V8 to move the IMU to 0x69 and it will stream.
 *
 * Serial commands: 's' = rescan + reprobe, 'r' = restart driver,
 * 'b' = reboot into BOOTSEL (for the next upload), 'h' = help.
 */

#include <Adafruit_ICM20948.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Board wiring (FA26 Internal board)
// ---------------------------------------------------------------------------
static const uint8_t PIN_SDA = 4;
static const uint8_t PIN_SCL = 5;

// 100 kHz is deliberately conservative: the IMU sits behind a BSS138 level
// shifter with 2.2k/10k pull-ups. Raise to 400000 once it is proven.
static const uint32_t I2C_HZ = 100000UL;

// ---------------------------------------------------------------------------
// Device addresses / registers
// ---------------------------------------------------------------------------
static const uint8_t PCF8523_ADDR = 0x68;  // RTC, fixed address
static const uint8_t IMU_ADDR_HIGH = 0x69; // AD0/SDO = +1V8  (recommended)
static const uint8_t IMU_ADDR_LOW = 0x68;  // AD0/SDO = GND   (factory build)
static const uint8_t ICM_REG_BANK_SEL = 0x7F;
static const uint8_t ICM_REG_WHO_AM_I = 0x00;
static const uint8_t ICM_CHIP_ID = 0xEA;

// Only set this to 1 if the PCF8523 is NOT populated on the board. With both
// chips at 0x68 the driver's configuration writes would also land in the RTC
// and every read would be the two chips' outputs AND-ed together.
#ifndef ALLOW_IMU_AT_0X68
#define ALLOW_IMU_AT_0X68 0
#endif

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
static const uint32_t SAMPLE_PERIOD_MS = 100; // 10 Hz print rate
static const uint32_t PROBE_RETRY_MS = 3000;  // retry while no IMU
static const uint16_t SUMMARY_EVERY = 50;     // sanity summary every 5 s

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static Adafruit_ICM20948 icm;
static bool imu_ready = false;
static uint8_t imu_addr = 0;
static uint32_t sample_count = 0;
static uint32_t next_sample_ms = 0;
static uint32_t next_probe_ms = 0;
static uint32_t next_masked_probe_ms = 0;
static bool collision_reported = false;
static bool absent_hint_printed = false;
static bool zero_warning_printed = false;

// ---------------------------------------------------------------------------
// Low-level I2C helpers
// ---------------------------------------------------------------------------
static bool i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t* out) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false; // no ACK
  if (Wire.requestFrom((int)addr, 1) != 1) return false;
  *out = (uint8_t)Wire.read();
  return true;
}

/**
 * Select register bank 0 then read WHO_AM_I.
 *
 * The ICM-20948 keeps its bank selection across resets of the driver, so the
 * bank is forced first. 0x7F is outside the PCF8523 register map, so when the
 * RTC shares the bus it simply ignores this write.
 *
 * @return 0xEA for an ICM-20948, 0xFF if the device did not answer.
 */
static uint8_t icmWhoAmI(uint8_t addr) {
  i2cWriteReg(addr, ICM_REG_BANK_SEL, 0x00);
  delay(2);
  uint8_t who = 0xFF;
  if (!i2cReadReg(addr, ICM_REG_WHO_AM_I, &who)) return 0xFF;
  return who;
}

/**
 * Probe 0x00 with the RTC's Control_1 forced to 0xFF.
 *
 * I2C is open drain, so when the RTC and the IMU both answer at 0x68 their
 * output bits are AND-ed. Control_1 normally reads 0x00, which hides the
 * IMU's WHO_AM_I completely. Writing 0xFF makes the RTC release every data
 * bit, so a read of register 0x00 shows the IMU's 0xEA if it is present and
 * powered. The original Control_1 value is restored immediately.
 *
 * Side effect: Control_1 bit 2 (STOP) is set for a couple of milliseconds, so
 * the RTC oscillator loses a few milliseconds. Harmless for a bench test.
 *
 * @param ctl1_out Receives the RTC Control_1 value that was restored.
 * @return 0xEA if the IMU answered, otherwise 0xFF / the masked RTC value.
 */
static uint8_t maskedWhoAmI(uint8_t* ctl1_out) {
  uint8_t ctl1 = 0x00;
  if (!i2cReadReg(PCF8523_ADDR, 0x00, &ctl1)) {
    // Neither device ACKs at 0x68.
    return 0xFF;
  }
  *ctl1_out = ctl1;

  if (!i2cWriteReg(PCF8523_ADDR, 0x00, 0xFF)) return 0xFF;
  delay(2);

  uint8_t who = 0xFF;
  i2cReadReg(PCF8523_ADDR, ICM_REG_WHO_AM_I, &who);

  i2cWriteReg(PCF8523_ADDR, 0x00, ctl1); // restore RTC Control_1
  delay(2);
  return who;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------
static void i2cScan() {
  Serial.println("# I2C0 scan (SDA=GP4, SCL=GP5):");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("#   ACK at 0x%02X", addr);
      if (addr == PCF8523_ADDR) Serial.print("  (PCF8523 RTC and/or ICM-20948)");
      Serial.println();
      found++;
    }
  }
  if (found == 0) {
    Serial.println("#   no devices responded - check SDA/SCL, +3V3, +1V8");
  }
}

static void printPowerHint() {
  Serial.println("#");
  Serial.println("# The IMU did not answer. Things to measure:");
  Serial.println("#   U600 pin 8  (VDDIO) must be 1.8 V - fed from J100 pin 3.");
  Serial.println("#     There is no 1.8 V regulator on this board; USB into the");
  Serial.println("#     Pico only provides +3V3. Power the PnC stack, or inject");
  Serial.println("#     1.8 V on J100 pin 3 (not both sources at once).");
  Serial.println("#   U600 pin 13 (VDD) must be 3.3 V.");
  Serial.println("#   U600 pin 22 (nCS) must be 1.8 V, pin 11 (FSYNC) GND.");
  Serial.println("#   Q600/Q601 level shifter: gate on 1.8 V, 3V3 side pull-ups");
  Serial.println("#     R601/R603, 1V8 side R600/R602, series R106/R107 to GP4/GP5.");
  Serial.println("#   AD0/SDO (U600 pin 9) must be +1V8 for address 0x69.");
}

static void printBanner() {
  Serial.println();
  Serial.println("# ------------------------------------------------");
  Serial.println("# ASCEND FA26 payload board - ICM-20948 IMU test");
  Serial.println("# I2C0: SDA=GP4 SCL=GP5, 100 kHz");
  Serial.println("# ------------------------------------------------");
}

// ---------------------------------------------------------------------------
// IMU bring-up
// ---------------------------------------------------------------------------
static bool startDriver(uint8_t addr) {
  Serial.printf("# Initialising Adafruit ICM20948 at 0x%02X ... ", addr);
  if (!icm.begin_I2C(addr, &Wire)) {
    Serial.println("FAILED");
    printPowerHint();
    return false;
  }
  Serial.println("ok");

  // Same ranges as the flight software.
  icm.setAccelRange(ICM20948_ACCEL_RANGE_16_G);
  icm.setGyroRange(ICM20948_GYRO_RANGE_1000_DPS);
  icm.setMagDataRate(AK09916_MAG_DATARATE_50_HZ);

  imu_addr = addr;
  imu_ready = true;
  sample_count = 0;
  zero_warning_printed = false;
  next_sample_ms = millis();

  Serial.printf("# IMU streaming at 0x%02X\n", addr);
  Serial.println(
      "# millis,ax_ms2,ay_ms2,az_ms2,gx_rads,gy_rads,gz_rads,mx_uT,my_uT,"
      "mz_uT,temp_C");
  return true;
}

static void tryFindImu() {
  const uint8_t who69 = icmWhoAmI(IMU_ADDR_HIGH);
  const uint8_t who68 = icmWhoAmI(IMU_ADDR_LOW);

  if (who69 == ICM_CHIP_ID) {
    Serial.printf("# probe: WHO_AM_I @0x69=0x%02X @0x68=0x%02X\n", who69, who68);
    startDriver(IMU_ADDR_HIGH);
    return;
  }

  if (who68 == ICM_CHIP_ID) {
    Serial.printf("# probe: WHO_AM_I @0x69=0x%02X @0x68=0x%02X\n", who69, who68);
    // A clean read at 0x68 means the RTC is not driving the bus (usually not
    // populated), so the IMU can be used directly.
    Serial.println(
        "# IMU answered cleanly at 0x68. The RTC appears to be absent, so "
        "0x68 is usable.");
    startDriver(IMU_ADDR_LOW);
    return;
  }

  // No direct answer. The RTC-masking probe briefly stops the RTC oscillator,
  // so only run it every 10 s instead of on every retry.
  if ((int32_t)(millis() - next_masked_probe_ms) < 0) {
    Serial.printf("# probe: WHO_AM_I @0x69=0x%02X @0x68=0x%02X\n", who69, who68);
    return;
  }
  next_masked_probe_ms = millis() + 10000;

  uint8_t ctl1 = 0x00;
  const uint8_t masked = maskedWhoAmI(&ctl1);
  Serial.printf(
      "# probe: WHO_AM_I @0x69=0x%02X @0x68=0x%02X masked@0x68=0x%02X\n", who69,
      who68, masked);

  if (masked == ICM_CHIP_ID) {
    if (!collision_reported) {
      collision_reported = true;
      Serial.printf(
          "# IMU is PRESENT at 0x68, hidden behind the PCF8523 RTC "
          "(RTC Control_1=0x%02X).\n",
          ctl1);
      Serial.println(
          "# Both chips are at 0x68 because AD0/SDO (U600 pin 9) is tied to "
          "GND.");
      Serial.println(
          "# Streaming is not reliable until AD0 is reworked: lift the GND "
          "connection");
      Serial.println(
          "# on pin 9 and strap it to +1V8. The sketch will then find the IMU "
          "at 0x69.");
    }
#if ALLOW_IMU_AT_0X68
    Serial.println(
        "# ALLOW_IMU_AT_0X68=1: driving 0x68 anyway. Values may be corrupted "
        "and the RTC registers will be overwritten.");
    startDriver(IMU_ADDR_LOW);
#else
    Serial.println("# Still waiting for the AD0 rework.");
#endif
    return;
  }

  if (!absent_hint_printed) {
    absent_hint_printed = true;
    Serial.printf("# masked WHO_AM_I @0x68=0x%02X (0xEA means the IMU is "
                  "present)\n",
                  masked);
    printPowerHint();
  }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
static float norm3(float x, float y, float z) {
  return sqrtf(x * x + y * y + z * z);
}

static void appendCsv(float v) {
  Serial.print(v, 4);
  Serial.print(',');
}

static void printSummary(const sensors_event_t& a, const sensors_event_t& g,
                         const sensors_event_t& m,
                         const sensors_event_t& t) {
  const float an = norm3(a.acceleration.x, a.acceleration.y, a.acceleration.z);
  const float gn = norm3(g.gyro.x, g.gyro.y, g.gyro.z) * 57.29578f; // rad/s -> dps
  const float mn = norm3(m.magnetic.x, m.magnetic.y, m.magnetic.z);

  const bool accel_ok = an > 8.5f && an < 11.0f;
  const bool gyro_ok = gn < 5.0f;
  const bool mag_ok = mn > 20.0f && mn < 80.0f;
  const bool temp_ok = t.temperature > 0.0f && t.temperature < 60.0f;

  // Printed with Serial.print(), not printf("%f"): some newlib builds link
  // without float printf support, and a bench tool must not print blanks.
  Serial.print("# summary: |a|=");
  Serial.print(an, 2);
  Serial.print(" m/s2 [");
  Serial.print(accel_ok ? "OK" : "CHECK");
  Serial.print("]  |g|=");
  Serial.print(gn, 2);
  Serial.print(" dps [");
  Serial.print(gyro_ok ? "OK" : "CHECK");
  Serial.print("]  |m|=");
  Serial.print(mn, 1);
  Serial.print(" uT [");
  Serial.print(mag_ok ? "OK" : "CHECK");
  Serial.print("]  T=");
  Serial.print(t.temperature, 2);
  Serial.print(" C [");
  Serial.print(temp_ok ? "OK" : "CHECK");
  Serial.println("]");

  if (an == 0.0f && gn == 0.0f && mn == 0.0f && !zero_warning_printed) {
    zero_warning_printed = true;
    Serial.println(
        "# WARNING: every value is exactly zero. The bus ACKs but data is not "
        "moving - suspect the level shifter, a loaded SDA/SCL, or the sensor "
        "stuck in reset.");
  }
}

static void readAndPrint() {
  sensors_event_t a, g, m, t;
  if (!icm.getEvent(&a, &g, &m, &t)) {
    Serial.println("# getEvent() failed - IMU stopped responding");
    return;
  }

  sample_count++;

  Serial.print(millis());
  Serial.print(',');
  appendCsv(a.acceleration.x);
  appendCsv(a.acceleration.y);
  appendCsv(a.acceleration.z);
  appendCsv(g.gyro.x);
  appendCsv(g.gyro.y);
  appendCsv(g.gyro.z);
  appendCsv(m.magnetic.x);
  appendCsv(m.magnetic.y);
  appendCsv(m.magnetic.z);
  Serial.println(t.temperature, 4);

  if (sample_count % SUMMARY_EVERY == 0) {
    printSummary(a, g, m, t);
  }
}

// ---------------------------------------------------------------------------
// Serial commands
// ---------------------------------------------------------------------------
static void printHelp() {
  Serial.println(
      "# commands: s = rescan/reprobe, r = restart driver, b = reboot into "
      "BOOTSEL, h = help");
}

static void enterBootloader() {
  Serial.println("# Rebooting into BOOTSEL - the RP2350 drive will mount.");
  Serial.flush();
  delay(100);
  rp2040.rebootToBootloader();
}

static void handleSerial() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c == 's' || c == 'S') {
      Serial.println("# rescan requested");
      i2cScan();
      imu_ready = false;
      next_probe_ms = millis();
      next_masked_probe_ms = millis();
    } else if (c == 'r' || c == 'R') {
      Serial.println("# driver restart requested");
      imu_ready = false;
      next_probe_ms = millis();
      next_masked_probe_ms = millis();
    } else if (c == 'b' || c == 'B') {
      enterBootloader();
    } else if (c == 'h' || c == 'H' || c == '?') {
      printHelp();
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && millis() - start < 3000) {
    delay(10);
  }

  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();
  Wire.setClock(I2C_HZ);

  printBanner();
  i2cScan();
  printHelp();
  next_probe_ms = millis();
}

void loop() {
  handleSerial();

  if (!imu_ready) {
    if ((int32_t)(millis() - next_probe_ms) >= 0) {
      next_probe_ms = millis() + PROBE_RETRY_MS;
      tryFindImu();
    }
    return;
  }

  if ((int32_t)(millis() - next_sample_ms) >= 0) {
    next_sample_ms = millis() + SAMPLE_PERIOD_MS;
    readAndPrint();
  }
}
