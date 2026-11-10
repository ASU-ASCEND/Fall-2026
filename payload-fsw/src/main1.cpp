/**
 * @file main1.cpp
 * @brief Main functions for Core 1, responsible for sending data to storage
 * peripherals
 */
#include <Arduino.h>

#include "hardware/watchdog.h"

// error code framework
#include "ErrorDisplay.h"
#include "Logger.h"
#include "PayloadConfig.h"
#include "Storage.h"

int verifyStorage();
int verifyStorageRecovery();
void storeData(String data);
void storeDataPacket(uint8_t* packet);

// include storage headers here
#if PAYLOAD_ENABLE_SD
#include "SDStorage.h"

// storage classes
SDStorage sd_storage;

// storage array
Storage* storages[] = {&sd_storage};
const int storages_len = 1;
#else
// No local storage: the PnC logs this board over CAN.
Storage* storages[] = {nullptr};
const int storages_len = 0;
#endif

// Global variables shared with core 0
extern queue_t qt;
extern volatile bool sd_summary_request;
extern volatile bool payload_sd_ok;
extern volatile uint32_t payload_sd_bytes;

// separate 8k stacks
bool core1_separate_stack = true;

/**
 * @brief Setup for core 1
 *
 *
 */
void real_setup1() {
  delay(500);  // wait for other setup to run
  log_core("Setup1 begin");

  // set up heartbeat
  pinMode(HEARTBEAT_PIN_1, OUTPUT);

#if PAYLOAD_ENABLE_SD
  // set storages to be tried forever, with 1 second recovery factor
  for (size_t i = 0; i < storages_len; i++) {
    storages[i]->recoveryConfig(-1, 1000);
  }

  // verify storage
  log_core("Verifying storage...");
  int verified_count = verifyStorageRecovery();
  if (verified_count == 0) {
    log_core("No storages verified, output will be Serial only.");
    ErrorDisplay::instance().addCode(Error::CRITICAL_FAIL);
  }

  payload_sd_ok = sd_storage.isOpen();
#else
  log_core("Local SD disabled; the PnC logs this board over CAN");
  payload_sd_ok = false;
#endif

  delay(500);  // wait for other setup to run
  watchdog_enable(8000, true);
}

int it2 = 0;
uint8_t received_data[QT_ENTRY_SIZE];
/**
 * @brief Loop for core 1
 *
 */
void real_loop1() {
  // Feed the watchdog unconditionally. This used to sit inside the "queue has
  // data" branch, so any stall on core 0 (a slow SD write, a long sensor read,
  // a serial command that blocks) stopped the feed and rebooted the board
  // after 8 s even though core 1 was healthy.
  watchdog_update();

  // Block if data is in the queue
  if (queue_get_level(&qt) > 0) {
    // toggle heartbeat
    it2++;
    digitalWrite(HEARTBEAT_PIN_1, (it2 & 0x1));

    log_core("it2: " + String(it2));

    // Retrieve sensor data from queue
    queue_remove_blocking(&qt, received_data);

    unsigned long timestamp;
    memcpy(
        &timestamp,
        received_data + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint16_t),
        sizeof(timestamp));
    log_core("Packet Received with Millis = " + String(timestamp));

    // store csv row
    storeDataPacket(received_data);
  }

#if PAYLOAD_ENABLE_SD
  // A failed device is retried by the recovery system. Keep the "SD is
  // healthy" flag and byte count fresh for the core 0 status/CAN heartbeat.
  // fileSize() flushes, so only sample it about once a second.
  payload_sd_ok = sd_storage.attemptConnection();
  static uint32_t last_size_check = 0;
  if (millis() - last_size_check > 1000) {
    last_size_check = millis();
    payload_sd_bytes = sd_storage.fileSize();
  }
#endif

  if (sd_summary_request) {
    sd_summary_request = false;
#if PAYLOAD_ENABLE_SD
    if (sd_storage.isOpen()) {
      sd_storage.printSummary();
    } else {
      Serial.println("[PNC] SD not mounted");
    }
#else
    Serial.println("[PNC] local SD disabled (payload logs via PnC)");
#endif
  }

  // Prevent a busy loop
  delay(10);
}

/**
 * @brief Verifies the connection with each storage device, and defines the
 * header_condensed field, uses recovery system
 *
 * @return int The number of verified storage devices
 */
int verifyStorageRecovery() {
  int count = 0;
  for (int i = 0; i < storages_len; i++) {
    if (storages[i]->attemptConnection()) {
      log_core(storages[i]->getDeviceName() + " verified.");
      count++;
    } else {
      log_core(storages[i]->getDeviceName() + " NOT verified");
    }
  }
  return count;
}

/**
 * @brief Verifies the connection with each storage device, and defines the
 * header_condensed field
 *
 * @return int The number of verified storage devices
 */
int verifyStorage() {
  int count = 0;
  for (int i = 0; i < storages_len; i++) {
    if (storages[i]->attemptConnection()) {
      log_core(storages[i]->getDeviceName() + " verified.");
      count++;
    }
  }
  return count;
}

/**
 * @brief Sends data to each storage device, assumes storage devices take care
 * of newline/data end themselves
 *
 * @param data Data in a CSV formatted string
 */
void storeData(String data) {
  for (int i = 0; i < storages_len; i++) {
    if (storages[i]->attemptConnection()) {
      storages[i]->store(data);
    }
  }
}

/**
 * @brief Sends data to each storage device
 *
 * @param packet Pointer to packet bytes
 */
void storeDataPacket(uint8_t* packet) {
  for (int i = 0; i < storages_len; i++) {
    if (storages[i]->attemptConnection()) {
      storages[i]->storePacket(packet);
    }
  }
}
