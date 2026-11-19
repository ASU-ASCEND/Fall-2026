# Power and Control (PnC) Board Software

Firmware for the Fall 2026 PnC board: Raspberry Pi Pico 2W (RP2350) reading the
onboard INA260, BME688, PCF8523 RTC and the external u-blox NEO-M9N GPS, writing
binary packets to the microSD card.

RadiaCode BLE clock synchronization is enabled by default and begins after the
checkout Wi-Fi window. The RadiaCode records independently; the PnC does not
download or delete its observations. The Geiger input is on the payload board.

## Wi-Fi checkout

For the first five minutes after boot, join the open
`STRATODEVILS-CHECKOUT` network and visit **http://pnc.local/**. If mDNS does
not resolve, use the access-point IP printed on serial at boot. The network has
no internet and no password; anyone nearby can see its read-only telemetry.

The page polls `/status` for sensor readings, sample age, GPS fix, SD and CAN
status. An initialized device with no successful sample in the past ten seconds
is shown as waiting or stale, not as a valid zero. SD status means mounted and
not low on space; it does not verify a completed write. CAN status means the
controller initialized, not that another node is online.

At five minutes of uptime the next checkout service pass stops HTTP, mDNS and
the access point. Normal sensor, SD and CAN work continues; a blocked main loop
can delay this shutdown. BLE initialization and the RadiaCode clock-sync
attempt are deferred until after Wi-Fi shutdown to avoid concurrent radio use.
The previous BLE/SD reset issue and this transition require a full hardware
bench test before flight. A reset starts a new five-minute window.

## Pin map

Taken from the Altium project netlist generated with `altium-monkey`
(`PCBs/tools/extract_netlists.py`, output in `PCBs/netlists/`).

| Signal | Pico pin | Notes |
|---|---|---|
| I2C0 SDA | GP4 | BME688, INA260, PCF8523, GPS JGPS1 pin 7, 10k pull-up |
| I2C0 SCL | GP5 | BME688, INA260, PCF8523, GPS JGPS1 pin 8, 10k pull-up |
| SPI1 MISO | GP8 | microSD DAT0 and MCP2515 SO |
| SPI1 SCK | GP10 | microSD CLK and MCP2515 SCK |
| SPI1 MOSI | GP11 | microSD CMD and MCP2515 SI |
| SD CS | GP17 | 4.7k pull-up |
| CAN CS | GP9 | MCP2515 |
| CAN INT | GP12 | MCP2515 |
| CAN RESET | GP7 | MCP2515 |
| Watchdog heartbeat | GP6 | to ATtiny85 PB1; ATtiny drives Pico RUN through Q3 |
| Error LEDs | GP20, GP21, GP22 | JERR1 pins 2, 3, 4; pin 1 is GND |

I2C addresses: INA260 `0x40`, GPS `0x42`, PCF8523 `0x68`, BME688 `0x76`.

The SD card is on **SPI1**, not the Pico's default SPI0 pins. The firmware calls
`SPI1.setRX/setTX/setSCK` and `SD.begin(SD_CS_PIN, SPI1)`.

## SD logging policy

- Packets go to `dataNNNN.bin` (zero-padded). The next index is found with a
  single `readdir` pass, not one `SD.exists()` call per index.
- The data file is opened on the first successful packet write, so a boot that
  never produces data does not leave a 0-byte file behind.
- At mount the firmware checks free space; below **8 MB** it stops data logging
  and keeps sensors, CAN and serial running.
- A mount or write failure is retried at most three times per power cycle, then
  logging stops. This prevents the old "write fails → remount → new empty file"
  loop.
- `PAYLOAD.BIN` appends across boots; its records are self-describing.

## Build and flash

```sh
pio run -d pnc-fsw
pio run -d pnc-fsw -t upload
```

Do not pin `framework-arduinopico` to 5.5.1 with the current PlatformIO
toolchain (GCC 16); that combination fails in `malloc-lock.cpp`.
`platformio.ini` intentionally lets the platform select a matching framework.

## Serial output

At 115200 baud on the USB CDC port the board prints one CSV line per sample:

```
PKT,<uptime_ms>,<id>,<len>,<pico_c>,<rtc_unix>,<bme_c>,<bme_pa>,<bme_rh>,<bme_ohm>,
<ina_ma>,<ina_mv>,<ina_mw>,<gps_unix>,<fix_type>,<fix_ok>,<siv>,<lat_e7>,<lon_e7>,
<alt_msl_mm>,<vel_n>,<vel_e>,<vel_d>,<hacc>,<vacc>,<checksum>
```

Send `B` to switch to raw 87-byte binary packets (same bytes written to the SD
card) and `T` to switch back. `D` re-reads the current `dataNNNN.bin` and reports
packet/checksum counts, `I` rescans I2C, `C` prints the RTC state, `S<unix>`
sets the RTC (newline terminated), and `R` reboots the board. `K` runs an
MCP2515 loopback test, `Y` loops a CAN `TIME_SYNC` frame, `L` fragments and
reassembles a packet through the controller, and `N` prints the frame/packet
counters. See `packet-decoder/` for the decoders and a live serial reader.

## CAN receive and payload log

The PnC services CAN from a **core 1 task every 5–10 ms**, not from the 0.6 s
sensor loop: the MCP2515 has only two RX buffers and the payload can send
~70 frames/s. SPI1 is shared with the SD, so both sides take `spi1_mutex`.

Incoming `PACKET` fragments (`0x200 + node`) are reassembled per node and
written to `PAYLOAD.BIN` on the SD with the PnC's UTC timestamp:

```
u32 magic "PLOG" | u32 unix_s | u32 pnc_uptime_ms | u8 node |
u8 length | u8 data[length]
```

Decode it with `packet-decoder/parse_payload_log.py`.

## CAN

The PnC is the CAN time authority. It broadcasts `TIME_SYNC` once a second from
the RTC (or a GPS fix when one is valid), answers `TIME_REQ`, and shares SPI1
with the SD card. The full frame layout, node IDs and timing are in
`CAN_PROTOCOL.md` at the repository root. The implementation is the shared
library in `common/lib/ascend-can/`.

## Bench status

- GPS works over I2C and reports fix type / satellite count even without a sky
  view; position fields are zero until a valid fix. The NEO-M9N boots slower
  than the Pico after a cold power cycle, so `verify()` retries for ~7.5 s and
  a missing GPS is retried once every 10 s at runtime.
- The RTC coin cell is fitted. Set the time once with `S<unix>`; `adjust()`
  clears the PCF8523 oscillator-stop flag and enables battery switchover, and
  the time then survives resets and a full power cycle. Check with `C`.
  ```sh
  printf 'S%s\n' "$(date +%s)" > /dev/cu.usbmodem101
  ```
- The SD mount is retried every 5 s; sensor output continues over serial if no
  card is present.
