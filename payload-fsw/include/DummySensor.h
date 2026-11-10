#ifndef DUMMY_SENSOR_H
#define DUMMY_SENSOR_H

#include "Sensor.h"

/**
 * @brief Synthetic multi-channel sensor used to exercise the CAN packet path
 * before the StratoSense (external) and Science boards are connected.
 *
 * It always verifies and emits `channels` floats. Set
 * PAYLOAD_ENABLE_DUMMY_EXTERNAL to 0 when the real sensors are wired.
 */
class DummySensor : public Sensor {
 private:
  uint8_t channels_ = 16;
  float last_[16] = {};
  uint32_t counter_ = 0;

  void generate();

 public:
  DummySensor();
  DummySensor(unsigned long minimum_period, uint8_t channels);

  bool verify() override;
  String readData() override;
  void readDataPacket(uint8_t*& packet) override;
  String decodeToCSV(uint8_t*& packet) override;
};

#endif  // DUMMY_SENSOR_H
