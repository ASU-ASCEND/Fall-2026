#include "DummySensor.h"

#include <math.h>

DummySensor::DummySensor() : DummySensor(500, 16) {}

DummySensor::DummySensor(unsigned long minimum_period, uint8_t channels)
    : Sensor("DummyExternal", "Dummy,", minimum_period) {
  if (channels > 16) channels = 16;
  this->channels_ = channels;
}

bool DummySensor::verify() { return true; }

void DummySensor::generate() {
  const float t = (millis() % 1000000UL) / 1000.0f;
  for (uint8_t i = 0; i < this->channels_; i++) {
    // Each channel gets a different frequency and amplitude so a log stands
    // out from a flat line.
    this->last_[i] =
        sinf(t * (0.05f + 0.017f * i)) * (float)(i + 1) +
        (float)(this->counter_ % 1000) * 0.001f;
  }
  this->counter_++;
}

String DummySensor::readData() {
  this->generate();
  String row;
  for (uint8_t i = 0; i < this->channels_; i++) {
    row += String(this->last_[i]) + ",";
  }
  return row;
}

void DummySensor::readDataPacket(uint8_t*& packet) {
  this->generate();
  const size_t bytes = this->channels_ * sizeof(float);
  memcpy(packet, this->last_, bytes);
  packet += bytes;
}

String DummySensor::decodeToCSV(uint8_t*& packet) {
  String row;
  for (uint8_t i = 0; i < this->channels_; i++) {
    float value;
    memcpy(&value, packet, sizeof(value));
    packet += sizeof(value);
    row += String(value) + ",";
  }
  return row;
}
