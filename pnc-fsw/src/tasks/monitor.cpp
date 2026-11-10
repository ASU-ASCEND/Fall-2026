#include "tasks/monitor.h"

#include <drivers/BMESensor.h>
#include <drivers/GPSSensor.h>
#include <drivers/INASensor.h>

#include "SysHead.h"
#include "tasks/watchdog.h"

void monitor_task() {
  watchdog_intertask_update(WATCHDOG_MONITOR_TASK_ID);

  // Binary packet mode needs a clean serial stream.
  if (pnc_binary_mode) {
    return;
  }

  float pico_temp_c = 0;
  uint32_t rtc_time = 0;
  BMESensorData bme_data = {};
  INASensorData ina_data = {};
  GPSSensorData gps_data = {};

  sysvar_get_pico_temp_c(&pico_temp_c);
  sysvar_get_rtc_time(&rtc_time);
  sysvar_get_bme_data(&bme_data);
  sysvar_get_ina_data(&ina_data);
  sysvar_get_gps_data(&gps_data);

  log_task_printf(
      "MON pico=%.2fC rtc=%lu bme=%.2fC/%luPa/%.2f%%/%luohm "
      "ina=%.2fmA/%.2fmV/%.2fmW gps=fix%u/ok%u/siv%u lat=%ld lon=%ld alt=%ldmm\n",
      pico_temp_c, (unsigned long)rtc_time, bme_data.BMETemp,
      (unsigned long)bme_data.BMEPressure, bme_data.BMEHumidity,
      (unsigned long)bme_data.BMEGasResistance, ina_data.INACurrent,
      ina_data.INABusVoltage, ina_data.INAPower, gps_data.fix_type,
      gps_data.fix_ok, gps_data.siv, gps_data.lat_e7, gps_data.lon_e7,
      gps_data.alt_msl_mm);
}

void monitor_task_init() {
  // do task setup
  log_task("Monitor task started.");
}
