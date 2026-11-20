#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

/**
 * @brief When true only sensor packets are written to the USB serial port.
 *
 * The human-readable log lines from both cores interleave with the binary
 * packets, which makes the stream hard to decode. 'B' on the serial port sets
 * this flag, 'T' clears it. It is defined in main.cpp.
 */
extern volatile bool payload_quiet;

// Every text line starts with "Payload": both boards share one log stream on
// the bench, so the prefix keeps them apart. Raw packet bytes are only
// written in binary mode, never mixed into the text log.
#define log_core_printf(fmt, ...)                          \
  do {                                                     \
    if (!payload_quiet) {                                  \
      Serial.printf("Payload [Core %d] " fmt, get_core_num(), \
                    ##__VA_ARGS__);                        \
    }                                                      \
  } while (0)

static inline void log_core(const String& str) {
  if (payload_quiet) return;
  Serial.printf("Payload [Core %d] %s\n", get_core_num(), str.c_str());
}

static inline void log_data_raw(const uint8_t* packet, const uint8_t len) {
  Serial.write((const char*)packet, len);
}

static inline void log_data_bytes(const uint8_t* packet, const uint8_t len) {
  if (payload_quiet) return;
  String packet_as_hex = "";
  for (size_t i = 0; i < len; i++) {
    packet_as_hex += String(packet[i], HEX) + " ";
  }
  packet_as_hex += "\n";
  Serial.print("Payload " + packet_as_hex);
}

static inline void log_data(String data) {
  if (payload_quiet) return;
  Serial.print("Payload [Data] " + data + "\n");
}

// Log flash-related data
static inline void log_flash(String data) {
  if (payload_quiet) return;
  Serial.print("Payload [Flash] " + data + '\n');
}

#endif  // LOGGER_H
