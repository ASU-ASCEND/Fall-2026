#ifndef SYS_VAR_H
#define SYS_VAR_H

#include <Arduino.h>
#include <drivers/BMESensor.h>
#include <drivers/GPSSensor.h>
#include <drivers/INASensor.h>

// When true the main loop streams raw binary packets on USB serial instead of
// the human readable CSV line. Toggled at runtime with 'B' / 'T' on serial.
extern volatile bool pnc_binary_mode;

// setup
void sysvar_init();

struct SysVarFreshness {
  uint32_t pico_ms, rtc_ms, ina_ms, bme_ms, gps_ms;
  uint8_t valid;
};
int8_t sysvar_get_freshness(SysVarFreshness* output);

// Access functions
int8_t sysvar_get_pico_temp_c(float* output);
int8_t sysvar_set_pico_temp_c(float input);

int8_t sysvar_get_rtc_time(uint32_t* output);
int8_t sysvar_set_rtc_time(uint32_t input);

int8_t sysvar_set_ina_data(INASensorData* ina_sensor_data);
int8_t sysvar_get_ina_data(INASensorData* ina_sensor_data);

int8_t sysvar_set_bme_data(BMESensorData* sensor_data);
int8_t sysvar_get_bme_data(BMESensorData* sensor_data);

int8_t sysvar_set_gps_data(GPSSensorData* sensor_data);
int8_t sysvar_get_gps_data(GPSSensorData* sensor_data);

#endif  // SYS_VAR_H
