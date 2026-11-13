# Rough power budget — Fall 2026 stack

Bench-scale estimate, not a measured figure. Every number is a datasheet
typical plus a stated duty cycle, so treat the total as **±25 %**. Validate with
the INA260 on the PnC (see the last section).

## What is included

| Board | Contents counted |
|---|---|
| PnC (`ASCEND_Fall26_Power&Control`) | Pico 2 W, NEO-M9N GPS, BME688, INA260, PCF8523, MCP2515 + TJA1051, microSD, ATtiny85, LEDs |
| Internal / payload (`ASCEND_FA26_Internal_Board`) | Pico 2 W, ICM-20948, PCF8523, MCP2515 + TJA1051, LEDs. Local SD disabled |
| External (`ASCEND_External_Fall2026`, on `J200`) | TMP117, BME688, ENS160, SHTC3 |

**Not counted:** the RadiaCode (own battery, link only modelled as a BLE duty
cycle), the Science board (not built), the MightyOhm Geiger (compiled out, no
tube), the AS7331 and BMP390 (dead/unfitted, so not in the packet), ozone sensor
(not fitted), and the two RTC coin cells.

## Assumptions

- Battery: **1S Li-ion, 3.7 V nominal**. The TPS63020 buck-boosts are rated
  5.5 V in, and D1 is a 6.2 V gate clamp, so this is a single-cell pack, not 2S.
- Converter efficiency: **88 %** for the +3V3 and +5V TPS63020s and the +1V8
  LM3671. The Pico 2 W sits directly on the battery through Q1, so its current
  is already battery current.
- BLE: Pico 2 W `CYW43439` adds **~15 mA average** while a BLE session is up
  (peaks are 35–40 mA; the average covers connection intervals and reads).
- PnC BME688 is read **every loop** (`sysvar_update()` → `performReading()`,
  no gating), which pins the loop at ~4–5 Hz and keeps the heater busy.
- Payload BME688 is gated to 500 ms by `Sensor::minimum_period`.
- 25 °C, all rails healthy, RTC coin cells excluded.

## Per-rail budget

### +3V3 (TPS63020, 88 %)

| Load | mA | Note |
|---|---:|---|
| GPS NEO-M9N | 30.0 | Continuous, datasheet 25–31 mA |
| BME688 (PnC) | 8.0 | Heater, ~60 % duty because the read is ungated |
| MCP2515 (PnC) | 5.0 | |
| MCP2515 (payload) | 5.0 | |
| BME688 (External) | 5.0 | 500 ms period |
| microSD (PnC) | 2.0 | 87 B every loop, flush every 5 |
| LEDs (payload D301/D500/D601) | 1.8 | 2.2 k, always on |
| INA260 | 1.0 | |
| LEDs (PnC D3–D7) | 0.75 | 10 k each, always on |
| SHTC3 | 0.6 | |
| ATtiny85 | 0.3 | |
| PCF8523 ×2 | 0.3 | |
| TMP117 | 0.15 | |
| **Total** | **59.9** | **197.7 mW** |

### +5V (TPS63020, 88 %)

| Load | mA | Note |
|---|---:|---|
| Pico 2 (payload) | 22.0 | Fed from +5V into VSYS via Q100 |
| TJA1051 (PnC) | 5.0 | VCC |
| TJA1051 (payload) | 5.0 | VCC |
| D300 LED | 1.4 | 2.2 k to +5V |
| **Total** | **33.4** | **167.0 mW** |

### +1V8 (LM3671, 88 %)

| Load | mA | Note |
|---|---:|---|
| ENS160 | 12.0 | Heater, biggest External-board load |
| ICM-20948 | 3.0 | 9-axis |
| **Total** | **15.0** | **27.0 mW** |

### Battery direct

| Load | mA | Note |
|---|---:|---|
| Pico 2 W baseline | 30.0 | RP2350 + CYW43439 idle, no radio |
| D2 battery LED | 0.2 | 10 k from `Batt` |
| **Total** | **30.2** | **111.7 mW** |

## Totals

| | |
|---|---:|
| Rail power sum | 503.4 mW |
| After converter losses (+3V3/+5V/+1V8) | **556.8 mW** |
| Battery current at 3.7 V | **150.5 mA** |
| Energy per day | **13.4 Wh** |
| Split: PnC (incl. its own Pico W) | ~86 mA |
| Split: payload + External | ~65 mA |

## Bluetooth duty cycle

The RadiaCode is only contacted when the BLE session is up. Each 10 % of duty
costs about 1.5 mA of battery current.

| BLE window | Duty | Average added current | Total @3.7 V |
|---|---:|---:|---:|
| Continuous | 100 % | +15.0 mA | 165.5 mA |
| 60 s every 5 min | 20 % | +3.0 mA | 153.5 mA |
| 30 s every 10 min | 5 % | +0.8 mA | 151.2 mA |
| 60 s every hour | 1.7 % | +0.3 mA | 150.7 mA |

BLE is **not** the load to worry about. A 1-minute sync every 10 minutes is
under 1 % of the budget.

## Battery life

| Pack (1S) | Energy | Full discharge | 80 % usable |
|---|---:|---:|---:|
| 1000 mAh | 3.7 Wh | 6.6 h | 5.3 h |
| 2000 mAh | 7.4 Wh | 13.3 h | 10.6 h |
| **2500 mAh (785060 spec sheet)** | 9.3 Wh | **16.6 h** | **13.3 h** |
| **3000 mAh (PKCELL LP785060 page)** | 11.1 Wh | **19.9 h** | **15.9 h** |
| 5000 mAh | 18.5 Wh | 33.2 h | 26.6 h |
| 10000 mAh | 37 Wh | 66.4 h | 53.2 h |

Add ~1 h if the BLE window is a 1-minute sync per 10 minutes.

### Selected pack: PKCELL LP785060

| | |
|---|---|
| Chemistry / format | 1S Li-polymer pouch, 7.8 × 50 × 60 mm, 46 g |
| Nominal voltage | 3.7 V |
| Capacity | 3000 mAh on PKCELL's page; the generic 785060 spec (Adafruit's copy) says 2500 mAh min 2375 |
| Max continuous charge | 1500 mA (785060 spec) |
| Max continuous discharge | **1500 mA** (785060 spec, 0.5–0.6 C) |
| Protection | built-in PCM (over-charge, over-discharge, short) |
| Cut-off | 3.0 V |

The stack sits far inside this cell's limits:

| | Stack | Cell rating | Utilisation |
|---|---:|---:|---:|
| Average current | 150 mA (0.05 C) | 1500 mA | 10 % |
| Design peak | ~500 mA (0.17 C) | 1500 mA | 33 % |
| Boot inrush | ~1 A, few ms | 1500 mA | brief |

At 0.05 C average there is effectively no voltage sag, so the delivered
capacity is close to nameplate. Expect **~13 h usable** on the 2500 mAh cell and
**~16 h** on the 3000 mAh one.

Ask PKCELL for the 3000 mAh variant's own datasheet before flight: some 785060
cells ship with a 1 C (3 A) PCM instead of the 0.6 C limit in the Adafruit-copied
spec, so the discharge ceiling may be higher than the 1500 mA above. Either way
neither the average nor the peak comes close.

## Where the power goes, and what to cut

| Rank | Load | Cost | Fix |
|---|---|---:|---|
| 1 | GPS NEO-M9N | ~30 mA battery | Duty-cycle it or use UBX power-save once the fix settles. Biggest single win, ~20 % of the total |
| 2 | Pico 2 W baseline | 30 mA battery | Run at 48–100 MHz, or WFE between loops; ~15 mA back |
| 3 | Pico 2 (payload) | 22 mA on +5V (~30 mA battery) | Same: lower clock; saves ~14 mA |
| 4 | ENS160 heater | 12 mA on +1V8 | Duty-cycle its standard mode; drop to idle between readings |
| 5 | Two BME688s | ~13 mA on +3V3 | Gate the PnC one to 1 Hz: 8 → 3.2 mA |
| 6 | Two TJA1051s + MCP2515s | ~20 mA on 5 V/3.3 V | Only if you can put the transceivers in standby between packets |
| 7 | microSD | ~2 mA | Write at 1 Hz instead of every loop |

Order of magnitude: with GPS duty-cycled to 10 % and both Picos at a lower
clock, the stack drops from ~150 mA to roughly **90–100 mA**, doubling runtime.

## How to measure it

The INA260 (`U2`) sits between `RAW` and `INA_OUT`, so it sees everything on the
stack including the PnC's own Pico W. It does not see D2 (battery LED) or the
coin cells.

The PnC packet and 5 s status line already carry `INACurrent`, `INABusVoltage`
and `INAPower`. Average readings over a few minutes, with the payload cable
connected, then repeat with the payload unplugged to split PnC vs payload.

```
battery mA ≈ INAPower_mW / 3.7
```

Check the sign convention in `parse_pnc.py` before trusting a negative value —
INA260 current is signed.
