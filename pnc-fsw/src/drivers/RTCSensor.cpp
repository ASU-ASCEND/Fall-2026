#include "drivers/RTCSensor.h"

#include "SysHead.h"

RTCSensor::RTCSensor() : Sensor("RTC") {}

bool RTCSensor::verify() {
  if (this->rtc.begin() == false) return false;

  // With a healthy coin cell this is false after the first setTime(). If it
  // stays true, the cell is missing, flat, or not making contact.
  if (this->rtc.lostPower()) {
    log_task("RTC lost power (time invalid; set it with S<unix>).");
  }

  this->rtc.start();
  return true;
}

void RTCSensor::readToSysVar() {
  DateTime now = rtc.now();

  // Without a valid set time the PCF8523 can report an arbitrary date, and a
  // failed I2C read can return nonsense. Keep the last good value instead of
  // writing a bad timestamp into packets.
  if (now.year() < 2020 || now.year() > 2100) {
    return;
  }

  sysvar_set_rtc_time(now.unixtime());
}

bool RTCSensor::setTime(uint32_t unix_time) {
  // adjust() also writes Control_3 = 0, which clears OSF and enables battery
  // switchover on the PCF8523.
  rtc.adjust(DateTime((time_t)unix_time));
  rtc.start();

  DateTime now = rtc.now();
  int32_t delta = (int32_t)(now.unixtime() - unix_time);
  return delta >= -2 && delta <= 2;
}

uint32_t RTCSensor::nowUnix() { return rtc.now().unixtime(); }

bool RTCSensor::lostPower() { return rtc.lostPower(); }
