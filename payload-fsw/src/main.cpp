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
#include "TempSensor.h"
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
volatile uint32_t payload_packets_generated = 0;
volatile uint32_t payload_queue_drops = 0;
extern volatile uint32_t payload_sd_packets_written;
extern volatile uint32_t payload_sd_packets_dropped;

// sensor classes
// clang-format off
// class        sensor            minimum period in ms
TempSensor      temp_sensor       (1000);
ICM20948Sensor  icm_sensor        (0);
#if PAYLOAD_ENABLE_GEIGER
GeigerSensor    geiger_sensor     (1000);
#endif
#if PAYLOAD_ENABLE_EXTERNAL
TMP11xSensor    tmp_sensor_out    (500,   &STRATOSENSE_I2C);
BME688Sensor    bme688_sensor_out (500,   &STRATOSENSE_I2C);
ENS160Sensor    ens160_sensor_out (500,   &STRATOSENSE_I2C, ENS160_ADDRESS_HIGH);
SHTC3Sensor     shtc3_sensor_out  (500,   &STRATOSENSE_I2C);
#endif
#if PAYLOAD_ENABLE_STRATOSENSE
AS7331Sensor    uv_sensor_out     (1000, UV_I2C_ADDR, &STRATOCORE_I2C);
OzoneSensor     ozone_sensor_out  (2000, &STRATOSENSE_I2C);
#endif
// clang-format on

// sensor array. ORDER IS PART OF THE PACKET FORMAT and must match
// packet-decoder/parse_payload.py.
// clang-format off
Sensor* sensors[] = {
    &temp_sensor,
    &icm_sensor,
    nullptr,  // Reserved RTC packet bit; the RTC is disabled.
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
    nullptr,  // Reserved BMP390 slot; the device does not respond.
    &ozone_sensor_out,
#endif
};
// clang-format on

const int sensors_len = sizeof(sensors) / sizeof(sensors[0]);

// Number of sensors that must answer for a healthy boot. PicoTemp always
// verifies, so this counts IMU + the External-board parts that are
// compiled in.
const int expected_sensors =
    2
#if PAYLOAD_ENABLE_GEIGER
    + 1
#endif
#if PAYLOAD_ENABLE_EXTERNAL
    + 4
#endif
#if PAYLOAD_ENABLE_STRATOSENSE
    + 2
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
    if (sensors[i]) sensors[i]->recoveryConfig(5, 1000);
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
                    verified_count, expected_sensors);
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
  payload_packets_generated++;
  if (!queue_try_add(&qt, packet)) payload_queue_drops++;

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
    if (sensors[i] && sensors[i]->attemptConnection()) {
      count++;
    }
  }

  log_core("Sensor Verification Results:");
  for (int i = 0; i < sensors_len; i++) {
    if (!sensors[i]) continue;
    log_core((sensors[i]->getDeviceName()) + ": " +
             (sensors[i]->getVerified()
                  ? "OK"
                  : "FAIL (check wiring and pin definitions)"));
  }
  log_core_printf("%d/%d sensors verified\n", count, expected_sensors);
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
    if (sensors[i] && sensors[i]->attemptConnection()) {
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
    Sensor* sensor = sensors[id_offset - curr_offset - 1];
    if (sensor && (sensor_id & (1 << curr_offset))) {
      csv_row += sensor->decodeToCSV(temp_packet);
    } else if (sensor && sensor->getVerified()) {
      csv_row += sensor->readEmpty();
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
    if (sensors[i] && sensors[i]->attemptConnection()) {
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
 * packet is forwarded, not just the hand-picked VALUE channels.
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
    hb.state = (uint8_t)((payload_sd_ok ? 0x02 : 0) |
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
      if (sensors[i] && sensors[i]->getVerified()) status.sensors_ok++;
    }
    status.sensors_total = (uint8_t)expected_sensors;
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
    if (sensors[i] && sensors[i]->getVerified()) verified++;
  }

  log_core_printf(
      "STATUS sensors=%d/%d sd=%d bytes=%lu can=%d time=%d "
      "tx=%lu rx=%lu err=%lu pkt_tx=%lu pkt_rx=%lu "
      "generated=%lu queue_drop=%lu sd_write=%lu sd_drop=%lu\n",
      verified, expected_sensors, payload_sd_ok ? 1 : 0,
      (unsigned long)payload_sd_bytes, can_ok ? 1 : 0,
      can_bus.hasTime() ? 1 : 0, (unsigned long)can_bus.framesSent(),
      (unsigned long)can_bus.framesReceived(),
      (unsigned long)(can_bus.sendErrors() + can_bus.readErrors()),
      (unsigned long)can_bus.packetsSent(),
      (unsigned long)can_bus.packetsReceived(),
      (unsigned long)payload_packets_generated,
      (unsigned long)payload_queue_drops,
      (unsigned long)payload_sd_packets_written,
      (unsigned long)payload_sd_packets_dropped);

  char mon[160];
  int n = snprintf(mon, sizeof(mon), "MON pico=%.2fC",
                   temp_sensor.lastTempC());
  if (n > 0 && n < (int)sizeof(mon)) {
    if (icm_sensor.getVerified() && icm_sensor.hasLastSample()) {
      n += snprintf(mon + n, sizeof(mon) - n, " icm=%.2fC/%.2fm/s2",
                    icm_sensor.lastTempC(), icm_sensor.lastAccel()[2]);
    } else {
      n += snprintf(mon + n, sizeof(mon) - n, " icm=unavailable");
    }
  }
#if PAYLOAD_ENABLE_GEIGER
  if (n > 0 && n < (int)sizeof(mon)) {
    snprintf(mon + n, sizeof(mon) - n, " geiger=%.2fcps",
             geiger_sensor.lastCPS());
  }
#endif
  log_core(mon);
}

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();

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
        Serial.println("[PNC] payload RTC disabled");
        break;
      case 'D':
      case 'd':
        sd_summary_request = true;
        break;
      case 'X':
      case 'x':
        Serial.println("[PNC] payload RTC disabled");
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
        Serial.println("[PNC] payload RTC disabled");
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
