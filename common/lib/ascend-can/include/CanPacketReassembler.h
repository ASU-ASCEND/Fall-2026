#ifndef ASCEND_CAN_PACKET_REASSEMBLER_H
#define ASCEND_CAN_PACKET_REASSEMBLER_H

/**
 * @file CanPacketReassembler.h
 * @brief Rebuild board packets from PACKET fragments (0x200 + node).
 *
 * One slot per node (payload, external, science). Fragments must arrive in
 * order; a lost frame makes that packet fail and is counted in dropped(). The
 * next packet's fragment 0 starts a new attempt.
 */

#include <stdint.h>

#include "CanProtocol.h"

class CanPacketReassembler {
 public:
  static const uint8_t SLOTS = 4;

  /**
   * @brief Feed one fragment.
   * @param node source node id (1..3)
   * @param fragment received fragment
   * @param now_ms millis() at reception, used for the stall timeout
   * @return true when this fragment completed a packet
   */
  bool feed(uint8_t node, const CanPacketFragment& fragment, uint32_t now_ms);

  const uint8_t* packet() const { return this->packet_; }
  uint8_t length() const { return this->packet_len_; }
  uint8_t node() const { return this->packet_node_; }
  uint8_t sequence() const { return this->packet_seq_; }

  uint32_t completed() const { return this->completed_; }
  uint32_t dropped() const { return this->dropped_; }

 private:
  struct Slot {
    uint8_t buffer[CAN_PACKET_MAX_LEN];
    uint8_t length = 0;
    uint8_t expected = 0;
    uint8_t received = 0;
    uint8_t seq = 0;
    bool active = false;
    uint32_t last_ms = 0;
  };

  Slot slots_[SLOTS] = {};

  uint8_t packet_[CAN_PACKET_MAX_LEN] = {};
  uint8_t packet_len_ = 0;
  uint8_t packet_node_ = 0;
  uint8_t packet_seq_ = 0;
  uint32_t completed_ = 0;
  uint32_t dropped_ = 0;
};

#endif  // ASCEND_CAN_PACKET_REASSEMBLER_H
