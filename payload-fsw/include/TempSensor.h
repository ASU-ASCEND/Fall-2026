#ifndef TEMP_SENSOR_H
#define TEMP_SENSOR_H

#include "Sensor.h"

/**
 * @brief Implementation of a Sensor for the onboard temperature sensor
 *
 */
class TempSensor : public Sensor {
 private:
  float last_temp_c_ = 0;
  bool has_sample_ = false;

 public:
  TempSensor();
  TempSensor(unsigned long minium_period);
  bool verify() override;
  String readData() override;
  void readDataPacket(uint8_t*& packet) override;
  String decodeToCSV(uint8_t*& packet) override;

  bool hasLastSample() const { return this->has_sample_; }
  float lastTempC() const { return this->last_temp_c_; }
};

#endif
