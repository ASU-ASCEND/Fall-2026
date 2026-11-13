# ASCEND CAN Protocol

Version 1. This is the contract between the PnC board (bus master) and the
payload (Internal) board. The External and Science boards are reserved node
IDs and are not implemented yet.

Code lives in `common/lib/ascend-can/` and is compiled into both
`pnc-fsw` and `payload-fsw` through `lib_extra_dirs = ../common/lib`.

## Physical layer

| Item | Value |
|---|---|
| Bus | Classic CAN 2.0B, 11-bit identifiers |
| Bit rate | 500 kbit/s |
| Controller | MCP2515, 16 MHz crystal |
| Transceiver | TJA1051T/3, 5 V supply, 3.3 V I/O |
| Termination | 120 Ω at each physical end of the bus |
| Byte order | Little endian for every multi-byte field |

Every frame is 8 data bytes or shorter. There is no fragmentation layer beyond
the fixed IMU fragments described below.

## Node IDs

| ID | Node | Role |
|---|---|---|
| 0 | PnC | Time authority, GPS/RTC, radio, SD |
| 1 | Payload (Internal) | IMU, RTC, External sensors on I2C1, SD |
| 2 | External | reserved |
| 3 | Science | reserved |
| 255 | broadcast | never used as a source |

## Time model

The PnC is the mission time authority. It has the PCF8523 RTC and the GPS, so
it broadcasts `TIME_SYNC` once a second. A node that boots without a valid time
sends `TIME_REQ`; the PnC answers immediately.

Receivers keep the UTC estimate as `(unix_s, millis)` plus the local
`millis()` at the moment the frame arrived, so the estimate keeps advancing
between frames. If no sync arrives for 5 s, the estimate is stale but still
monotonic.

The payload uses CAN time to set its own RTC only when the RTC has lost power
or reports an implausible year. It never overwrites a healthy RTC every second.

## Frames

| ID | Name | Direction | DLC | Rate |
|---|---|---|---|---|
| `0x010` | `TIME_SYNC` | PnC → all | 8 | 1 Hz |
| `0x011` | `TIME_REQ` | any → PnC | 0 | on boot, retry 5 s |
| `0x020` | `HEARTBEAT` | node → all | 8 | 1 Hz |
| `0x030` | `NODE_STATUS` | node → all | 8 | 1 Hz |
| `0x100` | `IMU_SAMPLE` | payload → PnC | 8 | 1 Hz, 4 fragments |
| `0x110` | `RADIATION` | payload → PnC | 8 | 1 Hz |
| `0x111` | `RAD_DOSE` | payload → PnC | 8 | 1 Hz |
| `0x120+ch` | `VALUE` | payload → PnC | 8 | 1 Hz per channel |
| `0x200+node` | `PACKET` | node → PnC | 8 | per sample |
| `0x7F0` | `TEST` | any | 8 | on demand |

### `0x010` TIME_SYNC

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | `u32` | `unix_s` | UTC seconds |
| 4 | `u16` | `millis` | milliseconds into that second, 0..999 |
| 6 | `u8` | `source` | 0 = RTC, 1 = GPS, 2 = host, 3 = CAN |
| 7 | `u8` | `flags` | bit0 valid, bit1 GPS fix, bit2 RTC lost power |

`flags.valid` must be set or receivers ignore the frame. A GPS-sourced frame is
only sent when the receiver has a 2D/3D fix, so it is traceable to UTC.

### `0x011` TIME_REQ

DLC 0. Sent on boot and every 5 s while the local time is invalid. The PnC
answers with `TIME_SYNC` immediately; it also broadcasts once a second
regardless.

### `0x020` HEARTBEAT

| Offset | Type | Field |
|---|---|---|
| 0 | `u8` | `node` |
| 1 | `u8` | `state` |
| 2 | `u16` | `seq` (wraps) |
| 4 | `u32` | `uptime_ms` |

Payload `state` bits: 0 = RTC verified, 1 = SD mounted, 3 = CAN time valid.

### `0x030` NODE_STATUS

| Offset | Type | Field |
|---|---|---|
| 0 | `u8` | `node` |
| 1 | `u8` | `sensors_ok` |
| 2 | `u8` | `sensors_total` |
| 3 | `u8` | `error_code` (matches `ErrorDisplay` enum) |
| 4 | `u32` | `uptime_ms` |

### `0x100` IMU_SAMPLE

One IMU sample is sent as four back-to-back frames with the same `seq`:

| Offset | Type | Field |
|---|---|---|
| 0 | `u8` | `seq` |
| 1 | `u8` | `frag` |
| 2 | `i16` | `a` |
| 4 | `i16` | `b` |
| 6 | `i16` | `c` |

| frag | Meaning | Scale |
|---|---|---|
| 0 | accelerometer X/Y/Z | mg (1 g = 1000) |
| 1 | gyroscope X/Y/Z | 0.1 deg/s |
| 2 | magnetometer X/Y/Z | 0.1 µT |
| 3 | temperature in `a` | 0.01 °C, `b`/`c` = 0 |

### `0x110` RADIATION and `0x111` RAD_DOSE

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | `uptime_ms` |
| 4 | `f32` | `cps` or `dose` (µSv/h) |

### `0x120 + channel` VALUE

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | `uptime_ms` |
| 4 | `f32` | `value` |

Channel numbers:

| Channel | ID | Quantity | Unit |
|---|---|---|---|
| 0 | `0x120` | Pico temperature | °C |
| 1 | `0x121` | BME688 temperature | °C |
| 2 | `0x122` | BME688 pressure | Pa |
| 3 | `0x123` | BME688 humidity | %RH |
| 4 | `0x124` | BME688 gas resistance | Ω |
| 5 | `0x125` | TMP117 temperature | °C |
| 6 | `0x126` | Geiger count rate | cps |
| 7 | `0x127` | Geiger dose rate | µSv/h |

Channels 8..31 are free. `float` is fine for every value above; do not put a
Unix timestamp in a `VALUE` frame because a 32-bit `float` cannot represent
2026-era seconds precisely. Use `TIME_SYNC` for time.

The payload does not emit `VALUE` frames today: heartbeats, node status and the
`PACKET` fragments carry everything. Channels 6 and 7 are reserved for the
Geiger, which is compiled out (`PAYLOAD_ENABLE_GEIGER 0`).

### `0x200 + node` PACKET (fragmented board packet)

This is the "full data" path. A board builds its normal binary packet (the
payload's `ASU!` packet) and splits it into 8-byte CAN frames. ID = `0x200 +
node`: `0x201` payload, `0x202` external, `0x203` science.

| Offset | Type | Field |
|---|---|---|
| 0 | `u8` | `seq` (increments per packet, wraps) |
| 1 | `u8` | `frag` (0-based fragment index) |
| 2 | `u8` | `nfrag` (total fragments, 1..64) |
| 3 | `u8` | `length` (exact packet length, 1..255) |
| 4 | `u8[4]` | data bytes |

`nfrag = ceil(length / 4)`. The last fragment is zero padded. The receiver
keeps one reassembly slot per node, requires fragments in order, and drops a
packet if a frame is lost. A slot with no progress for 1 s is released. The
next packet's `frag = 0` always starts a new attempt.

The sender paces fragments by `CAN_PACKET_FRAGMENT_SPACING_MS` (4 ms). A
138-byte packet is 35 frames, so a burst takes ~140 ms. Sending the fragments
back-to-back at 500 kbit/s (about 260 µs each) overruns the receiver's two RX
buffers; the pacing keeps each frame within the receiver's drain rate. At 2 Hz
that is about 70 frames/s, roughly 2 % of the bus.

**Receive timing matters.** The MCP2515 has only two RX buffers. The PnC
services CAN from a core 1 task every 5–10 ms; polling from the 0.6 s sensor
loop would lose most frames.

Reliability is best-effort: there is no application ACK or retransmit. The
`seq` and the packet's own checksum let the receiver detect and discard a bad
reassembly. Keep a local log if the data must survive a bus failure.

### `0x7F0` TEST

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | counter |
| 4 | `u8[4]` | pattern |

Used by `K` (controller loopback) and `Y` (time-sync loopback).

## Timing and bus load

At 500 kbit/s one standard frame is roughly 130–150 bits including stuffing, so
about 3,500 frames/s absolute maximum. The nominal schedule above is:

- PnC: 1 frame/s (`TIME_SYNC`; extra syncs only on `TIME_REQ`).
- Payload: one 138-byte packet per loop (35 frames × 2 Hz = 70 frames/s),
  plus heartbeat and status.

That is about 2 % bus load. A 255-byte packet at 10 Hz would be 640 frames/s
(about 18 %) and would need the PnC to keep up; the fast task has the headroom,
the SD write path does not.

The PnC does not currently broadcast its own heartbeat/status; add it there if
the ground station needs PnC health on the bus.

## PnC payload log

The PnC writes every reassembled packet to `PAYLOAD.BIN` on its own microSD,
timestamped with its RTC/GPS time. Records are self-describing:

| Offset | Type | Field |
|---|---|---|
| 0 | `u32` | magic `0x474F4C50` ("PLOG" in the file) |
| 4 | `u32` | PnC UTC seconds at reception (0 if unknown) |
| 8 | `u32` | PnC `millis()` at reception |
| 12 | `u8` | source node |
| 13 | `u8` | embedded packet length |
| 14 | `u8[]` | embedded board packet |

Decode with `packet-decoder/parse_payload_log.py`.

## Error handling

- MCP2515 error counters and bus-off are recovered by resetting the controller
  and re-entering normal mode. A node alone on the bus (no ACK) also triggers a
  recovery if no transmit has succeeded for 10 s.
- The driver accepts every standard frame in software; there is no hardware
  filter to update when a message ID is added.
- `TIME_SYNC` with `flags.valid = 0` is ignored.

## Board wiring

| Signal | PnC | Payload (Internal) |
|---|---|---|
| MCP2515 MISO (SO) | GP8 | GP12 |
| MCP2515 MOSI (SI) | GP11 | GP15 |
| MCP2515 SCK | GP10 | GP14 |
| MCP2515 CS | GP9 | GP13 |
| MCP2515 INT | GP12 | GP11 (via R104) |
| MCP2515 RESET | GP7 | GP10 (via R103) |
| TJA1051 standby | R21 10k to GND, `JMP_SIL?` to GP3 | GP9 (via R105, 0R) |

The PnC shares SPI1 with the microSD, so `CanBus::poll()`/`send()` must run on
the same core as the SD writes (core 0). The payload keeps CAN on SPI1 and the
SD on SPI0, so they are independent.

### Termination

| Board | Termination | Notes |
|---|---|---|
| PnC | R22 = **120 Ω** across CAN_H/CAN_L | plus C27/C28 common-mode caps and U10 ESD |
| Payload | R300/R301 = **60 Ω + 60 Ω** split, C300 = 0.1 µF to GND | U301 ESD |

Two nodes in parallel give **60 Ω differential**, which is correct for a
2-node bus. The 0.1 µF sits at the split midpoint, so it filters common-mode
noise and does not load the differential pair.

**Do not terminate every board on a 3+ node bus.** With the External and
Science boards added, three 120 Ω terminations give 40 Ω and load the bus
heavily. Keep termination at the two physical ends only, or make R22 and
R300/R301 jumper-selectable.

### 6-pin Molex (P nC J4 ↔ payload J100)

| Pin | Signal |
|---|---|
| 1 | CAN_H |
| 2 | CAN_L |
| 3 | +1V8 |
| 4 | +3V3 |
| 5 | +5V |
| 6 | GND |

The PnC **generates** +5V (U3), +3V3 (U4) and +1V8 (U5) from the main battery
on P1. The payload consumes them. Consequences:

- The PnC's Pico USB only powers the Pico. With VBUS present, Q1 isolates
  `INA_OUT` from VSYS, so the board rails still need the PnC battery. If the
  battery is disconnected or flat, both boards keep enumerating on USB but
  lose CAN, SD, sensors and the IMU.
- The payload's +5V net is shared with its Pico VSYS through Q100. Q100 is off
  when the payload's USB is plugged in, so the two USB supplies do not fight.
  With the payload USB unplugged, the PnC's +5V powers the payload's Pico.
- The payload's +3V3 and +1V8 are also brought out on J200/J201/J202/J203. Do
  not connect an external board that drives its own 3V3/1V8 there.

The CAN pair and the power pins share one unshielded 3 mm connector. That is
fine for a short bench cable, but use a twisted pair for CAN_H/CAN_L and keep
them separated from the power pair for flight.

### Known hardware caveat

In the FA26 Internal board netlist, MCP2515 `TXCAN` (U302 pin 1) and `RXCAN`
(U302 pin 2) are on isolated single-pin nets; they do not connect to the
TJA1051 `TXD`/`RXD` (U300 pins 1/4). The PnC netlist does connect them through
`CAN_TX`/`CAN_RX`. This means the payload board can pass controller loopback but
cannot put frames on the physical bus until those two links are added.

Continuity-test with the board unpowered:

- U302 pin 1 ↔ U300 pin 1
- U302 pin 2 ↔ U300 pin 4

If either is open, add a short wire. The physical board may be newer than the
Oct 2026 netlist, so confirm before reworking.

## Testing without a second node

Serial commands on both boards:

| Command | Effect |
|---|---|
| `K` | MCP2515 internal loopback, sends and receives a `TEST` frame |
| `Y` | Loops a `TIME_SYNC` through the controller and checks the decoded UTC |
| `L` | Fragments a 96-byte packet, loops it through the controller and reassembles it |
| `N` | Prints frame/packet counters and the last received ID |

Both boards should report `PASS` for `K`, `Y` and `L` before chasing wiring.
`L` exercises the exact fragmentation used for real telemetry.
