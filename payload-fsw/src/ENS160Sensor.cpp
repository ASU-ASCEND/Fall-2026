#include "ENS160Sensor.h"

#include "SHTC3Sensor.h"
#include "TMP11xSensor.h"

#if PAYLOAD_ENABLE_EXTERNAL
// Both live on the External board's I2C bus and are compiled in with it.
extern TMP11xSensor tmp_sensor_out;
extern SHTC3Sensor shtc3_sensor_out;
#endif

/**
 * @brief Construct a new ENS160 Sensor object with default minimum_period of 0
 * ms
 *
 */
ENS160Sensor::ENS160Sensor(TwoWire* i2c_bus, uint8_t i2c_addr)
    : ENS160Sensor(0, i2c_bus, i2c_addr) {}

/**
 * @brief Construct a new ENS160 Sensor object
 *
 * @param minium_period Minimum time to wait between readings in ms
 */
ENS160Sensor::ENS160Sensor(unsigned long minium_period, TwoWire* i2c_bus,
                           uint8_t i2c_addr)
    : Sensor("ENS160", "ENSAQI,ENSTVOC ppb,ENSECO2 ppm,", 3, minium_period) {
  this->i2c_bus = i2c_bus;
  if (this->i2c_bus == &Wire1) this->device_name += "_1";
  this->i2c_addr = i2c_addr;
}

/**
 * @brief Returns if the ENS160 sensor can be reached
 *
 * @return true if the ENS is connected
 * @return false if the ENS isn't connected
 */
bool ENS160Sensor::verify() {
  this->i2c_bus->begin();

  if (!ens.begin(*(this->i2c_bus), this->i2c_addr)) {
    return false;
  }

  ens.setOperatingMode(SFE_ENS160_STANDARD);

  return true;
}

void ENS160Sensor::setCompensations() {
  // Feed the ENS160 the temperature and humidity it uses for its gas
  // calculations. Both references are the External-board parts; the internal
  // board has no TMP117 or SHTC3.
#if PAYLOAD_ENABLE_EXTERNAL
  if (tmp_sensor_out.getVerified()) {
    this->ens.setTempCompensation(tmp_sensor_out.getTempC());
  }
  if (shtc3_sensor_out.getVerified()) {
    this->ens.setRHCompensationFloat(shtc3_sensor_out.getRelHum());
  }
#endif
}

/**
 * @brief Reads Air Quality Index, TVOC (ppb), and ECOS (ppm)
 *
 * @return String AQI, TVOC, ECOS data in CSV format
 */
String ENS160Sensor::readData() {
  if (ens.checkDataStatus()) {
    return String(ens.getAQI()) + "," + ens.getTVOC() + "," + ens.getECO2() +
           ",";
  } else {
    return this->readEmpty();
  }

  this->setCompensations();
}

void ENS160Sensor::readDataPacket(uint8_t*& packet) {
  if (!ens.checkDataStatus()) {
    return;
  }
  uint8_t aqi = ens.getAQI();
  uint16_t tvoc = ens.getTVOC();
  uint16_t eco2 = ens.getECO2();

  memcpy(packet, &aqi, sizeof(aqi));
  packet += sizeof(aqi);

  memcpy(packet, &tvoc, sizeof(tvoc));
  packet += sizeof(tvoc);

  memcpy(packet, &eco2, sizeof(eco2));
  packet += sizeof(eco2);

  this->setCompensations();
}

String ENS160Sensor::decodeToCSV(uint8_t*& packet) {
  uint8_t aqi = *packet;
  packet += sizeof(uint8_t);

  uint16_t tvoc;
  memcpy(&tvoc, packet, sizeof(uint16_t));
  packet += sizeof(uint16_t);

  uint16_t eco2;
  memcpy(&eco2, packet, sizeof(uint16_t));
  packet += sizeof(uint16_t);

  return String(aqi) + "," + String(tvoc) + "," + String(eco2) + ",";
}