#ifndef ASCEND_CAN_BUS_H
#define ASCEND_CAN_BUS_H

/**
 * @file CanBus.h
 * @brief MCP2515 transport + ASCEND CAN protocol handling.
 *
 * The MCP2515 is a classic-CAN controller on SPI. It has no hardware time
 * keeping, so this class keeps a UTC estimate from the PnC TIME_SYNC frames
 * against the local millis() clock.
 *
 * SPI note: the MCP2515 shares its SPI bus with anything else on that bus.
 * Call poll()/send() from the same core that drives the other devices on that
 * bus (see the board main.cpp).
 */

#include <Arduino.h>
#include <SPI.h>
#include <mcp2515.h>

#include "CanPacketReassembler.h"
#include "CanProtocol.h"

/** @brief Optional per-frame hook, called from poll(). */
typedef void (*CanFrameCallback)(uint16_t id, const uint8_t* data, uint8_t len);

/** @brief Optional completed-packet hook, called from poll(). */
typedef void (*CanPacketCallback)(uint8_t node, const uint8_t* data,
                                  uint8_t len);

/** @brief Board-specific wiring for the MCP2515/TJA1051 pair. */
struct CanBusConfig {
  SPIClass* spi;
  uint8_t cs_pin;
  uint8_t int_pin;    // 0xFF when not wired
  uint8_t reset_pin;  // 0xFF when not wired
  uint8_t stby_pin;   // 0xFF when not wired (TJA1051 S, low = normal)
  uint8_t node_id;
  CAN_SPEED bitrate;
  CAN_CLOCK clock;
};

/** @brief Result of one loopback self-test. */
struct CanLoopbackResult {
  bool ok;
  uint16_t echoed_id;
  uint8_t echoed_len;
  uint32_t sent_counter;
  uint32_t received_counter;
};

class CanBus {
 public:
  CanBus();

  /**
   * @brief Bring up the controller: reset, bitrate, filters, normal mode.
   * @return true if the MCP2515 answered and entered normal mode.
   */
  bool begin(const CanBusConfig& config);

  bool ready() const { return this->ready_; }

  /** @brief Drain the RX buffers and dispatch protocol frames. */
  void poll();

  /** @brief Send one standard-ID frame. */
  bool send(uint16_t id, const uint8_t* data, uint8_t len);

  template <typename T>
  bool sendStruct(uint16_t id, const T& payload) {
    static_assert(sizeof(T) <= 8, "CAN frame payload must fit in 8 bytes");
    return this->send(id, reinterpret_cast<const uint8_t*>(&payload), sizeof(T));
  }

  /** @brief Send a TIME_REQ (asks the PnC for the current UTC time). */
  bool requestTime();

  /**
   * @brief Fragment and send a raw board packet.
   * @param node source node (CAN_NODE_PAYLOAD / EXTERNAL / SCIENCE)
   * @param data packet bytes
   * @param len packet length, 1..255
   */
  bool sendPacket(uint8_t node, const uint8_t* data, uint8_t len);

  /** @brief True once a TIME_SYNC frame has been accepted. */
  bool hasTime() const { return this->time_valid_; }

  /** @brief UTC seconds at the last sync plus elapsed local time. */
  uint32_t utcUnix() const;

  /** @brief Milliseconds within the current UTC second (0..999). */
  uint16_t utcMillisOfSecond() const;

  /** @brief Milliseconds since the last accepted TIME_SYNC. */
  uint32_t timeSyncAgeMs() const;

  /** @brief Source of the last accepted TIME_SYNC (CanTimeSource). */
  uint8_t timeSource() const { return this->time_source_; }

  /** @brief Flags of the last accepted TIME_SYNC (CanTimeFlags). */
  uint8_t timeFlags() const { return this->time_flags_; }

  /** @brief Run an internal loopback test and restore normal mode. */
  CanLoopbackResult loopbackTest();

  /**
   * @brief Loop back a TIME_SYNC frame and check the decoded UTC estimate.
   * Proves the time protocol path without a second node on the bus.
   */
  bool timeLoopbackTest(uint32_t epoch_s);

  /**
   * @brief Send a synthetic packet through the controller in loopback and
   *        reassemble it. Proves fragmentation/reassembly without a peer.
   */
  bool packetLoopbackTest(uint32_t seed);

  /** @brief Last completely reassembled packet. */
  const uint8_t* lastPacket() const { return this->reassembler_.packet(); }
  uint8_t lastPacketLength() const { return this->reassembler_.length(); }
  uint8_t lastPacketNode() const { return this->reassembler_.node(); }
  uint32_t packetsReceived() const { return this->reassembler_.completed(); }
  uint32_t packetDrops() const { return this->reassembler_.dropped(); }
  uint32_t packetsSent() const { return this->packets_sent_; }

  void setFrameCallback(CanFrameCallback callback) { this->frame_callback_ = callback; }
  void setPacketCallback(CanPacketCallback callback) { this->packet_callback_ = callback; }

  uint32_t framesSent() const { return this->frames_sent_; }
  uint32_t framesReceived() const { return this->frames_received_; }
  uint32_t sendErrors() const { return this->send_errors_; }
  uint32_t readErrors() const { return this->read_errors_; }
  uint32_t timeSyncCount() const { return this->time_sync_count_; }
  uint32_t timeRequestCount() const { return this->time_request_count_; }
  uint32_t busResets() const { return this->bus_resets_; }
  uint8_t lastReceivedId() const { return this->last_rx_id_; }

 private:
  void handleFrame(uint16_t id, const uint8_t* data, uint8_t len);
  bool recoverBusOff();

  MCP2515* controller_ = nullptr;
  CanBusConfig config_ = {};
  bool ready_ = false;

  bool time_valid_ = false;
  uint32_t time_base_utc_s_ = 0;
  uint16_t time_base_ms_ = 0;
  uint32_t time_base_local_ms_ = 0;
  uint8_t time_source_ = 0;
  uint8_t time_flags_ = 0;

  CanFrameCallback frame_callback_ = nullptr;
  CanPacketCallback packet_callback_ = nullptr;

  CanPacketReassembler reassembler_ = {};
  uint8_t packet_seq_ = 0;
  uint32_t packets_sent_ = 0;

  uint32_t frames_sent_ = 0;
  uint32_t frames_received_ = 0;
  uint32_t send_errors_ = 0;
  uint32_t read_errors_ = 0;
  uint32_t time_sync_count_ = 0;
  uint32_t time_request_count_ = 0;
  uint32_t bus_resets_ = 0;
  uint32_t last_tx_ok_ms_ = 0;
  uint8_t last_rx_id_ = 0;
};

#endif  // ASCEND_CAN_BUS_H
