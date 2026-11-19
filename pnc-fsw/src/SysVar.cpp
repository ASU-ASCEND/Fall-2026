#include "SysVar.h"

#include "pico/mutex.h"

#define SYSVAR_ACCESS_DELAY 100

volatile bool pnc_binary_mode = false;

// mutex
static mutex_t sysvar_mutex;

void sysvar_init() { mutex_init(&sysvar_mutex); }

// System Variables
static float pico_temp_c;
static uint32_t rtc_time;
static INASensorData ina_data;
static BMESensorData bme_data;
static GPSSensorData gps_data;
static SysVarFreshness freshness = {};

// Access functions
#define with_sysvar_mutex(operation)                                   \
  if (mutex_try_enter_block_until(                                     \
          &sysvar_mutex, make_timeout_time_ms(SYSVAR_ACCESS_DELAY))) { \
    operation;                                                         \
    mutex_exit(&sysvar_mutex);                                         \
    return 0;                                                          \
  } else {                                                             \
    return -1;                                                         \
  }

int8_t sysvar_get_pico_temp_c(float* output) {
  with_sysvar_mutex(*output = pico_temp_c);
}

int8_t sysvar_set_pico_temp_c(float input) {
  with_sysvar_mutex(pico_temp_c = input; freshness.pico_ms = millis(); freshness.valid |= 1);
}

int8_t sysvar_get_rtc_time(uint32_t* output) {
  with_sysvar_mutex(*output = rtc_time);
}

int8_t sysvar_set_rtc_time(uint32_t input) {
  with_sysvar_mutex(rtc_time = input; freshness.rtc_ms = millis(); freshness.valid |= 2);
}

int8_t sysvar_set_ina_data(INASensorData* ina_sensor_data) {
  with_sysvar_mutex(ina_data = *ina_sensor_data; freshness.ina_ms = millis(); freshness.valid |= 4);
}

int8_t sysvar_get_ina_data(INASensorData* ina_sensor_data) {
  with_sysvar_mutex(*ina_sensor_data = ina_data);
}

int8_t sysvar_set_bme_data(BMESensorData* sensor_data) {
  with_sysvar_mutex(bme_data = *sensor_data; freshness.bme_ms = millis(); freshness.valid |= 8);
}

int8_t sysvar_get_bme_data(BMESensorData* sensor_data) {
  with_sysvar_mutex(*sensor_data = bme_data);
}

int8_t sysvar_set_gps_data(GPSSensorData* sensor_data) {
  with_sysvar_mutex(gps_data = *sensor_data; freshness.gps_ms = millis(); freshness.valid |= 16);
}

int8_t sysvar_get_gps_data(GPSSensorData* sensor_data) {
  with_sysvar_mutex(*sensor_data = gps_data);
}

int8_t sysvar_get_freshness(SysVarFreshness* output) {
  with_sysvar_mutex(*output = freshness);
}
