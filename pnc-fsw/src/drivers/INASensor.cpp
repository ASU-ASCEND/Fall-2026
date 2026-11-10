#include "drivers/INASensor.h"

#include "SysHead.h"
#include "Wire.h"

INASensor::INASensor() : Sensor("INA") {}

/**
 * @brief Verifies the connection to the INA sensor.
 *
 * INA260 A0 and A1 are both tied to GND on the PnC board, giving address 0x40.
 *
 * @return true If the sensor is successfully initialized.
 * @return false If the sensor fails to initialize or is not detected.
 */
bool INASensor::verify() {
  ina = Adafruit_INA260();
  return ina.begin(INA_I2C_ADDR);
}

void INASensor::readToSysVar() {
  INASensorData ina_data;

  ina_data.INACurrent = ina.readCurrent();
  ina_data.INABusVoltage = ina.readBusVoltage();
  ina_data.INAPower = ina.readPower();

  // A failed I2C transaction on the shared bus can return NAN or nonsense.
  // Drop the sample rather than publishing it.
  if (!isfinite(ina_data.INACurrent) || !isfinite(ina_data.INABusVoltage) ||
      !isfinite(ina_data.INAPower)) {
    return;
  }
  if (ina_data.INABusVoltage < 0 || ina_data.INABusVoltage > 30000) {
    return;
  }
  if (fabsf(ina_data.INACurrent) > 5000) {
    return;
  }
  if (ina_data.INAPower < 0 || ina_data.INAPower > 30000) {
    return;
  }

  sysvar_set_ina_data(&ina_data);
}
