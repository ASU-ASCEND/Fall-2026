#include "drivers/BMESensor.h"

#include "SysHead.h"
#include "Wire.h"

BMESensor::BMESensor() : Sensor("BME") { bme = Adafruit_BME680(&Wire); }

// BME688 on the PnC has CSB tied to 3V3 (I2C mode) and SDO tied to GND,
// so the address is 0x76.
bool BMESensor::verify() { return bme.begin(BME_I2C_ADDR); }

void BMESensor::readToSysVar() {
  if (!bme.performReading()) {
    return;
  }

  BMESensorData sensor_data;
  sensor_data.BMETemp = bme.temperature;
  sensor_data.BMEPressure = (uint32_t)bme.pressure;
  sensor_data.BMEHumidity = bme.humidity;
  sensor_data.BMEGasResistance = (uint32_t)bme.gas_resistance;

  sysvar_set_bme_data(&sensor_data);
}
