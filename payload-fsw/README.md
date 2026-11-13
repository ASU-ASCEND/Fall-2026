# Payload (Internal Board) Firmware

Firmware for the Fall 2026 Internal / payload board (StratoCore): Raspberry Pi
Pico 2W, PCF8523 RTC, ICM-20948 IMU, MCP2515 + TJA1051 CAN, microSD, and the
Geiger counter input.

The StratoSense (External) board is wired to I2C1 in the current bench setup, but
its drivers are still compiled out (`PAYLOAD_ENABLE_STRATOSENSE 0` in
`include/PayloadConfig.h`) until the failing parts described below are fixed.
The `E` / `W` serial commands test that board directly.

## Pin map

Taken from the Altium netlist generated with `altium-monkey`
(`PCBs/tools/extract_netlists.py`, output in `PCBs/netlists/`).

| Signal | Pico pin | Notes |
|---|---|---|
| I2C0 SDA | GP4 | PCF8523 RTC and ICM-20948 (through level shifter) |
| I2C0 SCL | GP5 | 10k + 5k pull-ups |
| I2C1 SDA | GP6 | StratoSense / external bus (J200/J201) |
| I2C1 SCL | GP7 | StratoSense / external bus |
| SPI0 MISO | GP16 | microSD J400 DAT0, 4.7k pull-up |
| SPI0 CS | GP17 | microSD DAT3 |
| SPI0 SCK | GP18 | microSD CLK |
| SPI0 MOSI | GP19 | microSD CMD |
| SPI1 MISO | GP12 | MCP2515 SO |
| SPI1 MOSI | GP15 | MCP2515 SI |
| SPI1 SCK | GP14 | MCP2515 SCK |
| CAN CS | GP13 | MCP2515, 10k pull-up |
| CAN INT | GP11 | MCP2515, through R104 (0R) |
| CAN RESET | GP10 | MCP2515, through R103 (0R) |
| CAN standby | GP9 | TJA1051 S, through R105 (0R), low = normal |
| Geiger input | GP22 | JGeig100 pin 2 |
| Onboard LED | GP25 | |
| Heartbeat 0/1 | GP21 / GP20 | not routed to LEDs on this board |

I2C addresses: PCF8523 `0x68` (fixed), ICM-20948 `0x68` with AD0/SDO low.

## Build and flash

The board is a Pico 2 W, so the `rpipico2w` env is the one to build and flash:

```sh
pio run -d payload-fsw -e rpipico2w
pio run -d payload-fsw -e rpipico2w -t upload --upload-port /dev/cu.usbmodemXXXX
```

The USB device path is not stable across reconnects and the two boards can
swap between `usbmodem101` and `usbmodem1101`. Identify the payload first: send
`I` and confirm I2C0 sees only `0x68` (the RTC; the IMU is absent on this
board), or look for `ASU!` packets. The PnC sees `0x40/0x42/0x68/0x76` and
prints `PKT,` lines.

Do not pin `framework-arduinopico` to 5.5.1: PlatformIO 6.2 /
platform-raspberrypi ships GCC 16 and that framework fails in
`malloc-lock.cpp`. `platformio.ini` lets the platform choose a matching one.

With two Pico boards attached, `picotool` cannot guess a target. Use
`--upload-port`, or reset the board into BOOTSEL (open the port at 1200 baud)
and copy `firmware.uf2` to the mounted `RP2350` volume.

## Serial console

115200 baud on the USB CDC port. By default the board prints log lines and raw
packets. `B` switches to packets only, `T` restores verbose output.

| Command | Effect |
|---|---|
| `B` | quiet mode: only raw packets |
| `T` | verbose mode: logs + packets |
| `I` | I2C scan of both buses |
| `E` | External board test: scan, raw ID registers, one reading per driver |
| `W` | External board watch: 20 samples every 3 s (~1 min) |
| `C` | RTC state (`rtc=`, `lost_power=`, `valid=`) |
| `S<unix-seconds>\n` | set the RTC |
| `X` | dump raw PCF8523 registers `0x00..0x12` |
| `D` | re-read `RAWDATA NNNN.BIN` and report packet/checksum counts |
| `K` | MCP2515 loopback self-test |
| `Y` | CAN `TIME_SYNC` loopback self-test |
| `L` | fragment/loopback/reassemble a packet self-test |
| `N` | CAN frame/packet counters and last received ID |
| `R` | reboot |

Set the RTC from the host:

```sh
printf 'S%s\n' "$(date +%s)" > /dev/cu.usbmodem1101
```

The PCF8523 coin cell keeps the time across power cycles. `lost_power=0` after
a power cycle means the cell is working. The firmware also accepts CAN time
from the PnC if its own RTC is invalid.

## Packet format

Packets are written to `RAWDATA NNNN.BIN` and streamed on serial in quiet mode.

```
char[4]  "ASU!"
uint32   sensor mask   bit (N-1-i) set when sensors[i] is in this packet
uint16   total length  includes the trailing checksum
uint32   millis()
...      sensor payloads in sensors[] order, only for set mask bits
int8     checksum      two's complement, whole frame sums to 0 mod 256
```

Current sensor order (`PAYLOAD_ENABLE_EXTERNAL 1`,
`PAYLOAD_ENABLE_STRATOSENSE 0`, `PAYLOAD_ENABLE_GEIGER 0`,
`PAYLOAD_ENABLE_DUMMY_EXTERNAL 0`):

| Index | Sensor | Payload bytes |
|---|---|---|
| 0 | PicoTemp | float °C (4) |
| 1 | ICM20948 | 10 × float: ax, ay, az, gx, gy, gz, mx, my, mz, temp (40) |
| 2 | PCF8523 | uint16 year + month, day, hour, minute, second (7) |
| 3 | TMP117 | float °C (4) |
| 4 | BME688 | float temp, uint32 pressure, float RH, uint32 gas resistance (16) |
| 5 | ENS160 | uint8 AQI, uint16 TVOC, uint16 eCO2 (5) |
| 6 | SHTC3 | float temp, float RH (8) |

Indexes 3–6 are the External board on `J200`/I2C1. `Geiger` (8 bytes) sits
right after the RTC when `PAYLOAD_ENABLE_GEIGER 1`; it is compiled out. The
AS7331, BMP390 and ozone sensor (`PAYLOAD_ENABLE_STRATOSENSE 1`) append after
the four working ones once the R16/address-rework items in `HARDWARE_DEBUG.md`
are done, and the 16-float dummy block goes last if enabled.

Each sensor has its own minimum period, so a packet only carries a sensor when
that sensor produced new data; the mask says which. A full packet is 99 bytes,
and an ENS160 or BME688 that has no fresh reading drops 5 or 16 bytes from that.
The decoder picks the layout from the mask sentinel and the byte count.

The **full packet** is fragmented and sent over CAN to the PnC (ID `0x201`) as
described in `CAN_PROTOCOL.md`. The PnC writes it to `PAYLOAD.BIN` on its SD.

Decode with `packet-decoder/parse_payload.py`.

## CAN

The payload speaks the shared ASCEND protocol documented in
`CAN_PROTOCOL.md`. It runs on core 0 (same core as I2C) on SPI1, independent of
the SD on SPI0. It sends:

- heartbeat and node status once a second,
- the **complete `ASU!` packet** as `PACKET` fragments (ID `0x201`) every loop,
- `TIME_REQ` until it has PnC time.

The PnC reassembles the packets and logs them to `PAYLOAD.BIN`.

`K`, `Y` and `L` prove the controller, the time protocol and the
fragmentation/reassembly path without a second node.

For bench bring-up with the sensors unavailable, `PAYLOAD_DUMMY_IMU` makes the
IMU driver emit synthetic values and `PAYLOAD_ENABLE_DUMMY_EXTERNAL` adds a
16-float Science placeholder. The dummy block is off by default now that the
four working External-board sensors are in the packet.

## Bench status (Fall 2026 bring-up)

Verified on the connected board:

- Pico temperature: works, ~22 °C.
- PCF8523 RTC: works after `S<unix>`; `lost_power=0` after a reboot.
- Geiger input: compiled out (`PAYLOAD_ENABLE_GEIGER 0`, MightyOhm library
  excluded), so it contributes no bytes to the packet. Re-enable only if a tube
  is fitted.
- MCP2515: `K`, `Y` and `L` all PASS.
- I2C0 scan: `0x68` responds (RTC).
- I2C1 scan with the external board attached: `0x48 0x53 0x70 0x77`.
- External board data is in the packet: TMP117, BME688, ENS160 and SHTC3. The
  AS7331, BMP390 and ozone sensor stay behind `PAYLOAD_ENABLE_STRATOSENSE 0`
  until the rework in `HARDWARE_DEBUG.md` is done.
- Local SD is enabled (`PAYLOAD_ENABLE_SD 1`) after the 2026-11-13 `3V3` →
  `+3V3` rework. A fresh FAT32 card reaches ACMD41 and mounts; logs go to
  `RAWDATA NNNN.BIN` using the lazy-open storage path.

### External (StratoSense) board

Netlist devices, expected addresses and measured state (`E`, 2026-11-09):

| Address | Part | ID read | Driver | Result |
|---|---|---|---|---|
| 0x48 | TMP117 | `0x0117` | OK | ~20.7 °C, stable |
| 0x53 | ENS160 | `0x0160` | OK | AQI 1, TVOC 24-26 ppb, ECO2 402-404 ppm |
| 0x70 | SHTC3 | `0x0807` | OK | ~21.1 °C, ~50 %RH |
| 0x76 | BMP390 | no ACK | FAIL | not responding at all |
| 0x77 | BME688 | `0x61` | OK | temperature ~21 °C, RH ~42 % good; **pressure ~50 Pa (broken)** |
| 0x74 | AS7331 | no ACK | FAIL | not responding at all |

The SHTC3 only answers after a wake command, so a raw probe must send `0x3517`
first (the Adafruit driver does this in `begin()`).

Two failures are schematic defects, not firmware:

1. **AS7331 analog rail is starved.** In `AS7331.SchDoc`, VDDA (U5 pin 3) is
   fed from `+3V3` through **R16 = 10 kΩ** with C9 (0.1 µF) as its only
   decoupling. The AS7331 draws 0.97 mA standby / 1.42 mA active, so a 10 kΩ
   series resistor drops 9.7-14 V and VDDA sits near 0 V. The datasheet also
   limits VDDA - VDDD to ±0.3 V, and here VDDD is 3.3 V. R16 should be 0 Ω (or
   a ferrite bead).
2. **AS7331 address pins float at mid-rail.** A0 (pin 14) has R11 10 kΩ to
   VDDA and R13 10 kΩ to AGND; A1 (pin 7) has the same with R10/R12. That puts
   both pins at ~1.65 V, inside the undefined band (VIL ≤ 0.99 V, VIH ≥ 2.31 V).
   R10/R11 or R12/R13 must be removed so each pin is tied hard to one rail
   (both low = 0x74).
3. **BMP390 does not answer.** Chip select (pin 6) is tied to +3V3 and SDO
   (pin 5) to GND, which is the correct I2C/0x76 configuration, so check that
   U4 is populated and soldered, and measure CSB = 3.3 V and SDO = 0 V.
4. **BME688 pressure channel returns no data.** The chip ID (`0x61`) and
   calibration block read cleanly and are stable at 100 kHz and 50 kHz;
   temperature and humidity track the SHTC3/TMP117. A forced conversion
   (`ctrl_meas = 0x8D`, 500 ms) leaves the pressure ADC at `0x000005` over six
   consecutive reads while the temperature ADC changes normally. That points at
   the part (`U2`), not the bus.

Only four of the six external devices are usable today, so
`PAYLOAD_ENABLE_STRATOSENSE` stays off and the dummy block remains in the
packet.

The USB device path can swap between the two Pico boards. The payload is the
one whose `I2C0` sees only `0x68` and which prints `ASU!` packets; the PnC sees
`0x40/0x42/0x68/0x76` and prints `PKT,` lines.

Open issues, with evidence from the firmware diagnostics:

1. **IMU does not respond.** `WHO_AM_I@0x68=0x00`, `@0x69=0xFF`. The netlist
   ties AD0/SDO to GND, so the IMU address (0x68) collides with the PCF8523.
   The firmware refuses to initialise `0x68` because the writes would also hit
   the RTC. Two things to check:
   - `+1V8` at U600 VDDIO (pin 8) and the level-shifter gate. The Internal
     board has no 1.8 V regulator; the PnC's LM3671 supplies it over the stack.
   - AD0/SDO strap. To use the IMU at all, AD0 must be tied high (0x69) or the
     IMU moved to another bus, and then `ICM20948_ADDR_0X68_PRESENT` set to 0
     (it already is) with a 0x69 path.

2. **Local microSD now works after the 3V3 rework.** The 2026-11-13 wire tied
   `3V3` to `+3V3`; the slot reaches ACMD41 and mounts with a known-good FAT32
   card. The PnC still logs the CAN copy, so this is a backup path.

3. **Geiger is compiled out.** `PAYLOAD_ENABLE_GEIGER 0` keeps `GeigerSensor`
   and the MightyOhm library out of the build. When it is re-enabled,
   `MightyOhmGeigerCounter::begin()` only attaches an interrupt, so
   `verify()` returns true even with no tube fitted — 0 cps proves nothing.

4. Error LEDs are not routed on this board (D300/D301/D400/D500/D600/D601 are
   power indicators), so `PAYLOAD_ERROR_LEDS_WIRED` is 0 and the error code is
   only visible over serial/CAN.
