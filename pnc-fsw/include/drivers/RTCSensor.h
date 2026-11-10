#ifndef RTC_SENSOR_H
#define RTC_SENSOR_H

#include "RTClib.h"
#include "Sensor.h"

/**
 * @brief Implementation of a Sensor for the real time clock
 *
 */
class RTCSensor : public Sensor {
 private:
  RTC_PCF8523 rtc;

 public:
  RTCSensor();
  bool verify() override;
  void readToSysVar() override;

  /** @brief Set the RTC from a Unix timestamp and clear the oscillator-stop flag. */
  bool setTime(uint32_t unix_time);

  /** @brief Current RTC time as a Unix timestamp. */
  uint32_t nowUnix();

  /** @brief True if the PCF8523 lost oscillator power (OSF set). */
  bool lostPower();
};

#endif
