#include "GeigerSensor.h"

GeigerSensor::GeigerSensor() : GeigerSensor(1000) {}

GeigerSensor::GeigerSensor(unsigned long minimum_period)
    : Sensor("GeigerCounter", "CPS,Dose,", minimum_period) {}

bool GeigerSensor::verify() {
  this->gc.begin(GEIGER_PIN, 1000);  // using enforced minimum of 1000 ms

  return true;
}

String GeigerSensor::readData() {
  this->last_cps_ = this->gc.getCPSRunning();
  this->last_dose_ = this->gc.getDoseRunning();
  this->has_sample_ = true;
  return String(this->last_cps_) + "," + String(this->last_dose_) + ",";
}

void GeigerSensor::readDataPacket(uint8_t*& packet) {
  this->last_cps_ = this->gc.getCPSRunning();
  this->last_dose_ = this->gc.getDoseRunning();
  this->has_sample_ = true;

  memcpy(packet, &this->last_cps_, sizeof(float));
  packet += sizeof(float);
  memcpy(packet, &this->last_dose_, sizeof(float));
  packet += sizeof(float);
}

String GeigerSensor::decodeToCSV(uint8_t*& packet) {
  float cps, dose;
  memcpy(&cps, packet, sizeof(float));
  packet += sizeof(float);
  memcpy(&dose, packet, sizeof(float));
  packet += sizeof(float);
  return String(cps) + "," + String(dose) + ",";
}
