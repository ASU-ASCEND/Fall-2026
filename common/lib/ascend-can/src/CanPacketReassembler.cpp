#include "CanPacketReassembler.h"

#include <cstring>

bool CanPacketReassembler::feed(uint8_t node,
                                const CanPacketFragment& fragment,
                                uint32_t now_ms) {
  if (fragment.nfrag == 0 || fragment.frag >= fragment.nfrag ||
      fragment.length == 0) {
    this->dropped_++;
    return false;
  }

  Slot& slot = this->slots_[node & (SLOTS - 1)];

  // A packet that stalls (lost frame, bus reset) must not block the slot
  // forever.
  if (slot.active && (uint32_t)(now_ms - slot.last_ms) > 1000) {
    slot.active = false;
    this->dropped_++;
  }

  if (fragment.frag == 0) {
    slot.active = true;
    slot.seq = fragment.seq;
    slot.expected = fragment.nfrag;
    slot.received = 0;
    slot.length = fragment.length;
    slot.last_ms = now_ms;
  } else if (!slot.active) {
    // We joined mid-packet. Wait for the next fragment 0.
    this->dropped_++;
    return false;
  }

  if (!slot.active || fragment.seq != slot.seq ||
      fragment.nfrag != slot.expected || fragment.length != slot.length ||
      fragment.frag != slot.received) {
    slot.active = false;
    this->dropped_++;
    return false;
  }

  const uint16_t offset = (uint16_t)fragment.frag * CAN_PACKET_DATA_BYTES;
  if (offset >= slot.length) {
    slot.active = false;
    this->dropped_++;
    return false;
  }

  uint8_t count = slot.length - offset;
  if (count > CAN_PACKET_DATA_BYTES) {
    count = CAN_PACKET_DATA_BYTES;
  }
  memcpy(slot.buffer + offset, fragment.data, count);

  slot.received++;
  slot.last_ms = now_ms;

  if (slot.received == slot.expected) {
    memcpy(this->packet_, slot.buffer, slot.length);
    this->packet_len_ = slot.length;
    this->packet_node_ = node;
    this->packet_seq_ = slot.seq;
    this->completed_++;
    slot.active = false;
    return true;
  }

  return false;
}
