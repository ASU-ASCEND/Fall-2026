/**
 * @file main.cpp
 * @brief Main functions for Core 0, responsible for reading data from sensor
 * peripherals
 */
#include <Arduino.h>
#include <math.h>

#include "hardware/watchdog.h"

// error code framework
#include "ErrorDisplay.h"
#include "Logger.h"
#include "PayloadConfig.h"

// interactive external-board test (serial command 'E')
#include "ExternalTest.h"

// CAN protocol / transport
#include "CanBus.h"

// parent classes
#include "Sensor.h"

// include sensor headers here
#include "ICM20948Sensor.h"
#include "PCF8523Sensor.h"
#include "TempSensor.h"
#if PAYLOAD_ENABLE_DUMMY_EXTERNAL
#include "DummySensor.h"
#endif
#if PAYLOAD_ENABLE_GEIGER
#include "GeigerSensor.h"
#endif
#if PAYLOAD_ENABLE_EXTERNAL
#include "BME688Sensor.h"
#include "ENS160Sensor.h"
#include "SHTC3Sensor.h"
#include "TMP11xSensor.h"
#endif
#if PAYLOAD_ENABLE_STRATOSENSE
#include "AS7331Sensor.h"
#include "BMP390Sensor.h"
#include "OzoneSensor.h"
#endif

// helper function definitions
void verifySensors();
void handleCommand();
int verifySensorRecovery();
String readSensorData();
uint16_t readSensorDataPacket(uint8_t* packet);
String decodePacket(uint8_t* packet);
void i2c_scan();
void rtc_raw_dump();
void handleSerial();
void can_setup();
void can_tick();
void can_send_sample(const uint8_t* packet, uint16_t len);
void print_status();

// Global variables
volatile bool payload_quiet = false;
volatile bool sd_summary_request = false;
// Written by core 1, read by core 0 for the status line and CAN heartbeat.
volatile bool payload_sd_ok = false;
volatile uint32_t payload_sd_bytes = 0;

// sensor classes
// clang-format off
// class        sensor            minimum period in ms
TempSensor      temp_sensor       (1000);
ICM20948Sensor  icm_sensor        (0);
PCF8523Sensor   rtc_sensor        (1000);
#if PAYLOAD_ENABLE_GEIGER
GeigerSensor    geiger_sensor     (1000);
#endif
#if PAYLOAD_ENABLE_DUMMY_EXTERNAL
DummySensor     dummy_ext_sensor  (500, PAYLOAD_DUMMY_EXTERNAL_CHANNELS);
#endif
#if PAYLOAD_ENABLE_EXTERNAL
TMP11xSensor    tmp_sensor_out    (500,   &STRATOSENSE_I2C);
BME688Sensor    bme688_sensor_out (500,   &STRATOSENSE_I2C);
ENS160Sensor    ens160_sensor_out (500,   &STRATOSENSE_I2C, ENS160_ADDRESS_HIGH);
SHTC3Sensor     shtc3_sensor_out  (500,   &STRATOSENSE_I2C);
#endif
#if PAYLOAD_ENABLE_STRATOSENSE
AS7331Sensor    uv_sensor_out     (500, UV_I2C_ADDR);
BMP390Sensor    bmp_sensor_out    (500,   &STRATOSENSE_I2C, BMP390_ALT_I2C_ADDR);
OzoneSensor     ozone_sensor_out  (500,   &STRATOSENSE_I2C);
#endif
// clang-format on

// sensor array. ORDER IS PART OF THE PACKET FORMAT and must match
// packet-decoder/parse_payload.py.
// clang-format off
Sensor* sensors[] = {
    &temp_sensor,
    &icm_sensor,
    &rtc_sensor,
#if PAYLOAD_ENABLE_GEIGER
    &geiger_sensor,
#endif
#if PAYLOAD_ENABLE_EXTERNAL
    &tmp_sensor_out,
    &bme688_sensor_out,
    &ens160_sensor_out,
    &shtc3_sensor_out,
#endif
#if PAYLOAD_ENABLE_STRATOSENSE
    &uv_sensor_out,
    &bmp_sensor_out,
    &ozone_sensor_out,
#endif
#if PAYLOAD_ENABLE_DUMMY_EXTERNAL
    &dummy_ext_sensor,
#endif
};
// clang-format on

const int sensors_len = sizeof(sensors) / sizeof(sensors[0]);

// Number of sensors that must answer for a healthy boot. PicoTemp always
// verifies, so this counts RTC + IMU + the External-board parts that are
// compiled in.
const int expected_sensors =
    3
#if PAYLOAD_ENABLE_GEIGER
    + 1
#endif
#if PAYLOAD_ENABLE_EXTERNAL
    + 4
#endif
#if PAYLOAD_ENABLE_STRATOSENSE
    + 3
#endif
#if PAYLOAD_ENABLE_DUMMY_EXTERNAL
    + 1
#endif
    ;

CanBus can_bus;
bool can_ok = false;

// Global variables shared with core 1
queue_t qt;

uint32_t time_paused;
uint32_t max_pause_duration = 60'000 * 2;

/**
 * @brief Setup for core 0
 *
 */
void setup() {
  // multicore setup
  queue_init(&qt, QT_ENTRY_SIZE, QT_MAX_SIZE);
  ErrorDisplay::instance().addCode(Error::NONE);  // for safety

  // setup i2c1 (StratoSense / external bus)
  Wire1.setSCL(I2C1_SCL_PIN);
  Wire1.setSDA(I2C1_SDA_PIN);

  // setup i2c0 (PCF8523 + ICM-20948)
  Wire.setSCL(I2C0_SCL_PIN);
  Wire.setSDA(I2C0_SDA_PIN);

  Wire.begin();
  Wire1.begin();

  // start serial. USB CDC only becomes ready once a host opens the port; wait
  // briefly so the boot log is not lost, but never block a battery-only boot.
  Serial.begin(115200);
  uint32_t serial_wait = millis();
  while (!Serial && millis() - serial_wait < 2000) {
    delay(10);
  }

  log_core("ASCEND payload (FA26 Internal Board) FSW");
  log_core_printf("BOOT reset_cause=%d\n", (int)watchdog_caused_reboot());

  // setup heartbeat pins
  pinMode(HEARTBEAT_PIN_0, OUTPUT);

  i2c_scan();

  // verify sensors
  // recovery config for sensors
  for (int i = 0; i < sensors_len; i++) {
    sensors[i]->recoveryConfig(5, 1000);
  }

  int verified_count = verifySensorRecovery();

  if (verified_count == 0) {
    log_core("All sensor communications failed");
    ErrorDisplay::instance().addCode(Error::CRITICAL_FAIL);
    while (1) {
      ErrorDisplay::instance().toggle();
      log_core("Error");
      delay(1000);
    }
  } else {
    log_core_printf("At least one sensor works (%d/%d), continuing\n",
                    verified_count, sensors_len);
    if (verified_count < expected_sensors) {
      ErrorDisplay::instance().addCode(Error::LOW_SENSOR_COUNT);
    }
  }

  can_setup();

  pinMode(ON_BOARD_LED_PIN, OUTPUT);
  log_core("Setup done.");
}

// loop counter
unsigned int it = 0;
/**
 * @brief Loop for core 0, handling sensor reads
 *
 */
void loop() {
  // toggle error display
  ErrorDisplay::instance().toggle();

  // toggle heartbeats
  it++;
  digitalWrite(HEARTBEAT_PIN_0, (it & 0x1));

  handleSerial();

  // start print line with iteration number
  log_core_printf("it: %d\n", it);

  // build csv row
  uint8_t packet[QT_ENTRY_SIZE];
  uint16_t packet_len = readSensorDataPacket(packet);

  // print csv row
  log_data_raw(packet, packet_len);

  // send data to core1
  queue_try_add(&qt, packet);

  // Forward the complete sample to the PnC. The PnC reassembles it and writes
  // it to its own SD card.
  can_send_sample(packet, packet_len);

  can_tick();

  // Periodic bring-up status and one-line sensor summary.
  static uint32_t last_status = 0;
  if (!payload_quiet && millis() - last_status > 2000) {
    last_status = millis();
    print_status();
  }

  delay(500);  // remove before flight

  digitalWrite(ON_BOARD_LED_PIN, (it & 0x1));  // toggle light with iteration
}

/**
 * @brief Uses Device abstraction to verify sensors
 *
 * @return int Number of sensors verified
 */
int verifySensorRecovery() {
  int count = 0;
  for (int i = 0; i < sensors_len; i++) {
    if (sensors[i]->attemptConnection()) {
      count++;
    }
  }

  log_core("Sensor Verification Results:");
  for (int i = 0; i < sensors_len; i++) {
    log_core((sensors[i]->getDeviceName()) + ": " +
             (sensors[i]->getVerified()
                  ? "OK"
                  : "FAIL (check wiring and pin definitions)"));
  }
  log_core_printf("%d/%d sensors verified\n", count, sensors_len);
  return count;
}

void i2c_scan() {
  for (int bus = 0; bus < 2; bus++) {
    TwoWire* wire = (bus == 0) ? &Wire : &Wire1;
    log_core_printf("I2C%d scan (SDA=GP%d SCL=GP%d):\n", bus,
                    bus == 0 ? I2C0_SDA_PIN : I2C1_SDA_PIN,
                    bus == 0 ? I2C0_SCL_PIN : I2C1_SCL_PIN);
    if (bus == 0) {
      // A dead scan with a line held low points at a bus clamp/short rather
      // than a missing device. Read-only: do not reconfigure the pins.
      log_core_printf("I2C0 idle levels: SDA=%d SCL=%d\n",
                      digitalRead(I2C0_SDA_PIN), digitalRead(I2C0_SCL_PIN));
    }
    uint8_t found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
      wire->beginTransmission(addr);
      if (wire->endTransmission() == 0) {
        log_core_printf("  found 0x%02X\n", addr);
        found++;
      }
    }
    if (found == 0) {
      log_core("  no devices responded");
    }
  }
}

void rtc_raw_dump() {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false) != 0) {
    Serial.println("[PNC] RTC raw: no response");
    return;
  }
  uint8_t n = Wire.requestFrom((int)RTC_I2C_ADDR, 0x13);
  Serial.printf("[PNC] RTC raw (%u bytes):", n);
  for (uint8_t i = 0; i < n; i++) {
    Serial.printf(" %02X", Wire.read());
  }
  Serial.println();
}

/**
 * @brief Reads sensor data into a packet byte array
 *
 * Layout (little endian):
 *   'A' 'S' 'U' '!'  sync
 *   uint32 sensor mask
 *   uint16 packet length
 *   uint32 millis()
 *   sensor payloads, in the order of the sensors[] array
 *   int8 checksum (byte sum is 0 mod 256)
 *
 * @param packet Pointer to the packet array
 */
uint16_t readSensorDataPacket(uint8_t* packet) {
  // set sync bytes
  uint8_t* temp_packet = packet;
  std::copy(SYNC_BYTES, SYNC_BYTES + sizeof(SYNC_BYTES), temp_packet);
  temp_packet += sizeof(SYNC_BYTES);

  uint32_t sensor_id = 0;
  uint16_t packet_len = 0;
  temp_packet += sizeof(sensor_id) + sizeof(packet_len);

  // build packet
  // millis()
  uint32_t now = millis();
  std::copy((uint8_t*)(&now), (uint8_t*)(&now) + sizeof(now), temp_packet);
  temp_packet += sizeof(now);
  sensor_id = (sensor_id << 1) | 1;
  // rest of the packet
  for (int i = 0; i < sensors_len; i++) {
    if (sensors[i]->attemptConnection()) {
      sensors[i]->getDataPacket(sensor_id, temp_packet);
    } else {
      sensor_id <<= 1;
    }
  }

  // calc data len
  packet_len = (temp_packet - packet) + 1;  // + 1 for checksum
  log_core_printf("Packet Len: %d\n", packet_len);

  // write sensor_id
  temp_packet = packet + sizeof(SYNC_BYTES);
  std::copy((uint8_t*)(&sensor_id), (uint8_t*)(&sensor_id) + sizeof(sensor_id),
            temp_packet);

  // write data len
  temp_packet += sizeof(sensor_id);
  std::copy((uint8_t*)(&packet_len),
            (uint8_t*)(&packet_len) + sizeof(packet_len), temp_packet);

  // calculate checksum with sum complement parity
  int8_t checksum = 0;
  for (size_t i = 0; i < packet_len - 1; i++) {
    checksum += packet[i];
  }
  *(packet + packet_len - 1) = -checksum;

  return packet_len;
}

/**
 * @brief Decodes the packet to a CSV row
 *
 * @param packet Pointer to the packet byte array
 * @return String The resulting CSV row
 */
String decodePacket(uint8_t* packet) {
  uint8_t* temp_packet = packet;

  uint32_t sync_bytes = *((uint32_t*)temp_packet);
  temp_packet += sizeof(sync_bytes);
  uint32_t sensor_id = *((uint32_t*)temp_packet);
  temp_packet += sizeof(sensor_id);
  uint16_t packet_len = *((uint16_t*)temp_packet);
  temp_packet += sizeof(packet_len);

  // start with sensor_id in a cell in Hex
  String csv_row = String(sensor_id, HEX) + ",";

  uint32_t sensor_id_temp = sensor_id;
  uint8_t id_offset = 0;
  for (int i = 0; i < 32; i++) {
    if (sensor_id_temp & (1 << i)) {
      id_offset = i;
    }
  }

  // millis decode
  // the words aren't aligned because of the uint16_t len
  // so casting will crash the pico
  uint32_t r_now;
  memcpy(&r_now, temp_packet, sizeof(uint32_t));

  temp_packet += sizeof(r_now);
  csv_row += String(r_now) + ",";

  int curr_offset = id_offset - 1;
  while (curr_offset >= 0) {
    if (sensor_id & (1 << curr_offset)) {
      csv_row += sensors[id_offset - curr_offset - 1]->decodeToCSV(temp_packet);
    } else if (sensors[id_offset - curr_offset - 1]->getVerified()) {
      csv_row += sensors[id_offset - curr_offset - 1]->readEmpty();
    }
    curr_offset--;
  }

  // check parity
  uint8_t sum = 0;
  for (uint16_t i = 0; i < packet_len - 1; i++) {
    sum += packet[i];
  }
  sum += *(int8_t*)(packet + packet_len - 1);
  log_core_printf("Sum = %d\n", sum);

  return csv_row;
}

/**
 * @brief Read data from each verified Sensor
 *
 * @return String Complete CSV row for iteration
 */
String readSensorData() {
  String csv_row = String(millis()) + ",";
  for (int i = 0; i < sensors_len; i++) {
    if (sensors[i]->attemptConnection()) {
      csv_row += sensors[i]->getDataCSV();
    }
  }
  return csv_row;
}

//-------------------------------------
// CAN
//-------------------------------------

void can_setup() {
#if PAYLOAD_ENABLE_CAN
  // SPI1 is dedicated to the MCP2515 on this board.
  SPI1.setRX(CAN_MISO_PIN);
  SPI1.setTX(CAN_MOSI_PIN);
  SPI1.setSCK(CAN_SCK_PIN);
  SPI1.begin();

  CanBusConfig config;
  config.spi = &SPI1;
  config.cs_pin = CAN_CS_PIN;
  config.int_pin = CAN_INT_PIN;
  config.reset_pin = CAN_RESET_PIN;
  config.stby_pin = CAN_STBY_PIN;
  config.node_id = CAN_NODE_PAYLOAD;
  config.bitrate = CAN_500KBPS;
  config.clock = MCP_16MHZ;

  can_ok = can_bus.begin(config);
  log_core(can_ok ? "CAN ready at 500 kbit/s"
                  : "CAN init FAILED (check MCP2515 / SPI1 pins)");
  if (can_ok) {
    can_bus.requestTime();
  }
#else
  log_core("CAN disabled at compile time");
#endif
}

/**
 * @brief Send one complete board packet as PACKET fragments.
 *
 * This is the "full data" path: every sensor that made it into the ASU!
 * packet (including the dummy external block for now) is forwarded, not just
 * the hand-picked VALUE channels.
 */
void can_send_sample(const uint8_t* packet, uint16_t len) {
#if !PAYLOAD_ENABLE_CAN
  (void)packet;
  (void)len;
  return;
#else
  if (!can_ok || packet == nullptr || len == 0 || len > CAN_PACKET_MAX_LEN) {
    return;
  }

  static uint32_t last_send = 0;
  if (millis() - last_send < 250) {
    return;
  }
  last_send = millis();
  can_bus.sendPacket(CAN_NODE_PAYLOAD, packet, (uint8_t)len);
#endif
}

void can_tick() {
#if !PAYLOAD_ENABLE_CAN
  return;
#else
  if (!can_ok) {
    // Retry once in a while so a controller that was not ready at boot
    // recovers without a reset.
    static uint32_t last_retry = 0;
    if (millis() - last_retry > 5000) {
      last_retry = millis();
      can_setup();
    }
    return;
  }

  can_bus.poll();

  // Adopt the PnC time if the local RTC has no usable time. The PnC has GPS
  // and is the mission time authority.
  if (can_bus.hasTime() &&
      (rtc_sensor.lostPower() || !rtc_sensor.timeValid())) {
    if (rtc_sensor.setTime(can_bus.utcUnix())) {
      log_core_printf("RTC set from CAN: %lu\n",
                      (unsigned long)can_bus.utcUnix());
    }
  }

  uint32_t now = millis();
  static uint32_t last_heartbeat = 0;
  static uint32_t last_status = 0;
  static uint32_t last_time_req = 0;
  static uint16_t heartbeat_seq = 0;

  if (!can_bus.hasTime() && now - last_time_req > 5000) {
    last_time_req = now;
    can_bus.requestTime();
  }

  if (now - last_heartbeat >= 1000) {
    last_heartbeat = now;
    CanHeartbeat hb;
    hb.node = CAN_NODE_PAYLOAD;
    hb.state = (uint8_t)((rtc_sensor.getVerified() ? 0x01 : 0) |
                         (payload_sd_ok ? 0x02 : 0) |
                         (can_bus.hasTime() ? 0x08 : 0));
    hb.seq = heartbeat_seq++;
    hb.uptime_ms = now;
    can_bus.sendStruct(CAN_ID_HEARTBEAT, hb);
  }

  if (now - last_status >= 1000) {
    last_status = now;
    CanNodeStatus status;
    status.node = CAN_NODE_PAYLOAD;
    status.sensors_ok = 0;
    for (int i = 0; i < sensors_len; i++) {
      if (sensors[i]->getVerified()) status.sensors_ok++;
    }
    status.sensors_total = (uint8_t)sensors_len;
    status.error_code = (uint8_t)ErrorDisplay::instance().getCode();
    status.uptime_ms = now;
    can_bus.sendStruct(CAN_ID_NODE_STATUS, status);
  }

#endif
}

//-------------------------------------
// Serial console
//-------------------------------------

void print_status() {
  int verified = 0;
  for (int i = 0; i < sensors_len; i++) {
    if (sensors[i]->getVerified()) verified++;
  }

  log_core_printf(
      "STATUS sensors=%d/%d sd=%d bytes=%lu can=%d time=%d rtc=%lu lost=%d "
      "tx=%lu rx=%lu err=%lu pkt_tx=%lu pkt_rx=%lu\n",
      verified, sensors_len, payload_sd_ok ? 1 : 0,
      (unsigned long)payload_sd_bytes, can_ok ? 1 : 0,
      can_bus.hasTime() ? 1 : 0, (unsigned long)rtc_sensor.nowUnix(),
      rtc_sensor.lostPower() ? 1 : 0, (unsigned long)can_bus.framesSent(),
      (unsigned long)can_bus.framesReceived(),
      (unsigned long)(can_bus.sendErrors() + can_bus.readErrors()),
      (unsigned long)can_bus.packetsSent(),
      (unsigned long)can_bus.packetsReceived());

  char mon[160];
  int n = snprintf(mon, sizeof(mon),
                   "MON pico=%.2fC icm=%.2fC/%.2fm/s2 rtc=%lu",
                   temp_sensor.lastTempC(), icm_sensor.lastTempC(),
                   icm_sensor.lastAccel()[2],
                   (unsigned long)rtc_sensor.nowUnix());
#if PAYLOAD_ENABLE_GEIGER
  if (n > 0 && n < (int)sizeof(mon)) {
    snprintf(mon + n, sizeof(mon) - n, " geiger=%.2fcps",
             geiger_sensor.lastCPS());
  }
#endif
  log_core(mon);
}

void handleSerial() {
  static bool expect_epoch = false;
  static String epoch_buf;

  while (Serial.available()) {
    char c = (char)Serial.read();

    if (expect_epoch) {
      if (c >= '0' && c <= '9') {
        epoch_buf += c;
        continue;
      }
      if (c == '\r' || c == '\n') {
        if (epoch_buf.length() >= 9) {
          uint32_t epoch = strtoul(epoch_buf.c_str(), nullptr, 10);
          if (rtc_sensor.setTime(epoch)) {
            Serial.printf("[PNC] RTC set to %lu\n", (unsigned long)epoch);
          } else {
            Serial.println("[PNC] RTC set failed");
          }
        } else {
          Serial.println("[PNC] usage: S<unix-seconds>");
        }
        expect_epoch = false;
        epoch_buf = "";
      }
      continue;
    }

    switch (c) {
      case 'B':
      case 'b':
        payload_quiet = true;
        break;
      case 'T':
      case 't':
        payload_quiet = false;
        Serial.println("[PNC] verbose mode");
        break;
      case 'I':
      case 'i':
        i2c_scan();
        break;
      case 'E':
      case 'e':
        external_test();
        break;
      case 'W':
      case 'w':
        external_watch(20, 3);
        break;
      case 'C':
      case 'c':
        Serial.printf("[PNC] rtc=%lu lost_power=%d valid=%d\n",
                      (unsigned long)rtc_sensor.nowUnix(),
                      (int)rtc_sensor.lostPower(),
                      (int)rtc_sensor.timeValid());
        break;
      case 'D':
      case 'd':
        sd_summary_request = true;
        break;
      case 'X':
      case 'x':
        rtc_raw_dump();
        break;
      case 'K':
      case 'k': {
        CanLoopbackResult result = can_bus.loopbackTest();
        Serial.printf(
            "[PNC] CAN loopback %s sent=%lu received=%lu id=0x%03X len=%u\n",
            result.ok ? "PASS" : "FAIL", (unsigned long)result.sent_counter,
            (unsigned long)result.received_counter, result.echoed_id,
            result.echoed_len);
        break;
      }
      case 'Y':
      case 'y': {
        bool ok = can_bus.timeLoopbackTest(1700000000UL);
        Serial.printf("[PNC] CAN time-sync loopback %s\n",
                      ok ? "PASS" : "FAIL");
        break;
      }
      case 'L':
      case 'l': {
        bool ok = can_bus.packetLoopbackTest(millis());
        Serial.printf(
            "[PNC] CAN packet loopback %s (tx=%lu rx=%lu drop=%lu len=%u)\n",
            ok ? "PASS" : "FAIL", (unsigned long)can_bus.packetsSent(),
            (unsigned long)can_bus.packetsReceived(),
            (unsigned long)can_bus.packetDrops(),
            can_bus.lastPacketLength());
        break;
      }
      case 'N':
      case 'n':
        Serial.printf(
            "[PNC] can ready=%d tx=%lu rx=%lu txerr=%lu rxerr=%lu "
            "timesync=%lu timereq=%lu busreset=%lu lastid=0x%03X time=%lu "
            "age=%lu pkt_tx=%lu pkt_rx=%lu pkt_drop=%lu\n",
            can_bus.ready() ? 1 : 0, (unsigned long)can_bus.framesSent(),
            (unsigned long)can_bus.framesReceived(),
            (unsigned long)can_bus.sendErrors(),
            (unsigned long)can_bus.readErrors(),
            (unsigned long)can_bus.timeSyncCount(),
            (unsigned long)can_bus.timeRequestCount(),
            (unsigned long)can_bus.busResets(), can_bus.lastReceivedId(),
            (unsigned long)can_bus.utcUnix(),
            (unsigned long)can_bus.timeSyncAgeMs(),
            (unsigned long)can_bus.packetsSent(),
            (unsigned long)can_bus.packetsReceived(),
            (unsigned long)can_bus.packetDrops());
        break;
      case 'S':
      case 's':
        expect_epoch = true;
        epoch_buf = "";
        Serial.println("[PNC] send S<unix-seconds> then newline");
        break;
      case 'R':
      case 'r':
        Serial.println("[PNC] rebooting");
        Serial.flush();
        watchdog_enable(100, true);
        while (1);
        break;
      default:
        break;
    }
  }
}

//-------------------------------------
// Core 1 Calls
//-------------------------------------
// declarations - definitions are in main1.cpp
void real_setup1();
void real_loop1();

void setup1() { real_setup1(); }

void loop1() { real_loop1(); }

//-------------------------------------
