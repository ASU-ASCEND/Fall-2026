# Hardware debug list — Internal (payload) + External sensor board

Tested 2026-11-09 on the bench stack:

- **Power & Control board** (P nC) — supplies `+5V` / `+3V3` / `+1V8` over the
  6-pin Micro-Fit stack cable (`J4` on the PnC → `J100` on the payload).
- **Internal board** (payload, "FA26 Internal") — Pico 2, PCF8523, ICM-20948,
  MCP2515 + TJA1051, microSD, Geiger input.
- **External sensor board** — plugged into **`J200`** on the Internal board
  (6-pin Micro-Fit, the only external connector that carries `+1V8`).

Source data: `PCBs/netlists/ASCEND_FA26_Internal_Board.json`,
`ASCEND_External_Fall2026.json`, `ASCEND_Fall26_Power&Control.json`
(extracted with altium-monkey). Schematics under
`PCBs/extracted/ASCEND_FA26_Internal_Board/` and
`PCBs/extracted/ASCEND_External_Fall2026/`.

Firmware test: payload serial `E` (one pass: I2C scan, raw ID registers, one
driver reading per part) and `W` (20 samples over 1 minute). Everything below is
reproducible without the PnC except where noted.

---

## Summary, in priority order

| # | Pri | Board · sheet | Refdes / pins / nets | Measured on the bench | Cause | Fix |
|---|---|---|---|---|---|---|
| 1 | P0 | Internal · `MicroSD_Slot.SchDoc` | **J400.4** (card VDD) on net **`3V3`**; pull-ups `R400`/`R401`/`R402` 4.7 kΩ; LED `D400` + `R403` 2.2 kΩ | Card answers CMD0 (`0x01`), CMD8 (`R7=000001AA`), CMD55; **never ACMD41**, so it stays in idle | Net `3V3` has **no source**. It holds only C400.1, C401.1, J400.4, R400–R403 and never ties to `+3V3` or `U100.3V3` (both separate nets) | Tie `3V3` → `+3V3` (0 Ω link or net merge). Probe `J400.4`, check `D400` with the card out |
| 2 | P0 | Internal · `CAN_Transceiver.SchDoc` | **U302.1** TXCAN → `NetU302_1`; **U302.2** RXCAN → `NetU302_2`; **U300.1** TXD → `NetU300_1`; **U300.4** RXD → `NetU300_4` | Both boards pass `K`/`Y`/`L` loopback, but payload `pkt_tx = 0`, PnC `pkt_rx = 0`, nothing on the wire | All four pins are single-pin nets — the controller never reaches the transceiver. PnC does it right (`U8.1 → CAN_TX → U9.1`) | Jumper **U302.1 ↔ U300.1** and **U302.2 ↔ U300.4** (continuity first, unpowered) |
| 3 | P0 | External · `AS7331.SchDoc` | **U5.3** VDDA on `VDDA+3V3`, fed from `+3V3` through **R16 = 10 kΩ**; `C9` 0.1 µF; VDDD `U5.10` = `+3V3` | No ACK at any address (0x74–0x77); every other part on `+3V3` works | 0.97 mA standby / 1.42 mA active through 10 kΩ → **VDDA ≈ 0 V**; also breaks the VDDA − VDDD ≤ ±0.3 V limit | **R16 → 0 Ω** (or ferrite), keep C9. Probe `U5.3` (expect 3.3 V) |
| 4 | P1 | External · `AS7331.SchDoc` | **U5.14** A0 = `NetR11_1`: `R11` 10 kΩ↑ to `VDDA+3V3`, `R13` 10 kΩ↓ to AGND. **U5.7** A1 = `NetR10_1`: `R10`↑, `R12`↓ | A0/A1 sit at ~1.65 V, inside the undefined band (V<sub>IL</sub> ≤ 0.99 V, V<sub>IH</sub> ≥ 2.31 V) | Both straps fitted on each pin = resistor divider | Remove **R10 and R11** → A0 = A1 = 0 → **0x74**. Must be done together with item 3 |
| 5 | P0 | Internal · ICM20948 + RTC sheets | **U600.9** AD0/SDO → GND; **U500** PCF8523 fixed at 0x68; both on `NetJ202_2`/`NetJ202_1` (I2C0, also on `J202`/`J203`) | PCF8523 regs `0x00..0x12` all read `0x00`, RTC time frozen; `WHO_AM_I` @0x68 = `0x00`, @0x69 = `0xFF` | IMU and RTC both ACK 0x68, data is the wired-AND of both parts | Lift **U600.9 to `+1V8`** (0x69); then `WHO_AM_I` = `0xEA` and RTC reads clean |
| 6 | P1 | External · `BMP390.SchDoc` | **U4.1/10** VDDIO/VDD → `+3V3`; **U4.6** CSB → `+3V3`; **U4.5** SDO → GND (0x76); **U4.7** INT floating | I2C scan shows `0x48 0x53 0x70 0x77` only; raw ID read at 0x76 gets no ACK | Straps are correct for I²C — part not fitted or not soldered | Inspect/reflow **U4**; probe CSB = 3.3 V, SDO = 0 V, VDD = 3.3 V |
| 7 | P1 | External · `BME688.SchDoc` | **U2.2** CSB → `+3V3`, **U2.5** SDO → `+3V3` (0x77), **U2.6** VDDIO, **U2.8** VDD → `+3V3`; `C6` 100 nF | ID `0x61` and calibration block read clean and stable at 100 kHz and 50 kHz; T ~21 °C, RH ~42 % good; **pressure ~50 Pa**, ADC stuck at `0x000005` | Pressure channel returns no valid conversion (temp ADC updates normally). Suspect the part | Check marking (BME680 shares ID `0x61`), swap **U2** |
| 8 | P2 | Termination (all boards) | PnC `R22` 120 Ω across `CAN_H`/`CAN_L`; Internal `R300`+`R301` 60 Ω/60 Ω with `C300` 0.1 µF to GND | 60 Ω differential with the cable connected — correct for 2 nodes | Every board is permanently terminated | Make `R22` and `R300`/`R301` cut/jumper-selectable; terminate only the two ends |

Priority: **P0** = blocks a subsystem that should work now (open net, missing
power, address collision). **P1** = part is dead or unusable, needs rework or
replacement. **P2** = design margin, decide before the next build.

Working on the External board: **U6** TMP117 (0x48), **U1** ENS160 (0x53),
**U3** SHTC3 (0x70), **U2** BME688 (0x77, temperature + humidity only).

---

## 1. Internal board: microSD socket is on an unpowered net

**Sheet:** `MicroSD_Slot.SchDoc`. **Net:** `3V3` (note: *not* `+3V3`).

The whole `3V3` net is:

```
3V3 ── C400.1, C401.1          decoupling
    ├─ J400.4                  microSD VDD
    ├─ R400.1 (4.7k)           CS / DAT3 pull-up
    ├─ R401.1 (4.7k)           DAT0 pull-up
    ├─ R402.1 (4.7k)           CMD pull-up
    └─ R403.2 (2.2k) → D400 → GND   power LED
```

**Nothing on the Internal board connects `3V3` to `+3V3`, or to the Pico's 3V3
output (`NetU100_3V3`, which is also single-pin), or to any other supply.** This
was checked by walking every component on the net: C400, C401, J400, R400, R401,
R402, R403 — none of their other pins touch `+3V3`.

The PnC board has only one 3.3 V net (`+3V3`) and its SD socket sits on it, so
this looks like a `3V3` vs `+3V3` power-port split on the Internal board.

**Symptom:** the card answers CMD0 (`0x01`), CMD8 (`R7 = 000001AA`) and CMD55,
but never ACMD41, and CMD0 still answers afterwards. That is exactly what a card
does when it is powered only parasitically through the 4.7 kΩ SPI pull-ups via
its input-protection diodes: enough to answer the first commands, not enough
current to finish initialization.

**Fix:** tie `3V3` to `+3V3` (0 Ω link, net-name merge, or rework).

**Measure to confirm:** `J400.4` to GND — expect 0 V (or ~2.6 V floating), not
3.3 V. With the card **removed**, the `3V3` rail LED `D400` (via `R403` 2.2k)
should be lit; if it is dark, the net has no source. Also check the PCB for a
net-tie or copper bridge between `3V3` and `+3V3` that the schematic lacks.

**Note:** the mission build logs the payload over CAN and does not use this
socket, so this does not currently block flight data.

---

## 2. Internal board: CAN controller ↔ transceiver is open

**Sheet:** `CAN_Transceiver.SchDoc`.

```
U302 (MCP2515T-E/SO)          U300 (TJA1051T/3)
  pin 1  TXCAN  → NetU302_1    pin 1  TXD  → NetU300_1     ← both single-pin nets
  pin 2  RXCAN  → NetU302_2    pin 4  RXD  → NetU300_4     ← both single-pin nets
```

Everything else on `U302` is routed as expected: `13/14/15` to SPI1
(`U100.GP14/GP15/GP12`), `16` CS via `R304` to `U100.GP13`, `12` INT via `R104`
to `U100.GP11`, `17` RESET via `R103` to `U100.GP10`, `18` VDD `+3V3`, `7/8` to
`Y300` with `C303`/`C304` 15 pF. Pins `3/4/5/6/10/11` (CLKOUT, RTS, RXnBF) are
unconnected, which is normal — **only pins 1 and 2 are the fault.**

`U300` is otherwise correct: `2` GND, `3` VCC `+5V`, `5` VIO `+3V3`,
`6` CANL → `J100.2`, `7` CANH → `J100.1`, `8` S via `R105` (0 Ω) to `U100.GP9`.

For comparison, the **Power & Control board is correct**:
`U8.1 TXCAN → CAN_TX → U9.1 TXD`, `U8.2 RXCAN → CAN_RX → U9.4 RXD`.

**Symptom:** both boards pass controller loopback (`K`, `Y`, `L`), but the
payload's `pkt_tx` stays 0 and the PnC receives nothing over the wire.

**Fix:** two jumper wires — **U302.1 ↔ U300.1** and **U302.2 ↔ U300.4** — or a
board revision. Continuity-test both with the board unpowered.

---

## 3. AS7331 analog supply — `U5` pin 3 (External board)

**Sheet:** `AS7331.SchDoc`. **Net:** `VDDA+3V3`.

```
+3V3 ──[ R16  10 kΩ ]── VDDA+3V3 ── U5.3 VDDA
                                   ├─ C9 0.1 µF ─ AGND
                                   ├─ R10.2 ─ NetR10_1 (A1 strap)
                                   └─ R11.2 ─ NetR11_1 (A0 strap)
```

- The AS7331 draws **0.97 mA standby / 1.42 mA typ active**, so a 10 kΩ series
  resistor drops 9.7–14 V. VDDA will sit at approximately **0 V**.
- The datasheet limits **VDDA − VDDD to ±0.3 V**; VDDD (`U5.10`) is hard-wired to
  `+3V3`, so this is outside the absolute maximum ratings.
- Matches the measurement: `U5` does not ACK at any address, while every part on
  the same `+3V3` rail (U1, U2, U3, U6) works.

**Fix:** change **R16 to 0 Ω** (or a ferrite bead), keep C9.

**Measure to confirm:** `U5.3` to AGND — expect ~3.3 V after the fix, ~0 V now.

---

## 4. AS7331 address straps — `U5` pins 14 (A0) and 7 (A1)

**Sheet:** `AS7331.SchDoc`.

```
A0 = U5.14 (NetR11_1):  R11 10 kΩ → VDDA+3V3
                        R13 10 kΩ → AGND
A1 = U5.7  (NetR10_1):  R10 10 kΩ → VDDA+3V3
                        R12 10 kΩ → AGND
```

All four are fitted, so each pin sits at **VDDA/2 ≈ 1.65 V**. The datasheet
bands are V<sub>IL</sub> ≤ 0.3·VDDD = **0.99 V** and V<sub>IH</sub> ≥ 0.7·VDDD =
**2.31 V**, so the level is undefined.

**Intended address is `0x74`** (A0 = A1 = 0 — matches the SparkFun driver's
`kDefaultAS7331Addr` and `UV_I2C_ADDR` in the payload firmware).

**Fix:** remove **R11 and R10** so A0/A1 are pulled to AGND by R12/R13 (0x74).
Do not use 0x77 — the BME688 is already there.

**Do this together with item 3.** Fixing R16 alone lifts the straps from ~0 V to
exactly 1.65 V, which is the undefined state.

**Measure to confirm:** `U5.14` and `U5.7` to AGND — expect 0 V, not 1.65 V.

---

## 5. Internal board: IMU address collides with the RTC

**Sheets:** `ICM20948_Motion_Tracker.SchDoc`, `RealTimeClock_UpdatedCrystal.SchDoc`.
```
U600 (ICM-20948)  pin 9 AD0/SDO → GND      → I2C address 0x68
U500 (PCF8523T)   fixed I2C address        → 0x68
```

Both sit on I2C0 (`U100.GP4` / `U100.GP5`, nets `NetJ202_2` / `NetJ202_1`, also
brought out on `J202` and `J203`). The IMU reaches that bus through the BSS138
level shifters `Q600`/`Q601` (`+1V8` side: `R600`/`R602` pull-ups and `U600`
pins 24/23; `+3V3` side: `R601`/`R603` plus `R106`/`R107`).

Because both devices ACK `0x68` and both drive SDA, every read is the wired-AND
of the two parts.

**Measured consequence:**

- PCF8523 registers `0x00..0x12` read
  `00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00`
- `WHO_AM_I` (`U600` reg `0x00`) also reads `0x00` instead of `0xEA`
- the RTC time never advances
- `WHO_AM_I` at `0x69` reads `0xFF` (nothing there)

So the RTC itself is almost certainly fine — it is being corrupted by the IMU on
every transaction. This is a single root cause for two symptoms.

**Fix:** strap **`U600.9` (AD0/SDO) to `+1V8`** → address `0x69` (a cut and wire
is enough on the current board). A firmware change to talk to the IMU at 0x69
goes with it.

**Measure to confirm:** `U600.9` to GND is 0 V today; after the lift, `WHO_AM_I`
at 0x69 should read `0xEA` and the RTC registers should become sane.

---

## 6. BMP390 does not answer — `U4` (External board)

**Sheet:** `BMP390.SchDoc`. The straps are **correct** for I2C at `0x76`:

| Pin | Signal | Net | Expected |
|---|---|---|---|
| 1 | VDDIO | `+3V3` | 3.3 V |
| 10 | VDD | `+3V3` | 3.3 V |
| 6 | CSB | `+3V3` | 3.3 V (I2C mode) |
| 5 | SDO/SA0 | `GND` | 0 V (address 0x76) |
| 4 | SDA | `SDA_1` | — |
| 2 | SCL | `SCL_1` | — |
| 7 | INT | `NetU4_7` | floating, fine |

The I2C1 scan sees `0x48 0x53 0x70 0x77` and no `0x76`; the raw chip-ID read
(`reg 0x00 → 0x60`) gets no ACK.

**Fix:** confirm **U4 is populated and reflowed**. Measure CSB = 3.3 V,
SDO = 0 V, VDD/VDDIO = 3.3 V, then re-check `0x76`.

---

## 7. BME688 pressure channel — `U2` (External board)

**Sheet:** `BME688.SchDoc`. Wiring is correct: `U2.2` CSB → `+3V3` (I2C mode),
`U2.5` SDO → `+3V3` (address `0x77`), `U2.6` VDDIO and `U2.8` VDD → `+3V3`,
`C6` 100 nF.

Responds correctly:

- chip ID `reg 0xD0 = 0x61`
- calibration block `reg 0x8A` is plausible and stable:
  `08 66 03 E0 DF 8C 4B D7 58 00 0A 1B C6 FF 23 1E 00 00 F5 F8 81 F4 1E BA`
  (identical on repeat reads and at 50 kHz)
- temperature ~21 °C and humidity ~42 %RH track the SHTC3/TMP117

Fails:

- pressure reads **50 Pa** instead of ~97,000 Pa
- a forced conversion (`ctrl_meas = 0x8D`, 500 ms wait) leaves the pressure ADC
  at **`0x000005`** on six consecutive reads, while the temperature ADC updates
  normally (`0x67E077` → `0x78EFB0`)
- the pressure `xlsb` byte is `0x5A`; its low nibble is reserved and should be 0,
  so the register is not holding a valid conversion

**Suspected part fault.** Check the marking (BME680 and BME688 share chip ID
`0x61`) and swap `U2` if a spare is available.

---

## 8. Termination when the External and Science boards are added

Correct today for **two** nodes:

| Board | Network | Refdes |
|---|---|---|
| PnC | 120 Ω across CAN_H / CAN_L | `R22` (`J4.1` / `J4.2`) |
| Internal | 60 Ω + 60 Ω split, midpoint to GND through 0.1 µF | `R300`, `R301`, `C300` (`J100.1` / `J100.2`) |

= **60 Ω differential**, which is what the bench measures with the cable
connected.

Both boards are permanently terminated. Adding the External and Science boards
with their own 120 Ω gives 3–4 parallel terminations (40 Ω or 30 Ω) and loads
the bus. **Make `R22` and `R300`/`R301` jumper- or cut-selectable, or agree that
only the two physical ends are terminated.**

---

## Checked and correct — don't spend time here

- **External I2C pull-ups:** six 10 kΩ per line (SCL: R2/R4/R6/R8/R14/R19,
  SDA: R3/R5/R7/R9/R15/R20) = 1.67 kΩ equivalent. Fine at 100/400 kHz.
- **External `J1` ↔ payload `J200`:** `1 SCL, 2 SDA, 3 +3V3, 4 +1V8, 5 NC, 6 GND`
  (7/8 are shell tabs).
- **AS7331 REXT:** `R17` = 3.3 MΩ — correct (datasheet 3.267–3.333 MΩ).
- **ENS160:** `U1.4` VDD = `+1V8`, `U1.5` VDDIO = `+3V3`, `U1.7` CS = `+3V3`,
  `U1.3` ADDR pulled to `+3V3` by `R1` → 0x53. Correct.
- **SHTC3:** `U3.1` VDD = `+3V3`, `U3.4/5` VSS/EP = GND. Correct.
- **TMP117:** `U6.4` ADD0 = GND → 0x48, `U6.3` ALERT pulled up by `R21` 10 kΩ.
- **PnC CAN:** `U8.1 → CAN_TX → U9.1`, `U8.2 → CAN_RX → U9.4`, `R22` 120 Ω,
  `C27`/`C28`, `U10` ESD. `U9.8` (S) held low by `R21` 10 kΩ, `JMP_SIL` to `GP3`
  not driven by firmware.
- **TJA1051 supplies:** VCC `+5V`, VIO `+3V3` on both boards.
- **6-pin stack cable:** `J4` / `J100` both `1 CAN_H, 2 CAN_L, 3 +1V8, 4 +3V3,
  5 +5V, 6 GND`.
- **Connector part numbers** (for rework/BOM questions): Internal `J100` and
  `J200` = 44914-0601 (Micro-Fit 3.0, 6 pos, press-fit); `J201`/`J202`/`J203` =
  43045-0400 (Micro-Fit 3.0, 4 pos, right-angle); `J204` = 43045-0429; External
  `J1` = 43045-0615 (Micro-Fit 3.0, 6 pos).
- **Remaining single-pin nets** (all benign): Pico `GP0/1/2/3/8/20/21/26/27/28`,
  `RUN`, `3V3`, `3V3_EN`, `ADC_VREF`, `AGND`; `U302.3/4/5/6/10/11`
  (CLKOUT/RTS/RXnBF); unused `U600` pins; `J400.1/8/CD` (DAT2/DAT1/CD, unused in
  SPI mode); `J200.5`; External `U1.6`, `U4.7`, `U5.8`, `U5.9`, `J1.5`.

---

## Re-test procedure after the rework

1. Power the stack from the PnC battery on `P1` (the PnC's USB alone does not
   power the board rails).
2. PnC serial: `D` (SD), `K` / `Y` / `L` (CAN), `N` (counters).
3. Payload serial: `E` for the one-pass board test. Expected with all fixes:
   `scan: 0x48 0x53 0x70 0x74 0x76 0x77`, six OK lines, `BMP390` pressure
   ~97,000 Pa and `AS7331` UV values.
4. `W` for a 1-minute stability check (the ENS160 needs ~60 s in standard mode
   before AQI/TVOC/ECO2 are valid).
5. CAN: after the jumper wires, the payload's `pkt_tx` should rise and the PnC's
   `pkt_rx` / `PAYLOAD.BIN` should grow (`N` and `D` on the PnC).

Device IDs the test expects:

| Address | Part | Register | Expected |
|---|---|---|---|
| 0x48 | TMP117 | 0x0F | `0x0117` |
| 0x53 | ENS160 | 0x00/0x01 | `0x60 0x01` (`0x0160`) |
| 0x70 | SHTC3 | cmd `0xEFC8` | `0x0807` (masked with `0x083F`) |
| 0x74 | AS7331 | — | ACK only (no ID register) |
| 0x76 | BMP390 | 0x00 | `0x60` |
| 0x77 | BME688 | 0xD0 | `0x61` |
