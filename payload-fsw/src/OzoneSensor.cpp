#include "OzoneSensor.h"

/**
 * @brief Construct a new Ozone Sensor object with default minimum_period of 0
 * ms
 *
 */
OzoneSensor::OzoneSensor(TwoWire* i2c_bus) : OzoneSensor(0, i2c_bus) {}

/**
 * @brief Construct a new Ozone Sensor object
 *
 * @param minium_period Minimum time to wait between readings in ms
 */
OzoneSensor::OzoneSensor(unsigned long minium_period, TwoWire* i2c_bus)
    : Sensor("OzoneSensor", "O3 PPB, ", minium_period), ozone(i2c_bus) {
  this->i2c_bus = i2c_bus;
}

/**
 * @brief Tests if the Ozone sensor can be reached
 *
 * @return true
 * @return false
 */
bool OzoneSensor::verify() {
  if (this->ozone.begin(OZONE_I2C_ADDR) == false) {
    return false;
  }

  ozone.setModes(MEASURE_MODE_PASSIVE);

  return true;
}

/**
 * @brief Reads ozone concentration data in PPB
 *
 * @return String CSV stub of O3 PPB
 */
String OzoneSensor::readData() {
  int16_t value;
  return readOzone(value) ? String(value) + "," : this->readEmpty();
}

bool OzoneSensor::readOzone(int16_t& value) {
  i2c_bus->beginTransmission(OZONE_I2C_ADDR);
  i2c_bus->write(SET_PASSIVE_REGISTER);
  i2c_bus->write(PASSIVE_READ_DATA);
  if (i2c_bus->endTransmission() != 0) return false;
  delay(100);
  i2c_bus->beginTransmission(OZONE_I2C_ADDR);
  i2c_bus->write(PASS_DATA_HIGE_REGISTER);
  if (i2c_bus->endTransmission() != 0) return false;
  delay(100);
  if (i2c_bus->requestFrom((int)OZONE_I2C_ADDR, 2) != 2) return false;
  uint16_t raw = (uint16_t)i2c_bus->read() << 8;
  raw |= (uint8_t)i2c_bus->read();
  value = (int16_t)raw;
  return value >= 0;
}

/**
 * @brief Reads ozone concentration data in PPB to packet
 *
 * @param packet The packet to copy data into
 */
void OzoneSensor::readDataPacket(uint8_t*& packet) {
  int16_t ozone_val;
  if (!readOzone(ozone_val)) return;
  memcpy(packet, &ozone_val, sizeof(int16_t));
  packet += sizeof(int16_t);
}

/**
 * @brief Decodes ozone concentration data from a packet to CSV
 *
 * @param packet The packet to decode from
 * @return String CSV stub of O3 PPB
 */
String OzoneSensor::decodeToCSV(uint8_t*& packet) {
  int16_t ozone_val = 0;
  memcpy(&ozone_val, packet, sizeof(int16_t));
  packet += sizeof(int16_t);

  return String(ozone_val) + ",";
}
