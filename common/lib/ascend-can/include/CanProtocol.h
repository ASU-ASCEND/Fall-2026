#ifndef ASCEND_CAN_PROTOCOL_H
#define ASCEND_CAN_PROTOCOL_H

/**
 * @file CanProtocol.h
 * @brief On-the-wire contract for the ASCEND CAN bus.
 *
 * Physical layer: classic CAN 2.0B, 500 kbit/s, 11-bit identifiers.
 * Multi-byte fields are little endian (same convention as the SD packets).
 * Every frame carries at most 8 data bytes.
 *
 * Identifier map (lower identifiers win arbitration):
 *
 *   0x010  TIME_SYNC    PnC -> everyone, 1 Hz, UTC time reference
 *   0x011  TIME_REQ     anyone -> PnC, on boot or after RTC loss
 *   0x020  HEARTBEAT    each node -> everyone, 1 Hz
 *   0x030  NODE_STATUS  each node -> everyone, 1 Hz
 *   0x100  IMU_SAMPLE   payload -> PnC, fragmented 4 x 8 bytes
 *   0x110  RADIATION    payload -> PnC, geiger CPS
 *   0x111  RAD_DOSE     payload -> PnC, geiger dose
 *   0x120  VALUE base   payload -> PnC, ID = 0x120 + channel (0..31)
 *   0x200  PACKET base  node -> PnC, fragmented raw board packet
 *                        ID = 0x200 + node (0x201 payload, 0x202 external,
 *                        0x203 science)
 *   0x7F0  TEST         any -> any, link/loopback self-test
 */

#include <stdint.h>

#define CAN_PROTOCOL_VERSION 1

/** @brief Node addresses used in HEARTBEAT and NODE_STATUS. */
enum CanNodeId : uint8_t {
  CAN_NODE_PNC = 0,
  CAN_NODE_PAYLOAD = 1,
  CAN_NODE_EXTERNAL = 2,
  CAN_NODE_SCIENCE = 3,
  CAN_NODE_BROADCAST = 0xFF,
};

/** @brief 11-bit message identifiers. */
enum CanId : uint16_t {
  CAN_ID_TIME_SYNC = 0x010,
  CAN_ID_TIME_REQ = 0x011,
  CAN_ID_HEARTBEAT = 0x020,
  CAN_ID_NODE_STATUS = 0x030,
  CAN_ID_IMU_SAMPLE = 0x100,
  CAN_ID_RADIATION = 0x110,
  CAN_ID_RAD_DOSE = 0x111,
  CAN_ID_VALUE_BASE = 0x120,
  CAN_ID_VALUE_LAST = 0x13F,
  CAN_ID_PACKET_BASE = 0x200,
  CAN_ID_PACKET_LAST = 0x203,
  CAN_ID_TEST = 0x7F0,
};

/** @brief Bytes of board packet carried in one fragment frame. */
#define CAN_PACKET_DATA_BYTES 4
/** @brief Largest board packet that fits in the length byte. */
#define CAN_PACKET_MAX_LEN 255
/** @brief Largest fragment count for a 255 byte packet. */
#define CAN_PACKET_MAX_FRAGS 64

/**
 * @brief Gap between packet fragments.
 *
 * The MCP2515 has two RX buffers. A 138-byte packet is 35 frames; sending them
 * back-to-back (about 260 us each at 500 kbit/s) overruns the receiver before
 * its service task can drain them. 4 ms keeps the burst below the receiver's
 * drain rate at the cost of ~140 ms per packet, which is fine at 2 Hz.
 */
#define CAN_PACKET_FRAGMENT_SPACING_MS 4

/** @brief Source of the timestamp carried in TIME_SYNC. */
enum CanTimeSource : uint8_t {
  CAN_TIME_SRC_RTC = 0,
  CAN_TIME_SRC_GPS = 1,
  CAN_TIME_SRC_HOST = 2,
  CAN_TIME_SRC_CAN = 3,
};

/** @brief Bits in the flags byte of TIME_SYNC. */
enum CanTimeFlags : uint8_t {
  CAN_TIME_FLAG_VALID = 1 << 0,
  CAN_TIME_FLAG_GPS_FIX = 1 << 1,
  CAN_TIME_FLAG_RTC_LOST_POWER = 1 << 2,
};

/** @brief Fragments of a single IMU sample, sent back to back. */
enum CanImuFragment : uint8_t {
  CAN_IMU_FRAG_ACCEL = 0,  // mg
  CAN_IMU_FRAG_GYRO = 1,   // 0.1 deg/s
  CAN_IMU_FRAG_MAG = 2,    // 0.1 uT
  CAN_IMU_FRAG_TEMP = 3,   // 0.01 C in field a
};

/** @brief VALUE channel numbers (message ID = CAN_ID_VALUE_BASE + channel). */
enum CanValueChannel : uint8_t {
  CAN_VALUE_PICO_TEMP_C = 0,
  CAN_VALUE_BME_TEMP_C = 1,
  CAN_VALUE_BME_PRESSURE_PA = 2,
  CAN_VALUE_BME_HUMIDITY_PCT = 3,
  CAN_VALUE_BME_GAS_OHM = 4,
  CAN_VALUE_TMP117_TEMP_C = 5,
  CAN_VALUE_GEIGER_CPS = 6,
  CAN_VALUE_GEIGER_DOSE = 7,
  CAN_VALUE_CHANNEL_COUNT = 32,
};

#pragma pack(push, 1)

/** @brief TIME_SYNC (0x010), DLC 8. */
struct CanTimeSync {
  uint32_t unix_s;
  uint16_t millis;
  uint8_t source;
  uint8_t flags;
};

/** @brief HEARTBEAT (0x020), DLC 8. */
struct CanHeartbeat {
  uint8_t node;
  uint8_t state;
  uint16_t seq;
  uint32_t uptime_ms;
};

/** @brief NODE_STATUS (0x030), DLC 8. */
struct CanNodeStatus {
  uint8_t node;
  uint8_t sensors_ok;
  uint8_t sensors_total;
  uint8_t error_code;
  uint32_t uptime_ms;
};

/** @brief IMU_SAMPLE (0x100), DLC 8. */
struct CanImuSample {
  uint8_t seq;
  uint8_t frag;
  int16_t a;
  int16_t b;
  int16_t c;
};

/** @brief RADIATION (0x110) and RAD_DOSE (0x111), DLC 8. */
struct CanValue {
  uint32_t uptime_ms;
  float value;
};

/** @brief VALUE (0x120 + channel), DLC 8. */
using CanChannelValue = CanValue;

/** @brief TEST (0x7F0): any pattern, used to prove the link end to end. */
struct CanTestFrame {
  uint32_t counter;
  uint8_t pattern[4];
};

/**
 * @brief PACKET fragment (0x200 + node), DLC 8.
 *
 * A board packet (for the payload that is the `ASU!` packet) is split into
 * ceil(length / 4) frames. `seq` increments per packet, `frag` is the 0-based
 * fragment index, `nfrag` is the fragment count and `length` is the exact
 * packet length. The last fragment is zero padded.
 */
struct CanPacketFragment {
  uint8_t seq;
  uint8_t frag;
  uint8_t nfrag;
  uint8_t length;
  uint8_t data[CAN_PACKET_DATA_BYTES];
};

#pragma pack(pop)

static_assert(sizeof(CanTimeSync) == 8, "TIME_SYNC must be 8 bytes");
static_assert(sizeof(CanHeartbeat) == 8, "HEARTBEAT must be 8 bytes");
static_assert(sizeof(CanNodeStatus) == 8, "NODE_STATUS must be 8 bytes");
static_assert(sizeof(CanImuSample) == 8, "IMU_SAMPLE must be 8 bytes");
static_assert(sizeof(CanValue) == 8, "VALUE must be 8 bytes");
static_assert(sizeof(CanTestFrame) == 8, "TEST must be 8 bytes");
static_assert(sizeof(CanPacketFragment) == 8, "PACKET fragment must be 8 bytes");

#endif  // ASCEND_CAN_PROTOCOL_H
