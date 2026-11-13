#ifndef EXTERNAL_TEST_H
#define EXTERNAL_TEST_H

#include <stdint.h>

/**
 * @file ExternalTest.h
 * @brief Interactive test for the External sensor board (I2C1).
 *
 * Probes every device that the Fall 2026 External board carries with raw
 * register reads (so a wrong driver address cannot hide a working part), then
 * runs each vendor driver once and prints a live reading.
 *
 * Devices and expected IDs, from the External netlist + datasheets:
 *
 *   0x48  TMP117   reg 0x0F -> 0x0117
 *   0x53  ENS160   reg 0x00 -> 0x0160 (ADDR strapped high through R1)
 *   0x70  SHTC3    command 0xEFC8 -> 0x0807
 *   0x76  BMP390   reg 0x00 -> 0x60   (SDO/SA0 tied to GND)
 *   0x77  BME688   reg 0xD0 -> 0x61   (SDO tied to 3V3)
 *   0x73  OZONE    no ID register; DFRobot Gravity (SEN0321)
 *   0x74  AS7331   no ID register; A1/A0 straps select 0x74..0x77
 */
void external_test();

/**
 * @brief Keep reading the External board for a while.
 *
 * The ENS160 reports valid AQI/TVOC/ECO2 only after about a minute in standard
 * mode, so one pass is not enough to judge it.
 *
 * @param samples   number of passes
 * @param period_s  seconds between passes
 */
void external_watch(uint8_t samples, uint8_t period_s);

#endif  // EXTERNAL_TEST_H
