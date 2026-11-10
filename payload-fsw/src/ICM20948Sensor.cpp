#include "ICM20948Sensor.h"

#include <math.h>

/**
 * @brief Construct a new ICM20948Sensor object with default minimum_period of 0
 *
 */
ICM20948Sensor::ICM20948Sensor() : ICM20948Sensor(0) {}

/**
 * @brief Construct a new ICM20948Sensor object, given mininum_period between
 * sensor reads
 *
 * @param minimum_period Minimum time to wait between readings in ms
 */
ICM20948Sensor::ICM20948Sensor(unsigned long minimum_period)
    : Sensor("ICM20948",
             "ICM AccX,ICM AccY,ICM AccZ,ICM GyroX,ICM "
             "GyroY,ICM GyroZ,ICM MagX,ICM MagY,ICM MagZ,ICM TempC,",
             minimum_period) {}

/**
 * @brief Verifies that the ICM is connected and working.
 *
 * The FA26 Internal board ties AD0/SDO to GND, so the I2C address is 0x68.
 * Adafruit's default is 0x69, so both are tried and the result is logged.
 *
 * @return true if connected and working
 * @return false if not connected and working
 */
static uint8_t icmReadReg(TwoWire& wire, uint8_t addr, uint8_t reg) {
  wire.beginTransmission(addr);
  wire.write(reg);
  if (wire.endTransmission(false) != 0) return 0xFF;
  if (wire.requestFrom((int)addr, 1) != 1) return 0xFF;
  return (uint8_t)wire.read();
}

static bool icmWriteReg(TwoWire& wire, uint8_t addr, uint8_t reg,
                        uint8_t value) {
  wire.beginTransmission(addr);
  wire.write(reg);
  wire.write(value);
  return wire.endTransmission() == 0;
}

/**
 * @brief Probe 0x00 with the RTC's data temporarily forced to 0xFF.
 *
 * I2C is open drain, so two slaves at 0x68 AND their output bits. The RTC's
 * Control_1 normally masks the IMU's WHO_AM_I. Writing 0xFF to Control_1
 * makes the RTC release every data bit, so a read of 0x00 shows the IMU's
 * WHO_AM_I (0xEA) if the IMU is present and powered. The original Control_1 is
 * restored immediately (the clock may lose a few milliseconds).
 */
static uint8_t icmProbeWithRtcMasked(TwoWire& wire) {
  // Select bank 0 first: if an earlier init left the IMU in bank 3, register
  // 0x00 is not WHO_AM_I. 0x7F is outside the PCF8523 register map, so the RTC
  // ignores this write.
  icmWriteReg(wire, 0x68, 0x7F, 0x00);
  delay(1);

  uint8_t control1 = icmReadReg(wire, 0x68, 0x00);
  if (control1 == 0xFF) {
    return 0xFF;
  }
  if (!icmWriteReg(wire, 0x68, 0x00, 0xFF)) {
    return 0xFF;
  }
  delay(2);
  uint8_t who = icmReadReg(wire, 0x68, 0x00);
  icmWriteReg(wire, 0x68, 0x00, control1);
  return who;
}

bool ICM20948Sensor::verify() {
  // Probe WHO_AM_I before handing the address to the driver. The PCF8523 RTC
  // also answers at 0x68 on this board, so an unconditional begin_I2C(0x68)
  // writes IMU configuration into the RTC registers.
  const uint8_t who_68 = icmReadReg(STRATOCORE_I2C, 0x68, 0x00);
  const uint8_t who_69 = icmReadReg(STRATOCORE_I2C, 0x69, 0x00);
  log_core_printf("ICM probe: WHO_AM_I@0x68=0x%02X @0x69=0x%02X\n", who_68,
                  who_69);

  uint8_t addr = 0xFF;
  if (who_69 == ICM20948_CHIP_ID) {
    addr = 0x69;
  } else if (who_68 == ICM20948_CHIP_ID) {
#if ICM20948_ADDR_0X68_PRESENT
    addr = 0x68;
#else
    log_core(
        "ICM20948 answers at 0x68, but that collides with the PCF8523 at "
        "0x68; tie AD0 high (0x69) before enabling the IMU");
#if PAYLOAD_DUMMY_IMU
    this->dummy_ = true;
    log_core("IMU dummy mode: emitting synthetic IMU data on the CAN path");
    return true;
#else
    return false;
#endif
#endif
  }

  if (addr == 0xFF) {
    // Neither address answered cleanly. Distinguish "IMU absent/unpowered"
    // from "IMU present but hidden behind the RTC at 0x68". Only do this once:
    // it briefly stops the RTC oscillator, and the recovery system retries
    // verify() several times.
    static bool masked_probe_done = false;
    if (!masked_probe_done) {
      masked_probe_done = true;
      uint8_t masked = icmProbeWithRtcMasked(STRATOCORE_I2C);
      log_core_printf(
          "ICM masked probe at 0x68: WHO_AM_I=0x%02X (0xEA means the IMU is "
          "present)\n",
          masked);
      if (masked == ICM20948_CHIP_ID) {
        log_core(
            "IMU shares 0x68 with the RTC: tie AD0/SDO (U600 pin 9) to +1V8 "
            "to move it to 0x69");
      } else {
        log_core(
            "IMU did not answer even with the RTC masked: check +1V8 VDDIO at "
            "U600 pin 8 and whether U600 is populated");
      }
    }
#if PAYLOAD_DUMMY_IMU
    if (!this->dummy_) {
      this->dummy_ = true;
      log_core("IMU dummy mode: emitting synthetic IMU data on the CAN path");
    }
    return true;
#else
    return false;
#endif
  }

  bool ok = this->icm.begin_I2C(addr, &STRATOCORE_I2C);
  if (ok) {
    log_core_printf("ICM20948 found at 0x%02X\n", addr);
  }
  if (!ok) {
    return false;
  }

  icm.setAccelRange(icm20948_accel_range_t::ICM20948_ACCEL_RANGE_16_G);
  icm.setGyroRange(icm20948_gyro_range_t::ICM20948_GYRO_RANGE_1000_DPS);
  icm.setMagDataRate(ak09916_data_rate_t::AK09916_MAG_DATARATE_50_HZ);

  this->icm_accel = icm.getAccelerometerSensor();
  this->icm_gyro = icm.getGyroSensor();
  this->icm_mag = icm.getMagnetometerSensor();
  this->icm_temp = icm.getTemperatureSensor();

  return this->icm_accel != nullptr && this->icm_gyro != nullptr &&
         this->icm_mag != nullptr && this->icm_temp != nullptr;
}

void ICM20948Sensor::readEvents() {
  if (this->dummy_) {
    const float t = (millis() % 1000000UL) / 1000.0f;
    this->last_accel_[0] = 0.05f * sinf(t * 1.3f);
    this->last_accel_[1] = 0.03f * cosf(t * 0.7f);
    this->last_accel_[2] = 9.80665f + 0.02f * sinf(t * 2.1f);
    this->last_gyro_[0] = 0.002f * sinf(t * 0.9f);
    this->last_gyro_[1] = 0.003f * cosf(t * 1.1f);
    this->last_gyro_[2] = 0.001f * sinf(t * 0.4f);
    this->last_mag_[0] = 21.0f + 1.5f * sinf(t * 0.05f);
    this->last_mag_[1] = -4.0f + 1.0f * cosf(t * 0.07f);
    this->last_mag_[2] = 43.0f + 2.0f * sinf(t * 0.03f);
    this->last_temp_c_ = 25.0f + 0.4f * sinf(t * 0.01f);
    this->has_sample_ = true;
    return;
  }

  sensors_event_t accel;
  sensors_event_t gyro;
  sensors_event_t temp;
  sensors_event_t mag;
  this->icm_temp->getEvent(&temp);
  this->icm_accel->getEvent(&accel);
  this->icm_gyro->getEvent(&gyro);
  this->icm_mag->getEvent(&mag);

  this->last_accel_[0] = accel.acceleration.x;
  this->last_accel_[1] = accel.acceleration.y;
  this->last_accel_[2] = accel.acceleration.z;
  this->last_gyro_[0] = gyro.gyro.x;
  this->last_gyro_[1] = gyro.gyro.y;
  this->last_gyro_[2] = gyro.gyro.z;
  this->last_mag_[0] = mag.magnetic.x;
  this->last_mag_[1] = mag.magnetic.y;
  this->last_mag_[2] = mag.magnetic.z;
  this->last_temp_c_ = temp.temperature;
  this->has_sample_ = true;
}

/**
 * @brief Retrieves data from ICM 9-axis IMU
 *
 * @return String A CSV section in format AccX, AccY, AccZ, GyroX, GyroY, GyroZ,
 * MagX, MagY, MagZ, TempC
 */
String ICM20948Sensor::readData() {
  this->readEvents();

  return String(this->last_accel_[0]) + "," + String(this->last_accel_[1]) +
         "," + String(this->last_accel_[2]) + "," + String(this->last_gyro_[0]) +
         "," + String(this->last_gyro_[1]) + "," + String(this->last_gyro_[2]) +
         "," + String(this->last_mag_[0]) + "," + String(this->last_mag_[1]) +
         "," + String(this->last_mag_[2]) + "," + String(this->last_temp_c_) +
         ",";
}

/**
 * @brief Append the ICM sensor data to the packet buffer as raw bytes.
 * The data appended is: accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z,
 * mag.x, mag.y, mag.z, and temperature.
 *
 * @param packet Pointer to the packet byte array which is incremented after
 * copying each value.
 */
void ICM20948Sensor::readDataPacket(uint8_t*& packet) {
  this->readEvents();

  const float values[10] = {this->last_accel_[0], this->last_accel_[1],
                            this->last_accel_[2], this->last_gyro_[0],
                            this->last_gyro_[1],  this->last_gyro_[2],
                            this->last_mag_[0],   this->last_mag_[1],
                            this->last_mag_[2],   this->last_temp_c_};
  memcpy(packet, values, sizeof(values));
  packet += sizeof(values);
}

/**
 * @brief Decodes the ICM sensor data from the packet buffer into a CSV string.
 * Reads the data in the same order as it was appended and increments the packet
 * pointer.
 *
 * @param packet - Pointer to the packet byte array which is incremented after
 * extracting each value.
 * @return String A CSV string of the sensor data.
 */
String ICM20948Sensor::decodeToCSV(uint8_t*& packet) {
  // Read values from the packet buffer by casting the pointer to a float

  const size_t vals_len = 10;
  String csv_row;

  for (size_t i = 0; i < vals_len; i++) {
    float temp;
    memcpy(&temp, packet, sizeof(float));
    packet += sizeof(float);

    csv_row += String(temp) + ",";
  }
  return csv_row;
}
