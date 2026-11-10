#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

/**
 * @file HardwareConfig.h
 * @brief PnC rev Fall-2026 pin map and device addresses.
 *
 * Values below are taken from the Altium project-wide netlist generated with
 * altium-monkey (PCBs/tools/extract_netlists.py), not from guesswork:
 *   PCBs/netlists/ASCEND_Fall26_Power&Control.json
 *
 * Keep every pin macro containing "PIN" so scripts/check_pins.py can detect
 * conflicts.
 */

// Error LED header JERR1 (pin 1 is GND, LEDs are driven high)
#define ERROR_0_PIN 20
#define ERROR_1_PIN 21
#define ERROR_2_PIN 22

// I2C0: BME688 U11, INA260 U2, PCF8523 U6, GPS JGPS1 pins 7/8
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5

// SPI1: microSD J1 and MCP2515 U8 share the bus, separate chip selects
#define SD_MISO_PIN 8
#define SD_SCK_PIN 10
#define SD_MOSI_PIN 11
#define SD_CS_PIN 17

#define CAN_CS_PIN 9
#define CAN_INT_PIN 12
#define CAN_RESET_PIN 7

// ATtiny85 U7 heartbeat input (PB1). U7 drives the Pico RUN pin through Q3,
// so this must keep toggling or the external watchdog resets the Pico.
#define WATCHDOG_HEARTBEAT_PIN 6

// I2C addresses as strapped on the PnC board
#define BME_I2C_ADDR 0x76  // BME688 CSB=3V3, SDO=GND
#define INA_I2C_ADDR 0x40  // INA260 A0=A1=GND
#define GPS_I2C_ADDR 0x42  // u-blox NEO-M9N default

// max resolution for the rp2350's ADC
#define PICO_TEMP_ADC_RES 12

#endif  // HARDWARE_CONFIG_H
