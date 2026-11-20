#ifndef GEIGER_SENSOR_H
#define GEIGER_SENSOR_H

#include "PayloadConfig.h"

#if PAYLOAD_ENABLE_GEIGER

#include "GeigerCounter.h"
#include "Sensor.h"

class GeigerSensor : public Sensor {
 private:
  GeigerCounter gc;
  float last_cps_ = 0;
  float last_dose_ = 0;
  bool has_sample_ = false;

 public:
  GeigerSensor();
  GeigerSensor(unsigned long minimum_period);
  bool verify() override;
  String readData() override;
  void readDataPacket(uint8_t*& packet) override;
  String decodeToCSV(uint8_t*& packet) override;

  bool hasLastSample() const { return this->has_sample_; }
  float lastCPS() const { return this->last_cps_; }
  float lastDose() const { return this->last_dose_; }
};

#endif  // PAYLOAD_ENABLE_GEIGER

#endif
