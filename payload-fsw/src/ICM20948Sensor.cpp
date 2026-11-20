#include "ICM20948Sensor.h"

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
    return false;
#endif
  }

  if (addr == 0xFF) {
    // Do not write the shared 0x68 address to probe the IMU: that would also
    // modify the disabled RTC while it remains electrically connected.
    return false;
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
