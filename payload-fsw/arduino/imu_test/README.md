# ICM-20948 IMU bench test (Arduino IDE)

Standalone sketch for the FA26 Internal (StratoCore) payload board. It does one
thing: prove whether the ICM-20948 is powered, reachable on I2C0, and producing
plausible motion data. No CAN, SD, RTC driver, or flight software involved.

Folder: `payload-fsw/arduino/imu_test/` (folder name must match `imu_test.ino`).

## 1. Arduino IDE setup

1. Boards Manager URL (already configured):
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
2. Install **Raspberry Pi Pico/RP2040/RP2350** by Earle Philhower.
3. **Tools → Board → Raspberry Pi Pico/RP2040/RP2350 → Raspberry Pi Pico 2 W**
4. **Tools → USB Stack → Pico SDK** (default).
5. Library Manager → install **Adafruit ICM20X** (`Adafruit BusIO` and
   `Adafruit Unified Sensor` come with it).
6. Open `imu_test.ino`, upload, open Serial Monitor at **115200**.

A correct Pico 2 W build reports roughly:

```
Sketch uses 324200 bytes (7%) of program storage space.
Global variables use 74888 bytes (14%) of dynamic memory.
```

If you see ~66000 bytes / ~14000 bytes you have **Raspberry Pi Pico 2**
(non-W) selected. Both are RP2350, so it will still run, but the wireless chip
and board defines are missing.

## 1a. Uploading, and fixing "No drive to deploy"

The Pico bootloader only accepts **.uf2** files over a USB mass-storage volume.
That is why the IDE always says "Converting to uf2" — it is a normal step, not
an error. On upload the IDE:

1. opens the CDC port at 1200 baud to reboot the board into BOOTSEL,
2. waits up to 10 s for an `RP2350` volume to appear in `/Volumes`,
3. copies `NEW.UF2` onto it.

"Scanning for RP2040 devices" is hard-coded text in `uf2conv.py`; it does not
mean an RP2040 was detected. "No drive to deploy" means step 2 failed.

Fixes, in order:

1. Close the Serial Monitor.
2. Make sure **Tools → USB Stack → Pico SDK** is selected.
3. Manual BOOTSEL: unplug, hold **BOOTSEL**, plug in, release. `ls /Volumes`
   should show `RP2350`.
4. Or send **`b`** in the Serial Monitor; the sketch reboots itself into
   BOOTSEL (`rp2040.rebootToBootloader()`).
5. Fallback that always works: **Sketch → Export Compiled Binary**, then drag
   `imu_test.ino.uf2` onto the `RP2350` volume in Finder.

Two Picos attached (`usbmodem101` and `usbmodem1101`) can also send the reset to
the wrong board. Unplug the other one while uploading.

## 2. Power the board correctly (important)

The IMU's VDDIO (`U600` pin 8) is on **+1V8**, and the payload board has **no
1.8 V regulator**. +1V8 comes from the PnC over the stack connector `J100`
pin 3. With only USB plugged into the Pico:

- +3V3 is present (Pico regulator backfeeds the rail),
- **+1V8 is missing, so the IMU is completely dead and cannot ACK.**

Valid setups:

| Setup | How |
|---|---|
| Full stack (recommended) | PnC powered (battery or USB) and the `J4`→`J100` cable connected, then USB into the payload Pico for serial. |
| Payload board alone | Bench supply **1.8 V** on `J100` pin 3 (or the `+1V8` rail) and GND. Never inject 1.8 V while the PnC is also driving that rail. |

The sketch cannot measure the rails, so it prints the pins to check when the
IMU stays silent.

## 3. Address collision with the RTC (as-built boards)

The netlist ties **AD0/SDO (`U600` pin 9) to GND**, putting the IMU at **0x68**
— the same fixed address as the PCF8523 RTC. Two devices on one address cannot
be read reliably, so the sketch:

- probes 0x69 first (AD0 strapped high),
- then 0x68,
- then masks the RTC (`Control_1 = 0xFF`) for one read to prove the IMU is
  present even though it is hidden.

Outcomes:

| Probe result | Meaning | Action |
|---|---|---|
| `@0x69=0xEA` | IMU at 0x69 | Streams data. Done. |
| `@0x68=0xEA` (clean) | IMU at 0x68, RTC effectively not driving | Streams data (RTC probably unpopulated). |
| `@0x68` masked = `0xEA` | **IMU present and powered, hidden behind the RTC** | Rework: lift `U600` pin 9 off GND and strap it to **+1V8**. Then rerun. |
| masked = `0xFF` / `0x00`, no 0x69 | IMU absent or unpowered | Check +1V8, +3V3, level shifter (see below). |

The rework is the only correct fix; `ALLOW_IMU_AT_0X68` exists as an escape
hatch for a board with no RTC fitted.

## 4. Expected output (healthy board at 0x69)

```
# I2C0 scan (SDA=GP4, SCL=GP5):
#   ACK at 0x68  (PCF8523 RTC and/or ICM-20948)
#   ACK at 0x69
# probe: WHO_AM_I @0x69=0xEA @0x68=0x00
# Initialising Adafruit ICM20948 at 0x69 ... ok
# IMU streaming at 0x69
# millis,ax_ms2,ay_ms2,az_ms2,gx_rads,gy_rads,gz_rads,mx_uT,my_uT,mz_uT,temp_C
1234,0.0412,-0.0180,9.8032,0.0012,-0.0007,0.0003,21.4000,-4.1000,43.2000,27.3100
...
# summary: |a|=9.81 m/s2 [OK]  |g|=0.07 dps [OK]  |m|=48.3 uT [OK]  T=27.31 C [OK]
```

Sanity checks used by the summary:

- **|accel| ≈ 9.81 m/s²** at rest (8.5–11.0 accepted). Tilt the board and the
  individual axes should swing ±9.81.
- **|gyro| ≈ 0 °/s** at rest (< 5 accepted). Spin the board and it rises.
- **|mag| 20–80 µT** (Earth field is 25–65 µT). Move a magnet near it and it
  changes.
- **Temperature** plausible for the room.

The CSV columns are raw sensor units: m/s², rad/s, µT, °C (Adafruit Unified
Sensor convention). Throughout the flight software stack, gyro stays in rad/s.

## 5. If nothing answers

Measure, in this order:

1. `U600` pin 8 (**VDDIO**) = 1.8 V, pin 13 (**VDD**) = 3.3 V.
2. `U600` pin 22 (**nCS**) = 1.8 V (tied to +1V8), pin 11 (**FSYNC**) = GND.
3. Level shifter `Q600`/`Q601`: gate = 1.8 V, high-side pull-ups `R601`/`R603`
   to 3.3 V, low-side `R600`/`R602` to 1.8 V, series `R106`/`R107` to GP4/GP5.
4. AD0/SDO (`U600` pin 9): GND = 0x68, +1V8 = 0x69.
5. Scope SDA/SCL at the Pico side: they should idle high. If either is stuck
   low, the level shifter or a short is holding the bus.
6. `s` in the Serial Monitor rescans; the sketch also retries every 3 s, so a
   rail that comes up late is picked up automatically.

`WHO_AM_I` values reported by the sketch:

- `0xEA` – ICM-20948
- `0x00` – RTC `Control_1` masking the bus (IMU silent or absent)
- `0xFF` – nobody ACKed at that address

## 6. What this does not test

Mag calibration, INT1, and the real sample timing used by the flight software.
Those live in `payload-fsw/src/ICM20948Sensor.cpp`. This sketch uses the same
ranges as flight (`±16 g`, `±1000 dps`, 50 Hz mag) so the numbers are directly
comparable.
