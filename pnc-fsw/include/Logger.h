#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <stdarg.h>

// Every line this board prints starts with "PNC": both boards share one log
// stream on the bench, so the prefix keeps them apart. Packet bytes are only
// written in binary mode ('B'), never mixed into the text log.
#define log_printf(...) Serial.printf("PNC " __VA_ARGS__)

#define log_task_printf(fmt, ...) \
  Serial.printf("PNC [Core %d] " fmt, get_core_num(), ##__VA_ARGS__)

static inline void log_task(String str) {
  Serial.print("PNC [" + String(get_core_num()) + "] " + str + "\n");
}

static inline void log_task_error(String str) {
  Serial.print("PNC [ERROR - " + String(get_core_num()) + "] " + str + "\n");
}

static inline void log_data_raw(const uint8_t* packet, const uint8_t len) {
  Serial.write((const char*)packet, len);
}

static inline void log_data_bytes(const uint8_t* packet, const uint8_t len) {
  String packet_as_hex = "";
  for (size_t i = 0; i < len; i++) {
    packet_as_hex += String(packet[i], HEX) + " ";
  }
  packet_as_hex += "\n";
  Serial.print("PNC " + packet_as_hex);
}

static inline void log_data(String data) {
  Serial.print("PNC [Data] " + data + "\n");
}

#endif  // LOGGER_H
