#include "drivers/GPSSensor.h"

#include <Wire.h>

#include "SysHead.h"

GPSSensor::GPSSensor() : Sensor("GPS") {}

GPSSensorData GPSSensorData::fromGnss(SFE_UBLOX_GNSS& gnss) {
  GPSSensorData sensor_data;
  sensor_data.unix_time_s = gnss.getUnixEpoch();
  sensor_data.fix_type = gnss.getFixType();
  sensor_data.fix_ok = gnss.getGnssFixOk();
  sensor_data.siv = gnss.getSIV();

  // Position/velocity are only meaningful with a valid fix. Keeping them zero
  // when there is no fix means the packet can still be decoded and the GPS can
  // be verified on a bench with no sky view.
  if (gnss.getInvalidLlh() || sensor_data.fix_type == 0) {
    sensor_data.lat_e7 = 0;
    sensor_data.lon_e7 = 0;
    sensor_data.alt_msl_mm = 0;
    sensor_data.vel_n_mmps = 0;
    sensor_data.vel_e_mmps = 0;
    sensor_data.vel_d_mmps = 0;
    sensor_data.hacc_mm = 0;
    sensor_data.vacc_mm = 0;
  } else {
    sensor_data.lat_e7 = gnss.getLatitude();
    sensor_data.lon_e7 = gnss.getLongitude();
    sensor_data.alt_msl_mm = gnss.getAltitudeMSL();
    sensor_data.vel_n_mmps = gnss.getNedNorthVel();
    sensor_data.vel_e_mmps = gnss.getNedEastVel();
    sensor_data.vel_d_mmps = gnss.getNedDownVel();
    sensor_data.hacc_mm = gnss.getHorizontalAccEst();
    sensor_data.vacc_mm = gnss.getVerticalAccEst();
  }

  return sensor_data;
}

bool GPSSensor::tryInit() {
  // NEO-M9N is wired to I2C0 (GP4/GP5) on JGPS1, default address 0x42.
  if (gnss.begin(Wire, GPS_I2C_ADDR, 250) == false) {
    return false;
  }

  // Keep I2C output clean and read compact UBX packets.
  gnss.setI2COutput(COM_TYPE_UBX);
  gnss.setAutoPVT(true);
  gnss.setDynamicModel(DYN_MODEL_AIRBORNE4g);
  return true;
}

bool GPSSensor::retryInit() { return tryInit(); }

bool GPSSensor::verify() {
  // The NEO-M9N boots slower than the Pico after a cold power cycle. Retry for
  // a few seconds instead of permanently marking the GPS missing on the first
  // failed begin(). 10 x (250 ms poll + 500 ms wait) is about 7.5 s.
  for (int attempt = 0; attempt < 10; attempt++) {
    if (tryInit()) {
      // One explicit read so the first packet already has the receiver state
      // (fix type / satellite count) instead of all zeros.
      readToSysVar();
      return true;
    }
    delay(500);
  }
  return false;
}

void GPSSensor::readToSysVar() {
  if (gnss.getPVT() == false) {
    return;
  }

  GPSSensorData sensor_data = GPSSensorData::fromGnss(gnss);

  sysvar_set_gps_data(&sensor_data);
}
