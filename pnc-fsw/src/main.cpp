#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

#include "CanBus.h"
#include "ErrorDisplay.h"
#include "SysHead.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/time.h"

// tasks
#include "tasks/monitor.h"
#include "tasks/watchdog.h"
#include "tasks/checkout.h"

// sensors
#include "drivers/BMESensor.h"
#include "drivers/GPSSensor.h"
#include "drivers/INASensor.h"
#include "drivers/PicoTempSensor.h"
#include "drivers/RTCSensor.h"
#include "drivers/Sensor.h"

#ifndef PNC_ENABLE_RADIACODE
// 0 since 2026-11-19: the RadiaCode clock is synced from a phone (UTC via
// NTP) as a pre-flight step, so no BLE path is built at all. The CYW43
// AP->BLE handoff deadlocked btstack after checkout teardown; see
// LAUNCH_CHECKLIST.md. Flip to 1 only to resume on-device BLE clock sync.
#define PNC_ENABLE_RADIACODE 0
#endif

#if PNC_ENABLE_RADIACODE
#include "RadiacodeBLE.h"
#endif

// Print the per-loop task chatter. Off by default so the serial link carries a
// clean CSV/binary packet stream that packet-decoder/ can actually parse.
#define PNC_VERBOSE_LOG 0

// Bench A/B for the SD stall: 0 keeps the card unmounted and takes the SD write
// path out of store_data(), while CAN/sensor logging continues.
#ifndef PNC_ENABLE_SD
#define PNC_ENABLE_SD 1
#endif

PicoTempSensor pico_temp_sensor;
RTCSensor rtc_sensor;
BMESensor bme_sensor;
GPSSensor gps_sensor;
INASensor ina_sensor;

Sensor* sensors[] = {&pico_temp_sensor, &rtc_sensor, &bme_sensor, &ina_sensor,
                     &gps_sensor};
const size_t sensors_len = sizeof(sensors) / sizeof(sensors[0]);
bool found_sensors[sensors_len];

#if PNC_ENABLE_RADIACODE
String rc_target_mac = "52:43:06:60:17:DD";
String rc_filename = "";
#endif
// declarations
void sysvar_update();
void store_data();
void sd_setup();
void i2c_scan();
void print_packet_csv(const struct Packet& packet);
void handle_serial_commands();
void dump_sd_summary();
void can_setup();
void can_service();
void can_send_time_sync();
#if PNC_ENABLE_RADIACODE
void radiacode_service();
#endif

// The PnC is the mission time authority: it owns the RTC and the GPS, so it
// broadcasts UTC to the other nodes over CAN.
CanBus can_bus;
bool can_ok = false;

// Latest telemetry received from the payload board, kept for the status line.
volatile uint32_t can_payload_frames = 0;
volatile uint32_t can_last_payload_ms = 0;
volatile float can_payload_pico_temp = 0;
volatile float can_payload_geiger_cps = 0;
volatile int16_t can_payload_accel_mg = 0;
volatile uint8_t can_payload_node = 0xFF;

void can_on_frame(uint16_t id, const uint8_t* data, uint8_t len) {
  if (id == CAN_ID_HEARTBEAT && len >= sizeof(CanHeartbeat)) {
    CanHeartbeat hb;
    memcpy(&hb, data, sizeof(hb));
    can_payload_node = hb.node;
    can_last_payload_ms = millis();
    can_payload_frames++;
  } else if (id == CAN_ID_IMU_SAMPLE && len >= sizeof(CanImuSample)) {
    CanImuSample imu;
    memcpy(&imu, data, sizeof(imu));
    if (imu.frag == CAN_IMU_FRAG_ACCEL) {
      can_payload_accel_mg = imu.c;
    }
    can_last_payload_ms = millis();
    can_payload_frames++;
  } else if (id >= CAN_ID_VALUE_BASE && id <= CAN_ID_VALUE_LAST &&
             len >= sizeof(CanChannelValue)) {
    CanChannelValue value;
    memcpy(&value, data, sizeof(value));
    uint8_t channel = (uint8_t)(id - CAN_ID_VALUE_BASE);
    if (channel == CAN_VALUE_PICO_TEMP_C) {
      can_payload_pico_temp = value.value;
    } else if (channel == CAN_VALUE_GEIGER_CPS) {
      can_payload_geiger_cps = value.value;
    }
    can_last_payload_ms = millis();
    can_payload_frames++;
  }
}

// SPI1 is shared by the microSD and the MCP2515. Core 0 owns the SD, core 1
// services CAN at a high rate, so every SPI1 transaction goes through this
// mutex.
mutex_t spi1_mutex;

/**
 * @brief One reassembled packet received from another node, queued for the SD
 * writer on core 0.
 */
struct __attribute__((packed)) PayloadRecord {
  uint32_t unix_s;     // PnC UTC at reception (0 if unknown)
  uint32_t uptime_ms;  // PnC millis at reception
  uint8_t node;
  uint8_t length;
  uint8_t data[CAN_PACKET_MAX_LEN];
};

queue_t payload_queue;
File payload_file;
bool payload_log_ok = false;
volatile uint32_t payload_packets_logged = 0;
volatile uint32_t payload_queue_drops = 0;

void can_on_packet(uint8_t node, const uint8_t* data, uint8_t len) {
  PayloadRecord record = {};
  uint32_t unix_s = 0;
  sysvar_get_rtc_time(&unix_s);
  record.unix_s = unix_s;
  record.uptime_ms = millis();
  record.node = node;
  if (len > CAN_PACKET_MAX_LEN) len = CAN_PACKET_MAX_LEN;
  record.length = len;
  memcpy(record.data, data, len);

  if (!queue_try_add(&payload_queue, &record)) payload_queue_drops++;

  can_payload_frames++;
  can_last_payload_ms = millis();
  can_payload_node = node;
}

bool sd_status = false;
String filename = "";
File data_file;
bool sd_log_opened = false;
bool sd_space_low = false;
bool sd_info_ok = false;
uint32_t sd_total_mb = 0;
uint32_t sd_used_mb = 0;
uint8_t sd_remounts = 0;
volatile bool pnc_setup_done = false;
volatile bool reboot_requested = false;

struct __attribute__((packed)) Packet {
  uint32_t sync_bytes;
  uint32_t uptime;
  uint8_t id;
  uint8_t length;
  float temp_data;
  uint32_t rtc_time;
  BMESensorData bme_data;
  INASensorData ina_data;
  GPSSensorData gps_data;
  int8_t checksum;
};

static_assert(sizeof(Packet) == 87, "packet layout changed; update parse_pnc.py");
static_assert(sizeof(Packet) <= 255, "packet must fit in the length byte");

void setup() {
  sysvar_init();       // setup sysvar protection
  watchdog_disable();  // wait for setup later
  mutex_init(&spi1_mutex);
  queue_init(&payload_queue, sizeof(PayloadRecord), 8);
  pinMode(LED_BUILTIN, OUTPUT);

  // External ATtiny85 watchdog (U7) is fed by this pin through Q3 -> Pico RUN.
  pinMode(WATCHDOG_HEARTBEAT_PIN, OUTPUT);
  digitalWrite(WATCHDOG_HEARTBEAT_PIN, LOW);

  Serial.begin(115200);

  Serial.printf("BOOT reset_cause=%d\n", (int)watchdog_caused_reboot());

  // I2C0 is GP4/GP5 on the PnC schematic (BME688, INA260, PCF8523, GPS).
  Wire.setSDA(I2C_SDA_PIN);
  Wire.setSCL(I2C_SCL_PIN);
  Wire.begin();
  Wire.setClock(100000);

  log_task("ASCEND PnC FSW");
  i2c_scan();

  // The MCP2515 shares SPI1 with the SD card. Its CS has no pull-up on the
  // schematic, so drive it high (and keep the controller out of reset) or it
  // can fight the card on MISO.
  pinMode(CAN_CS_PIN, OUTPUT);
  digitalWrite(CAN_CS_PIN, HIGH);
  pinMode(CAN_RESET_PIN, OUTPUT);
  digitalWrite(CAN_RESET_PIN, HIGH);

  // sensor setups
  for (size_t i = 0; i < sensors_len; i++) {
    log_task("Verifying " + sensors[i]->getSensorName() + "...");
    if (sensors[i]->verify()) {
      found_sensors[i] = true;
      log_task("Success.");
    } else {
      found_sensors[i] = false;
      log_task("Failure.");
    }
  }

  // sd setup
#if PNC_ENABLE_SD
  sd_setup();
#endif

  // CAN: the MCP2515 shares SPI1 with the card, so bring it up after the bus
  // pins are configured.
  can_setup();

  log_task("Setup complete.");
  pnc_setup_done = true;
  checkout_init();
}

void i2c_scan() {
  log_task("I2C scan (expect 0x40 INA260, 0x42 GPS, 0x68 RTC, 0x76 BME688):");
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      log_task("  found 0x" + String(addr, HEX));
      found++;
    }
  }
  if (found == 0) {
    log_task("  no I2C devices responded");
  }
}

/** Zero-padded data file name: data0007.bin. */
static String sd_data_name(int index) {
  char suffix[12];
  snprintf(suffix, sizeof(suffix), "%04d.bin", index);
  return String("data") + suffix;
}

/**
 * @brief Find the highest existing dataNNNN.bin in one directory pass.
 *
 * The old code called SD.exists() for 0,1,2,... so mount cost grew with the
 * file count. One readdir is O(n) and also lets us zero-pad the next name.
 */
static int sd_next_data_index() {
  int max_index = -1;
  Dir dir = SDFS.openDir("/");
  if (!dir.rewind()) return -1;

  while (dir.next()) {
    if (dir.isFile()) {
      String name = dir.fileName();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      name.toLowerCase();
      if (name.startsWith("data") && name.endsWith(".bin")) {
        String digits = name.substring(4, name.length() - 4);
        bool all_digits = digits.length() > 0;
        for (size_t i = 0; i < digits.length(); i++) {
          if (!isdigit((unsigned char)digits[i])) all_digits = false;
        }
        if (all_digits) {
          int index = digits.toInt();
          if (index > max_index) max_index = index;
        }
      }
    }
  }
  return max_index + 1;
}

/**
 * @brief Mount the microSD card.
 *
 * The card is on SPI1 (GP8 MISO, GP10 SCK, GP11 MOSI, GP17 CS) according to
 * the PnC netlist, not on the Pico's default SPI0 pins.
 */
void sd_setup() {
  static bool attempted = false;
  static uint32_t last_attempt_ms = 0;

  // A failing card used to unmount and remount every 5 s, minting an empty
  // dataN.bin each time. Give up after a few tries until the next power cycle.
  if (sd_remounts >= 3) {
    return;
  }
  if (attempted && millis() - last_attempt_ms < 5000) {
    return;  // retry at most every 5 s
  }
  attempted = true;
  last_attempt_ms = millis();

  log_task("Mounting SD on SPI1...");
  SPI1.setRX(SD_MISO_PIN);
  SPI1.setTX(SD_MOSI_PIN);
  SPI1.setSCK(SD_SCK_PIN);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  mutex_enter_blocking(&spi1_mutex);
  if (data_file) data_file.close();
  sd_log_opened = false;
  if (payload_file) {
    payload_file.close();
    payload_log_ok = false;
  }

  // arduino-pico 5.x signature: begin(csPin, spi). The 3-arg form is
  // begin(csPin, spiConfig, spi), not begin(csPin, spi, frequency).
  if (!SD.begin(SD_CS_PIN, SPI1)) {
    ErrorDisplay::instance().addCode(Error::SD_CARD_FAIL);
    log_task("SD mount failed.");
    sd_status = false;
    sd_remounts++;
    mutex_exit(&spi1_mutex);
    return;
  }

  // One readdir, not N SD.exists() walks. The file is not created here: it is
  // opened on the first successful packet write, so a boot with no data does
  // not leave an empty file behind.
  int next_index = sd_next_data_index();
  if (next_index < 0) {
    log_task("SD root directory open failed");
    sd_status = false;
    sd_remounts++;
    SD.end(false);
    mutex_exit(&spi1_mutex);
    return;
  }
  filename = sd_data_name(next_index);
  sd_status = true;

  FSInfo fs_info = {};
  sd_info_ok = SDFS.info(fs_info);
  uint64_t total = fs_info.totalBytes;
  uint64_t used = fs_info.usedBytes;
  sd_total_mb = (uint32_t)(total / (1024ULL * 1024ULL));
  sd_used_mb = (uint32_t)(used / (1024ULL * 1024ULL));
  uint64_t free_bytes = (total > used) ? (total - used) : 0;
  sd_space_low = (total > 0 && free_bytes < 8ULL * 1024ULL * 1024ULL);

  // Reassembled packets from the other boards go to their own file. Records
  // are self-describing (magic + length), so appending across boots is safe.
  payload_file = SD.open("PAYLOAD.BIN", FILE_WRITE);
  payload_log_ok = (bool)payload_file;
  mutex_exit(&spi1_mutex);

  log_task("Log target: " + filename +
           (sd_space_low ? " (LOW SPACE, data logging off)" : " (lazy open)") +
           (payload_log_ok ? " (+PAYLOAD.BIN)" : ""));
}

void can_setup() {
  // SPI1 is shared with the SD card; sd_setup() already selected these pins,
  // but set them again in case the card was missing.
  SPI1.setRX(SD_MISO_PIN);
  SPI1.setTX(SD_MOSI_PIN);
  SPI1.setSCK(SD_SCK_PIN);
  SPI1.begin();

  CanBusConfig config;
  config.spi = &SPI1;
  config.cs_pin = CAN_CS_PIN;
  config.int_pin = CAN_INT_PIN;
  config.reset_pin = CAN_RESET_PIN;
  config.stby_pin = 0xFF;  // strapped through JMP_SIL on this board
  config.node_id = CAN_NODE_PNC;
  config.bitrate = CAN_500KBPS;
  config.clock = MCP_16MHZ;

  mutex_enter_blocking(&spi1_mutex);
  can_ok = can_bus.begin(config);
  if (can_ok) {
    can_bus.setFrameCallback(can_on_frame);
    can_bus.setPacketCallback(can_on_packet);
    can_send_time_sync();
  }
  mutex_exit(&spi1_mutex);

  log_task(can_ok ? "CAN ready at 500 kbit/s"
                  : "CAN init FAILED (MCP2515 / SPI1)");
}

void can_send_time_sync() {
  if (!can_ok) {
    return;
  }

  uint32_t rtc_time = 0;
  GPSSensorData gps = {};
  sysvar_get_rtc_time(&rtc_time);
  sysvar_get_gps_data(&gps);

  CanTimeSync sync = {};
  // Prefer a 3D GPS fix (it is the only source traceable to UTC), then the
  // RTC, which the host or the GPS can set.
  if (gps.fix_type >= 2 && gps.unix_time_s > 1600000000UL) {
    sync.unix_s = gps.unix_time_s;
    sync.source = CAN_TIME_SRC_GPS;
    sync.flags |= CAN_TIME_FLAG_VALID | CAN_TIME_FLAG_GPS_FIX;
  } else if (rtc_time > 1600000000UL) {
    sync.unix_s = rtc_time;
    sync.source = CAN_TIME_SRC_RTC;
    sync.flags |= CAN_TIME_FLAG_VALID;
  }

  sync.millis = (uint16_t)(millis() % 1000UL);

  can_bus.sendStruct(CAN_ID_TIME_SYNC, sync);
}

/**
 * @brief Fast CAN service for core 1.
 *
 * The MCP2515 only has two RX buffers and the payload can push 70+ frames/s,
 * so this must run far more often than the 0.6 s sensor/store loop. It takes
 * the SPI1 mutex because the SD card shares the bus.
 */
void can_service() {
  if (!can_ok) {
    static uint32_t last_retry = 0;
    if (millis() - last_retry > 5000) {
      last_retry = millis();
      can_setup();
    }
    return;
  }

  // Core 0 holds this mutex across SD writes, and a slow write (especially
  // around a RadiaCode BLE sync) can hold it for longer than the 5 s hardware
  // watchdog. Keep feeding the watchdog while we wait instead of blocking
  // here unfed.
  while (!mutex_try_enter(&spi1_mutex, nullptr)) {
    watchdog_update();
    delay(1);
  }
  uint32_t requests_before = can_bus.timeRequestCount();
  can_bus.poll();

  // Answer a TIME_REQ immediately, otherwise broadcast once a second.
  static uint32_t last_sync = 0;
  bool requested = can_bus.timeRequestCount() != requests_before;
  if (requested || millis() - last_sync >= 1000) {
    last_sync = millis();
    can_send_time_sync();
  }
  mutex_exit(&spi1_mutex);
}

static uint8_t it = 0;
void loop() {
  CheckoutHealth checkout_health = {{found_sensors[0], found_sensors[1],
      found_sensors[2], found_sensors[3], found_sensors[4]},
      sd_status && !sd_space_low, can_ok};
  checkout_poll(checkout_health);
  handle_serial_commands();

  // Feed the external ATtiny85 watchdog.
  static bool heartbeat = false;
  heartbeat = !heartbeat;
  digitalWrite(WATCHDOG_HEARTBEAT_PIN, heartbeat);

  digitalWrite(LED_BUILTIN, it & 0x1);
  it++;

  watchdog_intertask_update(WATCHDOG_MAIN_TASK_ID);
  sysvar_update();
  store_data();

#if PNC_ENABLE_RADIACODE
  // The CYW43 is shared with Wi-Fi; wait until checkout has shut down.
  if (millis() >= CHECKOUT_DURATION_MS) {
    checkout_poll(checkout_health);
    static bool ble_initialized = false;
    if (!checkout_is_active()) {
      if (!ble_initialized) {
        radiacode_ble_init();
        ble_initialized = true;
      }
      radiacode_service();
    }
  }
#endif
  // CAN is serviced by the fast task on core 1.

  // Periodic bring-up status: catches the SD mount / sensor results even if
  // the USB port was not open during setup.
  static uint32_t last_status_ms = 0;
  if (!pnc_binary_mode && millis() - last_status_ms > 5000) {
    last_status_ms = millis();
    int sensor_count = 0;
    for (size_t i = 0; i < sensors_len; i++) {
      if (found_sensors[i]) sensor_count++;
    }
    uint32_t file_size = 0;
    mutex_enter_blocking(&spi1_mutex);
    if (sd_status && data_file) {
      file_size = data_file.size();
    }
    mutex_exit(&spi1_mutex);
    Serial.printf(
        "STATUS sd=%d file=%s bytes=%lu sd_open=%d sd_low=%d sd_info=%d "
        "sd_total_mb=%lu sd_used_mb=%lu payload_open=%d sd_failures=%u "
        "sensors=%d/%d can=%d tx=%lu rx=%lu "
        "busreset=%lu payload_frames=%lu pkt_rx=%lu pkt_drop=%lu queue_drop=%lu pkt_log=%lu "
        "payload_age=%lu reset=%d wdt=%d\n",
        sd_status, filename.c_str(), (unsigned long)file_size,
        sd_log_opened ? 1 : 0, sd_space_low ? 1 : 0, sd_info_ok ? 1 : 0,
        (unsigned long)sd_total_mb, (unsigned long)sd_used_mb,
        payload_log_ok ? 1 : 0, (unsigned)sd_remounts, sensor_count,
        (int)sensors_len, can_ok ? 1 : 0, (unsigned long)can_bus.framesSent(),
        (unsigned long)can_bus.framesReceived(),
        (unsigned long)can_bus.busResets(),
        (unsigned long)can_payload_frames,
        (unsigned long)can_bus.packetsReceived(),
        (unsigned long)can_bus.packetDrops(),
        (unsigned long)payload_queue_drops,
        (unsigned long)payload_packets_logged,
        can_payload_frames ? (unsigned long)(millis() - can_last_payload_ms)
                           : 0UL,
        (int)rp2040.getResetReason(), (int)watchdog_caused_reboot());
    if (can_payload_frames) {
      Serial.printf("PAYLOAD node=%u pkt_len=%u\n", can_payload_node,
                    can_bus.lastPacketLength());
    }
  }

  delay(200);
}

void handle_serial_commands() {
  static bool expect_epoch = false;
  static String epoch_buf;

  while (Serial.available()) {
    char c = (char)Serial.read();

    // "S<unix-seconds>\n" sets the RTC. Digits are accumulated until newline.
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
        Serial.print("\n[PNC] binary packet mode\n");
        pnc_binary_mode = true;
        break;
      case 'T':
      case 't':
        pnc_binary_mode = false;
        Serial.print("\n[PNC] text CSV mode\n");
        break;
      case 'R':
      case 'r':
        Serial.print("\n[PNC] rebooting\n");
        Serial.flush();
        // Stop the core 1 watchdog feed first, then arm a 100 ms watchdog
        // reset; otherwise the 5 ms feed defeats the reboot.
        reboot_requested = true;
        watchdog_reboot(0, 0, 100);
        while (1);
        break;
      case 'D':
      case 'd':
        dump_sd_summary();
        break;
      case 'I':
      case 'i':
        i2c_scan();
        break;
      case 'K':
      case 'k': {
        mutex_enter_blocking(&spi1_mutex);
        CanLoopbackResult result = can_bus.loopbackTest();
        mutex_exit(&spi1_mutex);
        Serial.printf(
            "[PNC] CAN loopback %s sent=%lu received=%lu id=0x%03X len=%u\n",
            result.ok ? "PASS" : "FAIL", (unsigned long)result.sent_counter,
            (unsigned long)result.received_counter, result.echoed_id,
            result.echoed_len);
        break;
      }
      case 'Y':
      case 'y': {
        mutex_enter_blocking(&spi1_mutex);
        bool ok = can_bus.timeLoopbackTest(1700000000UL);
        mutex_exit(&spi1_mutex);
        Serial.printf("[PNC] CAN time-sync loopback %s\n",
                      ok ? "PASS" : "FAIL");
        break;
      }
      case 'L':
      case 'l': {
        mutex_enter_blocking(&spi1_mutex);
        bool ok = can_bus.packetLoopbackTest(millis());
        uint32_t tx = can_bus.packetsSent();
        uint32_t rx = can_bus.packetsReceived();
        uint32_t drops = can_bus.packetDrops();
        uint8_t last_len = can_bus.lastPacketLength();
        mutex_exit(&spi1_mutex);
        Serial.printf(
            "[PNC] CAN packet loopback %s (tx=%lu rx=%lu drop=%lu len=%u)\n",
            ok ? "PASS" : "FAIL", (unsigned long)tx, (unsigned long)rx,
            (unsigned long)drops, last_len);
        break;
      }
      case 'N':
      case 'n':
        Serial.printf(
            "[PNC] can ready=%d tx=%lu rx=%lu txerr=%lu rxerr=%lu "
            "timesync=%lu timereq=%lu busreset=%lu lastid=0x%03X\n",
            can_bus.ready() ? 1 : 0, (unsigned long)can_bus.framesSent(),
            (unsigned long)can_bus.framesReceived(),
            (unsigned long)can_bus.sendErrors(),
            (unsigned long)can_bus.readErrors(),
            (unsigned long)can_bus.timeSyncCount(),
            (unsigned long)can_bus.timeRequestCount(),
            (unsigned long)can_bus.busResets(), can_bus.lastReceivedId());
        break;
      case 'C':
      case 'c':
        Serial.printf("[PNC] rtc=%lu lost_power=%d\n",
                      (unsigned long)rtc_sensor.nowUnix(),
                      (int)rtc_sensor.lostPower());
        break;
      case 'S':
      case 's':
        expect_epoch = true;
        epoch_buf = "";
        Serial.println("[PNC] send S<unix-seconds> then newline");
        break;
      default:
        break;
    }
  }
}

void dump_sd_summary() {
  if (!sd_status) {
    Serial.println("[PNC] SD not mounted");
    return;
  }

  mutex_enter_blocking(&spi1_mutex);
  uint32_t size = 0;
  uint32_t packets = 0;
  uint32_t valid = 0;
  uint32_t bad = 0;
  if (data_file) {
    data_file.flush();
    size = data_file.size();
    packets = size / sizeof(Packet);

    // Read through a separate read-only handle: the logging handle is opened
    // O_APPEND/O_RDWR and does not reliably seek back for verification.
    File read_file = SD.open(filename, FILE_READ);
    if (read_file) {
      Packet p;
      read_file.seek(0);
      while (read_file.read((uint8_t*)&p, sizeof(p)) == sizeof(p)) {
        bool ok = p.sync_bytes == 0xDEADCAFE && p.length == sizeof(Packet);
        if (ok) {
          int8_t sum = 0;
          uint8_t* b = (uint8_t*)&p;
          for (size_t i = 0; i < sizeof(Packet); i++) sum += b[i];
          ok = sum == 0;
        }
        if (ok) {
          valid++;
        } else {
          bad++;
        }
      }
      read_file.close();
    }
  }

  uint32_t payload_size = 0;
  if (payload_log_ok) {
    payload_file.flush();
    payload_size = payload_file.size();
  }

  mutex_exit(&spi1_mutex);

  Serial.printf("[PNC] %s bytes=%lu packets=%lu valid=%lu bad=%lu\n",
                filename.c_str(), (unsigned long)size, (unsigned long)packets,
                (unsigned long)valid, (unsigned long)bad);
  Serial.printf("[PNC] PAYLOAD.BIN bytes=%lu packets=%lu\n",
                (unsigned long)payload_size,
                (unsigned long)payload_packets_logged);
}

void print_packet_csv(const struct Packet& packet) {
  Serial.printf(
      "PKT,%lu,%u,%u,%.2f,%lu,%.2f,%lu,%.2f,%lu,%.2f,%.2f,%.2f,%lu,%u,%u,%u,"
      "%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d\n",
      (unsigned long)packet.uptime, packet.id, packet.length, packet.temp_data,
      (unsigned long)packet.rtc_time, packet.bme_data.BMETemp,
      (unsigned long)packet.bme_data.BMEPressure, packet.bme_data.BMEHumidity,
      (unsigned long)packet.bme_data.BMEGasResistance, packet.ina_data.INACurrent,
      packet.ina_data.INABusVoltage, packet.ina_data.INAPower,
      (unsigned long)packet.gps_data.unix_time_s, packet.gps_data.fix_type,
      packet.gps_data.fix_ok, packet.gps_data.siv, packet.gps_data.lat_e7,
      packet.gps_data.lon_e7, packet.gps_data.alt_msl_mm,
      packet.gps_data.vel_n_mmps, packet.gps_data.vel_e_mmps,
      packet.gps_data.vel_d_mmps, packet.gps_data.hacc_mm,
      packet.gps_data.vacc_mm, packet.checksum);
}

#if PNC_ENABLE_RADIACODE
// The RadiaCode records measurements and events to its own memory (about a
// thousand hours) as soon as it is powered, with no host involved. Reading
// VS::DATA_BUF moves those records off the device and deletes them there, so
// the PnC must never poll it during a flight. All this link does is set the
// device clock from the PnC's GPS/RTC time authority so the on-device log is
// stamped against the mission timeline, then release the device again.
#define RC_FIRST_SYNC_MS 10000UL
#define RC_SYNC_PERIOD_MS 86400000UL  // re-sync once a day
#define RC_RETRY_MS 300000UL          // 5 min if the sync itself failed
#define RC_RETRY_NO_TIME_MS 600000UL  // 10 min if no GPS/RTC time yet

namespace {
// Bounded BLE window: the blocking BLE calls cannot feed the watchdog from
// inside, and watchdog_pause() has a core-1 re-arm race that got the chip
// reset mid-connect (wdt=1 in the boot status after a RadiaCode sync).
// Instead of disarming the watchdog, a hardware alarm keeps it fed every 3 s
// for up to RC_BLE_WINDOW_MS. Past the deadline the feed stops and the armed
// watchdog resets the board within its 8 s interval, so a wedged BLE stack
// costs a clean reboot instead of a frozen board. If the wedge masks
// interrupts the alarm never runs and the watchdog still fires.
constexpr uint32_t RC_BLE_WINDOW_MS = 60000;
volatile uint32_t rc_feed_deadline_ms = 0;
volatile alarm_id_t rc_feed_alarm = -1;

int64_t rc_wdt_feed_cb(alarm_id_t id, void *user) {
  (void)id;
  (void)user;
  if ((int32_t)(millis() - rc_feed_deadline_ms) >= 0) {
    rc_feed_alarm = -1;
    return 0;  // stop feeding: the watchdog reboots the board
  }
  watchdog_update();
  return 3000000;  // re-arm in 3 s
}

void rc_ble_window_begin() {
  rc_feed_deadline_ms = millis() + RC_BLE_WINDOW_MS;
  if (rc_feed_alarm >= 0) cancel_alarm(rc_feed_alarm);
  rc_feed_alarm = add_alarm_in_ms(3000, rc_wdt_feed_cb, nullptr, true);
  watchdog_update();
}

void rc_ble_window_end() {
  if (rc_feed_alarm >= 0) {
    cancel_alarm(rc_feed_alarm);
    rc_feed_alarm = -1;
  }
  watchdog_update();
}
}  // namespace

void radiacode_service() {
  static uint32_t next_ms = RC_FIRST_SYNC_MS;

  if ((int32_t)(millis() - next_ms) < 0) return;

  if (!pnc_binary_mode) Serial.println("[PNC] RadiaCode clock sync");

  // The framework's connect() is a single blocking call we cannot feed from
  // inside. Keep the watchdog armed but alarm-fed for this bounded ground
  // sync window; see rc_ble_window_begin().
  rc_ble_window_begin();
  uint8_t connect_rc = radiacode_ble_connect(rc_target_mac, true);
  rc_ble_window_end();

  if (connect_rc != 0) {
    next_ms = millis() + RC_RETRY_MS;
    if (!pnc_binary_mode) Serial.println("[PNC] RadiaCode connect failed");
    return;
  }
  watchdog_update();

  // GPS fix beats the RTC: it is the only source traceable to UTC.
  uint32_t unix_s = 0;
  GPSSensorData gps = {};
  sysvar_get_rtc_time(&unix_s);
  sysvar_get_gps_data(&gps);
  if (gps.fix_type >= 2 && gps.unix_time_s > 1600000000UL) {
    unix_s = gps.unix_time_s;
  }
#ifdef RC_BENCH_UNIX
  unix_s = (uint32_t)RC_BENCH_UNIX;  // bench build only
#endif
  if (unix_s < 1600000000UL) {
    next_ms = millis() + RC_RETRY_NO_TIME_MS;
    if (!pnc_binary_mode) Serial.println("[PNC] RadiaCode clock unknown");
  } else {
    rc_ble_window_begin();
    uint8_t set_rc = radiacode_set_time(unix_s);
    rc_ble_window_end();
    if (set_rc == 0) {
      next_ms = millis() + RC_SYNC_PERIOD_MS;
      if (!pnc_binary_mode) {
        Serial.printf("[PNC] RadiaCode clock set to %lu\n",
                      (unsigned long)unix_s);
      }
    } else {
      next_ms = millis() + RC_RETRY_MS;
      if (!pnc_binary_mode) Serial.println("[PNC] RadiaCode clock set failed");
    }
  }

  // Always drop the link: the device keeps logging on its own, and a live link
  // is what upsets the SD path on this board.
  radiacode_ble_disconnect();
  if (!pnc_binary_mode) Serial.println("[PNC] RadiaCode released");
}
#endif

void sysvar_update() {
#if PNC_VERBOSE_LOG
  log_task("Starting SysVar Update...");
#endif
  static uint32_t last_retry_ms = 0;
  bool retry_now = (millis() - last_retry_ms > 10000);
  if (retry_now) {
    last_retry_ms = millis();
  }

  for (size_t i = 0; i < sensors_len; i++) {
    if (found_sensors[i]) {
      sensors[i]->readToSysVar();
    } else if (retry_now && sensors[i]->retryInit()) {
      found_sensors[i] = true;
      if (!pnc_binary_mode) {
        log_task("Late init: " + sensors[i]->getSensorName());
      }
    }
  }

#if PNC_VERBOSE_LOG
  log_task("Done.");
#endif
}

void store_data() {
#if PNC_VERBOSE_LOG
  log_task("store_data");
#endif

  float pico_temp_c = 0;
  uint32_t rtc_time = 0;
  BMESensorData bme_data = {};
  INASensorData ina_data = {};
  GPSSensorData gps_data = {};
  Packet packet = {};

  // Zero the checksum before summing so the transmitted byte is part of the
  // sum as 0, then set it to the two's complement of the total.
  int8_t sum = 0;

  // A timed-out mutex leaves the destination untouched; never publish
  // uninitialised stack data as a sensor value.
  if (sysvar_get_pico_temp_c(&pico_temp_c) != 0) pico_temp_c = 0;
  if (sysvar_get_rtc_time(&rtc_time) != 0) rtc_time = 0;
  if (sysvar_get_bme_data(&bme_data) != 0) bme_data = {};
  if (sysvar_get_ina_data(&ina_data) != 0) ina_data = {};
  if (sysvar_get_gps_data(&gps_data) != 0) gps_data = {};

  packet.sync_bytes = 0xDEADCAFE;
  packet.uptime = millis();
  packet.id = 0;
  packet.length = (uint8_t)sizeof(Packet);
  packet.temp_data = pico_temp_c;
  packet.rtc_time = rtc_time;
  packet.bme_data = bme_data;
  packet.ina_data = ina_data;
  packet.gps_data = gps_data;
  packet.checksum = 0;

  uint8_t* pos = (uint8_t*)&packet;
  for (size_t i = 0; i < sizeof(Packet); i++) {
    sum += pos[i];
  }
  packet.checksum = -sum;  // sum complement parity

  // Store on SD when available. Do not skip the serial output if the card is
  // missing: bench verification needs the sensor values either way.
#if PNC_ENABLE_SD
  if (!sd_status) {
    sd_setup();
  }
  if (sd_status && !sd_space_low) {
    watchdog_update();
    mutex_enter_blocking(&spi1_mutex);

    // Lazy open: only a boot that actually produces a packet creates a file.
    if (!sd_log_opened) {
      if (SD.exists(filename)) {
        log_task("SD target already exists; refusing to append: " + filename);
      } else {
        data_file = SD.open(filename, FILE_WRITE);
      }
      if (data_file) {
        sd_log_opened = true;
        log_task("Opened " + filename);
      } else {
        ErrorDisplay::instance().addCode(Error::SD_CARD_FAIL);
        sd_status = false;
        sd_remounts++;
        if (payload_file) payload_file.close();
        payload_log_ok = false;
        SD.end(false);
      }
    }

    size_t wrote = 0;
    if (sd_log_opened) {
      wrote = data_file.write((uint8_t*)&packet, sizeof(packet));
    }
    // Keep the file open (open/close per packet hammers FAT metadata and can
    // stall long enough for the external watchdog), flush about once every 20
    // packets to avoid partial-sector writes on a slow card.
    static uint8_t pending = 0;
    if (sd_log_opened && ++pending >= 20) {
      pending = 0;
      data_file.flush();
    }
    if (sd_log_opened && wrote != sizeof(packet)) {
      ErrorDisplay::instance().addCode(Error::SD_CARD_FAIL);
      data_file.close();
      sd_log_opened = false;
      sd_status = false;
      sd_remounts++;
      if (payload_file) payload_file.close();
      payload_log_ok = false;
      SD.end(false);
    }

    // Drain packets reassembled by the core 1 CAN task and append them to
    // PAYLOAD.BIN. Each record is:
    //   uint32 magic "PLOG" | uint32 unix_s | uint32 pnc_uptime_ms |
    //   uint8 node | uint8 length | uint8 data[length]
    if (sd_log_opened) {
    PayloadRecord record;
    while (payload_log_ok && queue_try_remove(&payload_queue, &record)) {
      uint8_t buffer[14 + CAN_PACKET_MAX_LEN];
      uint16_t offset = 0;
      const uint32_t magic = 0x474F4C50;  // bytes "PLOG"
      memcpy(buffer + offset, &magic, sizeof(magic));
      offset += sizeof(magic);
      memcpy(buffer + offset, &record.unix_s, sizeof(record.unix_s));
      offset += sizeof(record.unix_s);
      memcpy(buffer + offset, &record.uptime_ms, sizeof(record.uptime_ms));
      offset += sizeof(record.uptime_ms);
      buffer[offset++] = record.node;
      buffer[offset++] = record.length;
      memcpy(buffer + offset, record.data, record.length);
      offset += record.length;

      if (payload_file.write(buffer, offset) != offset) {
        payload_log_ok = false;
        payload_file.close();
        break;
      }
      static uint8_t payload_pending = 0;
      if (++payload_pending >= 16) {
        payload_pending = 0;
        payload_file.flush();
      }
      payload_packets_logged++;
    }
    }  // sd_log_opened

    mutex_exit(&spi1_mutex);
    watchdog_update();
  }
#endif  // PNC_ENABLE_SD

  if (pnc_binary_mode) {
    Serial.write((uint8_t*)&packet, sizeof(packet));
  } else {
    print_packet_csv(packet);
  }
}

extern "C" bool core1_separate_stack = true;
// core 1
// ---------------------------------------------------------------------------------------------
// monitor and watchdog
void setup1() {
  // Core 0 is still running setup() and has not started loop() yet, so keep the
  // external ATtiny85 watchdog fed from here until then.
  uint32_t start = millis();
  bool heartbeat = false;
  while (!pnc_setup_done && millis() - start < 10000) {
    heartbeat = !heartbeat;
    digitalWrite(WATCHDOG_HEARTBEAT_PIN, heartbeat);
    delay(100);
  }

  monitor_task_init();
  watchdog_task_init();
}

void loop1() {
  // A requested reboot must stop the watchdog feed, otherwise the 5 ms update
  // keeps the chip alive forever.
  if (reboot_requested) {
    while (1);
  }

  // Feed the watchdog before any call that can block on the shared SPI bus.
  // can_service() waits for core 0's SD writes, so feeding after it is too
  // late on a slow card.
  watchdog_update();

  // Fast path: drain the MCP2515 before its two RX buffers overflow.
  can_service();

  // Feed the external ATtiny85 watchdog at 2 Hz from the fast loop. Do not
  // toggle it every 5 ms: the ATtiny expects a slow heartbeat, not a square
  // wave at the loop rate.
  static bool heartbeat = false;
  static uint32_t last_heartbeat = 0;
  if (millis() - last_heartbeat >= 500) {
    last_heartbeat = millis();
    heartbeat = !heartbeat;
    digitalWrite(WATCHDOG_HEARTBEAT_PIN, heartbeat);
  }

  // Slow path: 1 Hz housekeeping and serial monitor.
  static uint32_t last_slow = 0;
  if (millis() - last_slow >= 1000) {
    last_slow = millis();
    watchdog_task();
    monitor_task();
  }

  delay(5);
}
