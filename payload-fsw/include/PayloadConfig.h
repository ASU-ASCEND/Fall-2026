/**
 * @file PayloadConfig.h
 * @brief Central location for pinout and config defines
 *
 * Values are taken from the Altium project netlist generated with
 * altium-monkey (PCBs/tools/extract_netlists.py):
 *   PCBs/netlists/ASCEND_FA26_Internal_Board.json
 *
 * All pin definitions must include PIN in their name to allow the conflict
 * checker to catch pin conflits ex: SD_CS_PIN instead of just SD_CS
 */

#ifndef PAYLOAD_CONFIG_H
#define PAYLOAD_CONFIG_H

#include <Wire.h>

/**
 * @brief Error display pins.
 *
 * The FA26 Internal board does not route these to LEDs (D300/D301/D400/D500/
 * D600/D601 are power indicators). Keep the macros for the shared framework,
 * but do not drive the pins unless a board revision adds the LEDs.
 */
#define ERROR_PIN_2 2
#define ERROR_PIN_1 1
#define ERROR_PIN_0 0
#define PAYLOAD_ERROR_LEDS_WIRED 0

/** @brief UV sensor I2C Addresses (StratoSense board) */
// [1, 1, 1, 0, 1, A1, A0]
#define UV_I2C_ADDR 0x74

/** @brief Ozone I2C address (StratoSense board) */
#define OZONE_I2C_ADDR 0x73

/** @brief Geiger interrupt pin (JGeig100 pin 2) */
#define GEIGER_PIN 22

/** @brief I2C1 pins: StratoSense / external sensor bus (J200/J201) */
#define I2C1_SDA_PIN 6
#define I2C1_SCL_PIN 7

/** @brief I2C0 pins: PCF8523 RTC and ICM-20948 IMU (J202/J203) */
#define I2C0_SDA_PIN 4
#define I2C0_SCL_PIN 5

/** @brief Default I2C Address for TMP117 (StratoSense board) */
#define TMP117_I2C_ADDR 0x48
/** @brief Default I2C Address for BME688 (StratoSense board) */
#define BME688_I2C_ADDR 0x77
/** @brief PCF8523 RTC address (fixed) */
#define RTC_I2C_ADDR 0x68
/**
 * @brief ICM-20948 address.
 *
 * The FA26 Internal board netlist ties AD0/SDO (U600 pin 9) to GND, which puts
 * the IMU at 0x68. The PCF8523 RTC is also fixed at 0x68 on the same bus, so
 * the two devices collide. Until AD0 is reworked to 3V3 (address 0x69), the
 * firmware must not initialise the IMU: writes would also hit the RTC.
 *
 * Set to 0 only after the AD0 rework.
 */
#define ICM20948_ADDR_0X68_PRESENT 0
#define ICM20948_I2C_ADDR 0x68

/** @brief ADC Pin for Thermistor Readings */
#define THERMISTOR_PIN 28

/** @brief StratoSense board I2C Bus */
#define STRATOSENSE_I2C Wire1

/** @brief StratoCore board I2C Bus */
#define STRATOCORE_I2C Wire

// storages
/** @brief Toggle sending packets vs C strings over transfer queue */
#define STORING_PACKETS 1

// SPI1: MCP2515 CAN controller only
/** @brief SPI1 MISO Pin (MCP2515 SO) */
#define CAN_MISO_PIN 12
/** @brief SPI1 SCK Pin (MCP2515 SCK) */
#define CAN_SCK_PIN 14
/** @brief SPI1 MOSI Pin (MCP2515 SI) */
#define CAN_MOSI_PIN 15
/** @brief MCP2515 chip select (10k pull-up R304) */
#define CAN_CS_PIN 13
/** @brief MCP2515 INT, through R104 (0R) */
#define CAN_INT_PIN 11
/** @brief MCP2515 RESET, through R103 (0R) */
#define CAN_RESET_PIN 10
/** @brief TJA1051 S (standby), through R105 (0R) */
#define CAN_STBY_PIN 9

// SPI0: microSD on the Pico's default SPI0 pins (J400)
/** @brief SD Card SPI Toggle: 0 = SPI0 (board wiring) */
#define SD_SPI1 0
/** @brief SD Card SPI CS Pin */
#define SD_CS_PIN 17
/** @brief SD MISO Pin */
#define SD_MISO_PIN 16
/** @brief SD SCK Pin */
#define SD_SCK_PIN 18
/** @brief SD MOSI Pin */
#define SD_MOSI_PIN 19

// main pin definitions
/** @brief Built-in LED Pin */
#define ON_BOARD_LED_PIN 25
/** @brief Core 0 Heartbeat Pin */
#define HEARTBEAT_PIN_0 21
/** @brief Core 1 Heartbeat Pin */
#define HEARTBEAT_PIN_1 20

// multicore transfer queue
#define QT_ENTRY_SIZE 500
#define QT_MAX_SIZE 10

// packet properties
const uint8_t SYNC_BYTES[] = {'A', 'S', 'U', '!'};

/**
 * @brief Sensor set compiled into this build.
 *
 * The FA26 Internal board carries the Pico, PCF8523 RTC, ICM-20948 IMU, the
 * MCP2515/TJA1051 CAN pair, the microSD socket and the Geiger input. The
 * External sensor board plugs into J200; its parts are enabled below when they
 * pass verification.
 */
/**
 * @brief External sensor board (J200) — the four devices that work today.
 *
 * TMP117, ENS160, SHTC3 and BME688 (temperature/humidity; its pressure channel
 * is dead on the current board). Enabled by default because the External board
 * is attached to the payload and its data belongs in the packet instead of the
 * dummy placeholder.
 */
#ifndef PAYLOAD_ENABLE_EXTERNAL
#define PAYLOAD_ENABLE_EXTERNAL 1
#endif

/**
 * @brief The remaining External-board devices, blocked on hardware rework.
 *
 * AS7331 (VDDA through R16 10 kOhm, A0/A1 mid-rail), BMP390 (not fitted) and
 * the ozone sensor. Leave at 0 until HARDWARE_DEBUG.md items 3, 4 and 6 are
 * done; their verify() fails and they add no usable bytes.
 */
#ifndef PAYLOAD_ENABLE_STRATOSENSE
#define PAYLOAD_ENABLE_STRATOSENSE 0
#endif

#ifndef PAYLOAD_ENABLE_CAN
#define PAYLOAD_ENABLE_CAN 1
#endif

/**
 * @brief MightyOhm Geiger counter on JGeig100.
 *
 * Not flown on this build. The driver stays in the tree but is compiled out,
 * and the MightyOhm library is excluded in platformio.ini. Set back to 1 to
 * restore the sensor and its two columns in the ASU! packet.
 */
#ifndef PAYLOAD_ENABLE_GEIGER
#define PAYLOAD_ENABLE_GEIGER 0
#endif

#ifndef PAYLOAD_ENABLE_ICM20948
#define PAYLOAD_ENABLE_ICM20948 1
#endif

/**
 * @brief Synthetic data for bench bring-up.
 *
 * PAYLOAD_DUMMY_IMU makes the ICM driver report plausible accel/gyro/mag
 * values when the real IMU is not usable, so the CAN packet path carries the
 * same 40-byte block it will in flight.
 *
 * PAYLOAD_ENABLE_DUMMY_EXTERNAL adds a 16-float placeholder for the Science
 * board. Off by default now that the four working External-board sensors are in
 * the packet; turn it on for bench tests that need the old ~138-byte packet.
 */
#ifndef PAYLOAD_DUMMY_IMU
#define PAYLOAD_DUMMY_IMU 1
#endif

#ifndef PAYLOAD_ENABLE_DUMMY_EXTERNAL
#define PAYLOAD_ENABLE_DUMMY_EXTERNAL 0
#endif

/** @brief Number of float channels the dummy external block emits. */
#define PAYLOAD_DUMMY_EXTERNAL_CHANNELS 16

/**
 * @brief Local SD logging on the payload board.
 *
 * The FA26 socket's `3V3` net was tied to `+3V3` on 2026-11-13 (the "wire of
 * shame"), so the card now reaches ACMD41 and the slot is usable. Logging is a
 * local backup; the PnC still receives the packet over CAN.
 */
#ifndef PAYLOAD_ENABLE_SD
#define PAYLOAD_ENABLE_SD 1
#endif

// temporary toggle macros for testing
#define PACKET_SYSTEM_TESTING 1

#endif
