#ifndef AS7331SENSOR_H
#define AS7331SENSOR_H

#include <SparkFun_AS7331.h>
#include <Wire.h>
#include <string.h>

#include "PayloadConfig.h"
#include "Sensor.h"

/**
 * @brief Implementation of a Sensor for UVA/B/C sensor
 *
 */
class AS7331Sensor : public Sensor {
 private:
  SfeAS7331ArdI2C myUVSensor;
  uint8_t i2c_addr;
  TwoWire* i2c_bus;

 public:
  AS7331Sensor(uint8_t i2c_addr, TwoWire* bus = &STRATOSENSE_I2C);
  AS7331Sensor(unsigned long minimum_period, uint8_t i2c_addr,
               TwoWire* bus = &STRATOSENSE_I2C);

  bool verify() override;
  void readDataPacket(uint8_t*& packet) override;
  String decodeToCSV(uint8_t*& packet) override;
  String readData() override;
};

#endif
