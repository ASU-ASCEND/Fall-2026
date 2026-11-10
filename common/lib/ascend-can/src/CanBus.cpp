#include "CanBus.h"

#include <cstring>

CanBus::CanBus() = default;

bool CanBus::begin(const CanBusConfig& config) {
  this->config_ = config;
  this->ready_ = false;

  // Hardware reset first: the MCP2515 needs a clean config mode. If the board
  // does not wire RESET, the SPI reset instruction below is enough.
  if (config.reset_pin != 0xFF) {
    pinMode(config.reset_pin, OUTPUT);
    digitalWrite(config.reset_pin, LOW);
    delay(5);
    digitalWrite(config.reset_pin, HIGH);
    delay(10);
  }

  // TJA1051 S pin: low is normal mode, high is silent. Boards that strap it
  // with a jumper leave stby_pin at 0xFF.
  if (config.stby_pin != 0xFF) {
    pinMode(config.stby_pin, OUTPUT);
    digitalWrite(config.stby_pin, LOW);
  }

  if (config.int_pin != 0xFF) {
    pinMode(config.int_pin, INPUT_PULLUP);
  }

  if (this->controller_ == nullptr) {
    this->controller_ =
        new MCP2515(config.cs_pin, 10000000, config.spi);
  }
  if (this->controller_ == nullptr) {
    return false;
  }

  if (this->controller_->reset() != MCP2515::ERROR_OK) {
    return false;
  }
  if (this->controller_->setBitrate(config.bitrate, config.clock) !=
      MCP2515::ERROR_OK) {
    return false;
  }
  // Accept every standard frame; filtering is done in software so new message
  // IDs do not need a controller reflash.
  if (this->controller_->setNormalMode() != MCP2515::ERROR_OK) {
    return false;
  }

  this->ready_ = true;
  this->last_tx_ok_ms_ = millis();
  return true;
}

bool CanBus::send(uint16_t id, const uint8_t* data, uint8_t len) {
  if (!this->ready_) {
    return false;
  }
  if (len > 8) {
    len = 8;
  }

  struct can_frame frame;
  frame.can_id = id;
  frame.can_dlc = len;
  memset(frame.data, 0, sizeof(frame.data));
  if (data != nullptr) {
    memcpy(frame.data, data, len);
  }

  // The controller has three TX buffers. Retrying briefly covers the case
  // where all three are still pending from the previous burst.
  for (int attempt = 0; attempt < 3; attempt++) {
    MCP2515::ERROR result = this->controller_->sendMessage(&frame);
    if (result == MCP2515::ERROR_OK) {
      this->frames_sent_++;
      this->last_tx_ok_ms_ = millis();
      return true;
    }
    if (result != MCP2515::ERROR_ALLTXBUSY) {
      break;
    }
    delay(1);
  }

  this->send_errors_++;
  return false;
}

void CanBus::poll() {
  if (!this->ready_) {
    return;
  }

  // Bound the work per call so a busy bus cannot starve the caller.
  for (int i = 0; i < 8; i++) {
    struct can_frame frame;
    MCP2515::ERROR result = this->controller_->readMessage(&frame);
    if (result == MCP2515::ERROR_NOMSG) {
      break;
    }
    if (result != MCP2515::ERROR_OK) {
      this->read_errors_++;
      break;
    }
    this->handleFrame((uint16_t)(frame.can_id & 0x7FF), frame.data,
                      frame.can_dlc > 8 ? 8 : frame.can_dlc);
  }

  // A node alone on the bus (or with a broken transceiver link) has its three
  // TX buffers retry forever because nothing acknowledges the frames. The
  // controller may never reach bus-off in that state, so also recover when
  // there has been no successful transmit for a while.
  bool tx_stalled = this->send_errors_ > 0 &&
                    (millis() - this->last_tx_ok_ms_) > 10000;
  if (tx_stalled ||
      (this->controller_->getErrorFlags() & MCP2515::EFLG_TXBO) != 0) {
    this->recoverBusOff();
  }
}

bool CanBus::recoverBusOff() {
  if (!this->ready_) {
    return false;
  }
  if (this->controller_->reset() != MCP2515::ERROR_OK) {
    return false;
  }
  if (this->controller_->setBitrate(this->config_.bitrate,
                                   this->config_.clock) != MCP2515::ERROR_OK) {
    return false;
  }
  this->bus_resets_++;
  this->last_tx_ok_ms_ = millis();
  return this->controller_->setNormalMode() == MCP2515::ERROR_OK;
}

void CanBus::handleFrame(uint16_t id, const uint8_t* data, uint8_t len) {
  this->frames_received_++;
  this->last_rx_id_ = (uint8_t)id;

  if (id >= CAN_ID_PACKET_BASE && id <= CAN_ID_PACKET_LAST &&
      len >= sizeof(CanPacketFragment)) {
    CanPacketFragment fragment;
    memcpy(&fragment, data, sizeof(fragment));
    uint8_t node = (uint8_t)(id - CAN_ID_PACKET_BASE);
    if (this->reassembler_.feed(node, fragment, millis())) {
      if (this->packet_callback_ != nullptr) {
        this->packet_callback_(node, this->reassembler_.packet(),
                               this->reassembler_.length());
      }
    }
  }

  switch (id) {
    case CAN_ID_TIME_SYNC: {
      if (len < sizeof(CanTimeSync)) {
        break;
      }
      CanTimeSync sync;
      memcpy(&sync, data, sizeof(sync));
      if ((sync.flags & CAN_TIME_FLAG_VALID) == 0) {
        break;
      }
      this->time_base_utc_s_ = sync.unix_s;
      this->time_base_ms_ = sync.millis;
      this->time_base_local_ms_ = millis();
      this->time_source_ = sync.source;
      this->time_flags_ = sync.flags;
      this->time_valid_ = true;
      this->time_sync_count_++;
      break;
    }
    case CAN_ID_TIME_REQ:
      this->time_request_count_++;
      break;
    default:
      break;
  }

  if (this->frame_callback_ != nullptr) {
    this->frame_callback_(id, data, len);
  }
}

uint32_t CanBus::utcUnix() const {
  if (!this->time_valid_) {
    return 0;
  }
  uint32_t elapsed_ms = millis() - this->time_base_local_ms_;
  uint32_t total_ms = (uint32_t)this->time_base_ms_ + elapsed_ms;
  return this->time_base_utc_s_ + total_ms / 1000U;
}

uint16_t CanBus::utcMillisOfSecond() const {
  if (!this->time_valid_) {
    return 0;
  }
  uint32_t elapsed_ms = millis() - this->time_base_local_ms_;
  return (uint16_t)(((uint32_t)this->time_base_ms_ + elapsed_ms) % 1000U);
}

uint32_t CanBus::timeSyncAgeMs() const {
  if (!this->time_valid_) {
    return 0xFFFFFFFFUL;
  }
  return millis() - this->time_base_local_ms_;
}

CanLoopbackResult CanBus::loopbackTest() {
  CanLoopbackResult result = {};
  if (!this->ready_) {
    return result;
  }

  // A controller that has been alone on the bus can be in error-passive or
  // bus-off, where a plain REQOP write is not enough. Reset and reconfigure.
  if (this->controller_->reset() != MCP2515::ERROR_OK ||
      this->controller_->setBitrate(this->config_.bitrate, this->config_.clock) !=
          MCP2515::ERROR_OK ||
      this->controller_->setLoopbackMode() != MCP2515::ERROR_OK) {
    this->controller_->reset();
    this->controller_->setBitrate(this->config_.bitrate, this->config_.clock);
    this->controller_->setNormalMode();
    return result;
  }

  CanTestFrame test = {};
  test.counter = millis();
  test.pattern[0] = 0xA5;
  test.pattern[1] = 0x5A;
  test.pattern[2] = 0x3C;
  test.pattern[3] = 0xC3;
  result.sent_counter = test.counter;

  struct can_frame frame;
  frame.can_id = CAN_ID_TEST;
  frame.can_dlc = sizeof(CanTestFrame);
  memcpy(frame.data, &test, sizeof(test));

  if (this->controller_->sendMessage(&frame) != MCP2515::ERROR_OK) {
    this->controller_->setNormalMode();
    return result;
  }

  for (int attempt = 0; attempt < 20; attempt++) {
    struct can_frame rx;
    if (this->controller_->readMessage(&rx) == MCP2515::ERROR_OK) {
      CanTestFrame echoed;
      memcpy(&echoed, rx.data, sizeof(echoed));
      result.ok = (rx.can_id & 0x7FF) == CAN_ID_TEST &&
                  rx.can_dlc == sizeof(CanTestFrame) &&
                  echoed.counter == test.counter &&
                  memcmp(echoed.pattern, test.pattern, sizeof(test.pattern)) == 0;
      result.echoed_id = (uint16_t)(rx.can_id & 0x7FF);
      result.echoed_len = rx.can_dlc;
      result.received_counter = echoed.counter;
      break;
    }
    delay(2);
  }

  this->controller_->setNormalMode();
  return result;
}

bool CanBus::requestTime() {
  return this->send(CAN_ID_TIME_REQ, nullptr, 0);
}

bool CanBus::sendPacket(uint8_t node, const uint8_t* data, uint8_t len) {
  if (!this->ready_ || data == nullptr || len == 0) {
    return false;
  }

  const uint8_t nfrag =
      (uint8_t)((len + CAN_PACKET_DATA_BYTES - 1) / CAN_PACKET_DATA_BYTES);
  if (nfrag > CAN_PACKET_MAX_FRAGS) {
    return false;
  }

  const uint8_t seq = this->packet_seq_++;

  for (uint8_t fragment = 0; fragment < nfrag; fragment++) {
    CanPacketFragment frame = {};
    frame.seq = seq;
    frame.frag = fragment;
    frame.nfrag = nfrag;
    frame.length = len;

    const uint16_t offset = (uint16_t)fragment * CAN_PACKET_DATA_BYTES;
    uint8_t count = len - offset;
    if (count > CAN_PACKET_DATA_BYTES) {
      count = CAN_PACKET_DATA_BYTES;
    }
    memcpy(frame.data, data + offset, count);

    if (!this->send((uint16_t)(CAN_ID_PACKET_BASE + node),
                    reinterpret_cast<const uint8_t*>(&frame), sizeof(frame))) {
      return false;
    }
    if (fragment + 1 < nfrag) {
      delay(CAN_PACKET_FRAGMENT_SPACING_MS);
    }
  }

  this->packets_sent_++;
  return true;
}

bool CanBus::packetLoopbackTest(uint32_t seed) {
  if (!this->ready_) {
    return false;
  }

  if (this->controller_->reset() != MCP2515::ERROR_OK ||
      this->controller_->setBitrate(this->config_.bitrate, this->config_.clock) !=
          MCP2515::ERROR_OK ||
      this->controller_->setLoopbackMode() != MCP2515::ERROR_OK) {
    this->controller_->setNormalMode();
    return false;
  }

  // Use a multi-fragment packet so the test exercises the real path.
  uint8_t packet[96];
  const uint8_t len = sizeof(packet);
  packet[0] = 'A';
  packet[1] = 'S';
  packet[2] = 'U';
  packet[3] = '!';
  for (uint16_t i = 4; i < len; i++) {
    packet[i] = (uint8_t)(seed + i * 7u);
  }

  const uint32_t completed_before = this->reassembler_.completed();
  const uint8_t nfrag =
      (uint8_t)((len + CAN_PACKET_DATA_BYTES - 1) / CAN_PACKET_DATA_BYTES);
  const uint8_t seq = this->packet_seq_++;
  bool ok = false;

  // Send one fragment, then poll the looped-back copy immediately. Sending all
  // fragments first would overrun the two MCP2515 RX buffers, which is exactly
  // the failure this test is meant to catch.
  for (uint8_t fragment = 0; fragment < nfrag; fragment++) {
    CanPacketFragment frame = {};
    frame.seq = seq;
    frame.frag = fragment;
    frame.nfrag = nfrag;
    frame.length = len;
    const uint16_t offset = (uint16_t)fragment * CAN_PACKET_DATA_BYTES;
    uint8_t count = len - offset;
    if (count > CAN_PACKET_DATA_BYTES) count = CAN_PACKET_DATA_BYTES;
    memcpy(frame.data, packet + offset, count);

    if (!this->send((uint16_t)(CAN_ID_PACKET_BASE + CAN_NODE_PAYLOAD),
                    reinterpret_cast<const uint8_t*>(&frame), sizeof(frame))) {
      break;
    }
    delay(2);
    this->poll();
    if (this->reassembler_.completed() != completed_before) {
      ok = this->reassembler_.length() == len &&
           memcmp(this->reassembler_.packet(), packet, len) == 0;
      break;
    }
  }

  this->controller_->reset();
  this->controller_->setBitrate(this->config_.bitrate, this->config_.clock);
  this->controller_->setNormalMode();
  this->last_tx_ok_ms_ = millis();
  return ok;
}

bool CanBus::timeLoopbackTest(uint32_t epoch_s) {
  if (!this->ready_) {
    return false;
  }

  // Save the live time base: this test uses a fake epoch and must not leave
  // the node believing it is 2023 afterwards.
  const bool saved_valid = this->time_valid_;
  const uint32_t saved_utc = this->time_base_utc_s_;
  const uint16_t saved_ms = this->time_base_ms_;
  const uint32_t saved_local = this->time_base_local_ms_;
  const uint8_t saved_source = this->time_source_;
  const uint8_t saved_flags = this->time_flags_;
  const uint32_t saved_count = this->time_sync_count_;

  if (this->controller_->reset() != MCP2515::ERROR_OK ||
      this->controller_->setBitrate(this->config_.bitrate, this->config_.clock) !=
          MCP2515::ERROR_OK ||
      this->controller_->setLoopbackMode() != MCP2515::ERROR_OK) {
    this->controller_->setNormalMode();
    return false;
  }

  CanTimeSync sync = {};
  sync.unix_s = epoch_s;
  sync.millis = 123;
  sync.source = CAN_TIME_SRC_HOST;
  sync.flags = CAN_TIME_FLAG_VALID;

  bool ok = false;
  if (this->sendStruct(CAN_ID_TIME_SYNC, sync)) {
    for (int attempt = 0; attempt < 20; attempt++) {
      this->poll();
      if (this->time_valid_ && this->utcUnix() >= epoch_s) {
        // utcUnix() may have advanced up to ~50 ms since the frame was built.
        ok = (this->utcUnix() - epoch_s) <= 5;
        break;
      }
      delay(2);
    }
  }

  this->controller_->reset();
  this->controller_->setBitrate(this->config_.bitrate, this->config_.clock);
  this->controller_->setNormalMode();
  this->last_tx_ok_ms_ = millis();

  this->time_valid_ = saved_valid;
  this->time_base_utc_s_ = saved_utc;
  this->time_base_ms_ = saved_ms;
  this->time_base_local_ms_ = saved_local;
  this->time_source_ = saved_source;
  this->time_flags_ = saved_flags;
  this->time_sync_count_ = saved_count;
  return ok;
}
