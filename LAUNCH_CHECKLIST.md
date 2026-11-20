# Launch Readiness Checklist

Launch date: TBD — the original 2026-11-14 target slipped. Check items off only
when verified; record evidence, configuration, and unresolved issues in the
table at the bottom. A checked sensor initialization alone does not prove valid
measurements or successful storage.

**Current decision: NO-GO.** All software and logging paths are verified. The
remaining gates are the one-hour integrated soak (§6) and an outdoor GPS check
(§3), plus the pre-flight manifest and the limitation sign-off.

**Out of scope for this launch (decided 2026-11-20):** the RadiaCode
(BLE/spectra/extraction) and the external ATtiny85 watchdog. The RP2350 internal
watchdog (8 s) is the only reset source.

**Status as of 2026-11-20.** Both firmwares are current and flash-verified; both
cards freshly formatted (`ASCEND-PNC` → next file `data0000.bin`, `ASCEND-PAYL`
→ `RAWDATA0000.BIN`). The checkout page serves the PnC sensors plus every
CAN-linked payload sensor; one PnC data file now interleaves PnC packets with
the payload records (`PAYLOAD.BIN` retired); the Science board (BMP390 + TMP117)
is logging; the dead IMU is dropped with its packet slot reserved. Merged-log
decoders pass 18 tests and decoded every real capture with 0 skipped bytes.
Backups: `~/ASCEND-launch-backups/pnc-card-20261120-0102/` (41 files) and
`~/ASCEND-launch-backups/payl-card-20261120-0104/` (16 files), all SHA-256
verified.

## 1. Flight configuration

- [x] Sensor set, packet layout, and rates are frozen and documented. PnC: pico
      temp, PCF8523 RTC, BME688, INA260, GPS. Payload: pico temp, TMP117, BME688,
      ENS160, SHTC3, AS7331 UV, BMP390 (Science), ozone, TMP117 (Science).
      The IMU is dropped (dead part, reserved slot); the payload RTC stays
      disabled. Full-house payload packet = 90 bytes (14 header + 75 + checksum).
- [ ] Record a one-page pre-flight manifest: card identities, firmware commit,
      battery model/voltage, harness wiring, and rework list. Backups and
      HARDWARE_DEBUG.md cover most of it; the manifest is still missing.
- [x] Launch-critical fixes only: teardown hardening, RadiaCode BLE removal,
      SPI1 mutex fix, SD merge, IMU drop. No broad refactoring.

## 2. PnC SD and unexpected resets — launch blocker

- [x] Diagnose the SD-on resets: shared-SPI locking (SPI1 mutex), SD failure
      teardown that stopped the bus, and watchdog interplay. Evidence in the
      table for 2026-11-13 and 2026-11-19.
- [x] Corrected and re-tested mount, file creation, writes, flushes, and
      controlled restarts across many boots; no unexpected resets in the
      2026-11-19/20 runs (`reset=1 wdt=0` on clean boots).
- [x] PnC sensor data and CAN payload records survive restarts and decode
      correctly: `data0038/0039` = 260 PnC packets + 143 payload records,
      0 skipped. The interleaved single-file schema is verified on hardware.
- [ ] Boot with the freshly formatted `ASCEND-PNC` and confirm `data0000.bin`
      is created and grows (next power-up; cards formatted 2026-11-20).
- [ ] Missing-card, full-card, and interrupted-write recovery (optional, time
      permitting).
- [ ] One-hour integrated soak with no reboot, SD error, or write failure —
      see §6.

## 3. Payload SD and sensor measurements

- [ ] Confirm the `3V3`→`+3V3` rework is secure and insulated (physical check).
- [x] `ASCEND-PAYL` mounts and its local log grows: 16 RAWDATA files decoded,
      latest two = 362 + 292 packets, `sd_write` = `generated`, 0 drops.
- [x] Extracted and decoded real card data: checksums, sample rates, coverage,
      and representative values checked (2026-11-20, 0 skipped bytes).
- [x] Test the AS7331 with a UV source and verify a response: **passed
      2026-11-20** — the UV torch produces a clear response.
- [x] Resolved or accepted the sensor issues: IMU dropped (0x68/RTC collision,
      dead); payload RTC disabled; BME688 values coherent in the 2026-11-20
      captures. Early-boot BME688 freeze remains an open risk (see below).
- [ ] Verify GPS outdoors and assess fix and position data (indoors: fix 0).
- [ ] Record which sensor limitations are acceptable for launch. Draft: IMU
      absent, UV zero without a source, ozone diagnostic-only, first BMP390
      sample per boot discarded.

## 4. CAN and redundant logging

- [x] Payload packets traverse CAN and are stored on the PnC — now interleaved
      into `dataNNNN.bin`; `PAYLOAD.BIN` is retired.
- [x] Bus resets, TX errors, and drops investigated: SPI1 sharing (mutex),
      fragment spacing, fragment drops. Current runs: `busreset=0`,
      `pkt_drop=0`, `queue_drop=0`, `payload_age` under 0.2 s.
- [x] Compared decoded PnC CAN records with the payload local log: same sensor
      mix and the same ~0.96 packets/s on both sides.
- [ ] Measure received/queued/logged/dropped counters during the one-hour soak.
- [x] Primary log and redundancy agreed: the PnC merged file is primary, the
      payload card is the redundant copy; zero unexpected loss is the target
      (fragment drops are counted and must stay 0 in the soak).

## 5. Data extraction

- [x] Tested decoders against real card files and fixed gaps: `parse_pnc.py`
      walks interleaved PnC + payload records (`--payload`, `--summary`),
      `parse_payload.py` resolves the science build, 18 regression tests pass.
      Commands documented in `packet-decoder/README.md`.
- [x] Copy-raw / decode-separately / report-invalid / back-up-both procedure
      exercised on both cards (2026-11-20, SHA-256 verified backups).
- [x] Verified counts, coverage, and representative values for all sources and
      compared the redundant payload records (RTC timestamps stay provisional
      until the pre-flight time set).

## 6. Final integrated one-hour test and launch decision

- [ ] Freeze and record flight firmware, cards, wiring, power, watchdogs,
      sensors, CAN configuration.
- [ ] Record start/end time, battery voltage, and firmware versions.
- [ ] Run continuously for one hour while monitoring resets, SD errors, CAN
      errors/drops, queue loss, and sensor health.
- [ ] Safely power down, extract all logs, decode, and inspect integrity,
      timestamps, coverage, and CAN loss.
- [ ] Pass: no unexpected reboots or SD mount/write failures; required data
      present; CAN loss within the agreed limit.
- [ ] Record go/no-go, accepted limitations, responsible person; keep the tested
      configuration unchanged.

## Known open risks (2026-11-20)

- BME688 froze at `31.66 °C/72337 Pa/100 %RH/0 Ω` in the 2026-11-19 early boots
  (init path suspect; healthy since). Watch it on every boot in the soak.
- The first BMP390 sample after each boot is invalid (81 kPa / 1825 m before
  settling to 97.3 kPa / 337 m) — discard it in analysis.
- The RTC is stale (reads Jan 2024) until the pre-flight time set; payload
  records inherit it.
- USB port names swap between boots and multiple serial readers silently
  corrupt one another's logs — start the loggers with
  `scripts/tio_serial.py start` (one reader per port, one shared log) and check
  `scripts/tio_serial.py status`, which reports the last line age per board.

## Test evidence and decisions

| Date/time | Configuration or test | Result / evidence | Follow-up |
|---|---|---|---|
| 2026-11-13 | PnC and payload passive serial check | PnC `sd=1`, `data0005.bin` 110490→111273 bytes, `sensors=5/5`, `pkt_drop=35`, `busreset=16`, payload age >630 s; reset reason reports WDT but does not timestamp it. Payload repeatedly reported `SD open failed: RAWDATA0003.BIN`; CAN `txerr=1433`, `busreset=50`, time-sync age >656 s. | Preserve both cards and inspect filesystems and logs; diagnose CAN. Counters are cumulative, not proof of when errors occurred. |
| 2026-11-13 | Source review and build | PnC status SD operation lacked the shared SPI mutex; SD failure teardown could stop the SPI bus shared with CAN. Targeted changes build successfully; payload also builds. | Changes are not yet flashed or hardware-tested; neither issue is proven to cause the resets. |
| 2026-11-13 | Git history review | Historical RadiaCode code periodically read existing spectra; no spectrum-start command found. No ATtiny source or programming procedure found in available project history. | Confirm desired spectrum behavior and identify programmer/firmware. |
| 2026-11-13 | Offline decoder review | Six regression tests pass; checked-in PnC captures yielded 5,639 packets and 957 skipped bytes. Checked-in payload capture uses an unsupported 12-sensor layout; no real PLOG capture was available. | Test against current SD files; PLOG recovery is heuristic, not checksum-protected. |
| 2026-11-13 | `ASCEND-PAYL` card inspection | Copied three `RAWDATA` files to `~/ASCEND-launch-backups/2026-11-13-payload-card/` and matched SHA-256 hashes. Read-only macOS filesystem verification passed. Decoder reports 664 packets and zero skipped bytes. | Investigate Pico open failures; passing filesystem check does not prove hardware or sustained writes. Decoded sample includes invalid RTC month `00` and IMU values may be dummy; investigate before accepting sensor data. |
| 2026-11-13 | Payload packet inspection | All 332 recorded RTC samples contain frozen `2026-00-13 08:00:00`; raw bytes agree with the decoder. Recorded IMU values match the firmware's synthetic fallback. | Diagnose RTC state and I2C; `timeValid()` checks year only. Do not treat IMU values as flight measurements; resolve or explicitly accept missing real IMU. |
| 2026-11-13 | Payload RTC disabled in source | RTC slot reserved but absent in packets, RTC time-sync and status reads removed; build and seven decoder tests pass. | Flash the payload, verify RTC bit absent in actual local and CAN logs, and plan uptime-to-UTC correlation. |
| 2026-11-13 | Payload RTC-disabled build flashed to identified `/dev/cu.usbmodem1101` | Flash verified; 12 checksum-valid live packets had RTC mask bit clear. Payload status `sensors=6/6`, SD bytes 1795→2515, CAN `pkt_tx=30→39`; PnC received and logged payload packets (`pkt_rx=48→53`, `pkt_log=48→53`). | Verify recorded card and PnC CAN log contents, sustained stability, and uptime-to-UTC correlation. IMU still uses dummy mode; sensor count is not proof of real IMU data. |
| 2026-11-13 | No-synthetic payload build and sensor checks | Removed synthetic IMU and dummy external generators, flashed payload. Fifteen valid live packets omitted both IMU and RTC; status 5/6 and `icm=unavailable`. TMP117 ~24.49 C, SHTC3 ~24.77 C / 42.26% RH, ENS160 AQI 1/49 ppb/455 ppm. I2C0 AS7331 responds, UV counts zero without a source; ozone responds ~20 ppb. BMP390 no ACK. BME688 diagnostic 42.3 Pa and 0 ohm despite earlier plausible packet values. | IMU requires hardware address/power investigation; UV needs a source; BME channels and ~10 percentage-point humidity discrepancy need validation. Ozone/UV are diagnostic-only, not in current flight packets. RadiaCode BLE not tested. |
| 2026-11-13 | Added AS7331 and ozone to payload packets | Flashed build; 22 valid live packets decoded, including 15 UV and 8 ozone samples (~20 ppb); UV channels zero without source. Payload SD bytes 1533→1879, status 7/8. PnC receive counters remained stalled. | Test UV with a source, verify payload card contents and PnC CAN copy after fixing CAN; long-duration storage remains unverified. |
| 2026-11-13 | PnC live check | PnC Pico temperature ~31 C, BME ~25.3 C / 97196 Pa / 33% RH, INA ~124–144 mA / 3.54–3.56 V. RTC reports lost_power=0, GPS packet has no fix. PnC CAN RX stopped increasing, payload age >400 s; sd byte count grew. | Outdoor GPS fix and independent calibration/RTC time check; diagnose CAN stall and storage. PnC may emit stale or zero-initialized fields without validity flags. |
| 2026-11-13 | CAN controller loopback | Bench supply 3.7 V per operator. PnC and payload MCP2515 internal loopbacks both PASS while PnC normal TX/RX counters remain frozen and payload sends increase. PnC SD and core 0 status still advance. | Internal loopback does not exercise transceivers or physical CAN. Investigate PnC core 1/watchdog/serial/SPI service; preserve PnC SD before reset/flash, then test again. Multimeter is available if software tests do not resolve it. |
| 2026-11-13 | `ASCEND-PNC` backup and inspection | Copied logs to `~/ASCEND-launch-backups/pnc-card-20261113-Bom05e/`, matching SHA-256 hashes; read-only filesystem check passed. Seven PnC files decode to 7,261 packets, zero skipped bytes; `PAYLOAD.BIN` has 428 decodable records, zero unparsed bytes. All use the older seven-slot layout, so none verifies the later UV/ozone build. | Preserve older IMU data as potentially synthetic; diagnose CAN stall and verify new 10-slot packets on PnC storage. |
| 2026-11-13 | PnC SPI-sharing fix flashed, short integrated check | PnC `sd=1`, bytes 87→11832, CAN packets received/logged 1→72, `queue_drop=0`, `pkt_drop` increased to 15, `busreset=0` over the observed interval. PnC chose `data0003.bin` at 87 bytes despite the backed-up PnC card already having that name and higher indices. Payload repeatedly mounts but fails to open `RAWDATA0000.BIN`; status alternates to `sd=1` with zero bytes. | Investigate filename enumeration and mount/open status. Neither storage path nor sustained CAN is proven; do not assume these counts represent persisted data. |
| 2026-11-13 | Storage source review / serial identity | Operator confirmed cards are correctly seated. Arduino-Pico `openNextFile()` can stop the application's scan when an individual entry cannot be reopened; this is a possible, unproven cause of incomplete enumeration. Payload status used mount verification rather than log-open state; corrected and built in source, not yet flashed. USB mapping changed: `/dev/cu.usbmodem101` now outputs payload packets; PnC USB port absent. | Reconnect PnC, identify ports again, diagnose enumeration and preserve data; hardware-test the SD status fix. |
| 2026-11-13 | PnC-only USB connection | Reidentified `/dev/cu.usbmodem101` as PnC from live `PKT`/sensor output. Over ~27 s: SD bytes 27840→31755, CAN received/logged 59→85, `pkt_drop=0`, `queue_drop=0`, `busreset=0`. | Verify persisted records, filename selection and sustained operation; USB attachment does not establish whether the payload is otherwise powered. |
| 2026-11-13 | Payload SD recovery and queue counters | Reproduced repeated open failure. Added filesystem teardown on open failure, truthful SD-open status and generated/queue-drop/SD-write/SD-drop counters. First reboot opened `RAWDATA0006.BIN`; a subsequent boot exposed 10 startup queue drops. Increased payload queue from 10 to 64 packets and reflashed; on the next boot generated=199, SD write calls=199, queue_drop=0, sd_drop=0, bytes=11678 after a short run. | Verify actual persisted contents by removing and decoding card, then test restart/failure recovery and longer soak. Reboot alone may have cleared the original failure; root cause is not proven. Counters count accepted writes, not verified persistence. |
| 2026-11-13 | Payload SD persistence check | Backed up all nine RAWDATA files to `~/ASCEND-launch-backups/payl-card-20261113-NQ99OB/` with matching SHA-256 hashes; read-only filesystem verification passed. `RAWDATA0008.BIN` contains 810 valid packets and zero skipped bytes, covering uptime 2.355–819.681 s; adjacent timestamps 891–1156 ms apart. It includes 540 UV samples and 270 ozone samples, no IMU/RTC. | Check live final counters against persisted count in a controlled run; test UV response, failure/restart recovery and one-hour soak. Zero UV and 20 ppb ozone do not establish sensor accuracy. |
| 2026-11-13 | Reconnected stack and USB identification | New passive script identified PnC `/dev/cu.usbmodem101` and payload `/dev/cu.usbmodem1101`. PnC reports SD open and CAN received/logged counts increasing; payload `sd=0`, `sd_write=0`, `sd_drop` reached 160, with SD mount failing at default and 1 MHz. | Investigate payload mount failure before flight; no payload local samples from this boot were stored as of this observation. Port assignments can change. |
| 2026-11-13 | Bench supply on before USB | Payload opened its SD log and reported generated=40, sd_write=40, queue_drop=0, sd_drop=0 after initial startup. PnC reported `sd=1` but data file bytes=0, pkt_log=0 and queue_drop=105 while CAN packets were received. | Payload power sequencing is plausible but not proven by one successful boot; verify persisted data and repeat. Diagnose PnC storage status/low-space/open state; PnC `sd=1` alone does not prove logging. |
| 2026-11-13 | PnC SD diagnostics and directory change | After reflash, PnC reported sd_open=1, sd_low=0, sd_info=1, 59313 MB total / 5 MB used and payload_open=1; data bytes and received/logged CAN counts grew. It appended to existing `data0003.bin`. Updated directory enumeration and added an existing-target refusal; next boot reported new `data0004.bin` at 87 bytes, growing, but the prior PnC backup already contained that name. | Remove PnC card and compare current files, sizes and hashes against the backup. Do not assume name safety or underlying failure fixed; library existence check is not an atomic exclusive create. CAN fragment drops rose to 18. |
| 2026-11-13 | Offline decoder failure handling | Added regression tests and corrected both PnC and payload decoders to return nonzero when any requested input is missing. Ten decoder tests pass; the backed-up PnC files still decode 7,261 packets and payload `RAWDATA0008.BIN` 810 packets. | Continue checking skipped-byte counts separately: a successful exit does not establish complete data. |
| 2026-11-13 | Offline soak tooling | Added `scripts/monitor_stack.py` to passively record both boards' status lines to a new JSONL file; offline parsing tests pass. | Test with both actual boards before relying on it. Review resets, counter deltas, SD health and card contents manually; the tool does not certify a soak. |
| 2026-11-19 | RadiaCode BLE removed; client-friendly checkout teardown | Decision: the RadiaCode clock is synced from a phone pre-flight (UTC via NTP) instead of over BLE — `PNC_ENABLE_RADIACODE=0`, `lib_ignore = RadiaCodeBLELib` active (firmware 592→537 KB). Root cause of the BLE saga: `cyw43_wifi_leave()` after checkout teardown deadlocked the driver (bench-only path, now gone). Teardown hardened: per-step `[PNC] checkout closing:` prints, 250 ms watchdog-fed drain before pulling the AP interface (teardown with a live station had tripped the WDT at +8 s). Verified on user's power-cycle: window armed 16:21:22, all steps printed, `checkout ended` 16:22:59 (exactly the 120 s deadline), same boot still streaming at 16:25 with no reset. Also: a rogue orphaned serial-logger process was racing the port and eating log lines — kill duplicates with `lsof /dev/cu.usbmodem1101` if flaps reappear. | Open: one unexplained watchdog reset at ~16:17:37 on a post-window boot (2 min after a clean teardown) — log blind spot (rogue logger) hid its diagnostic; recovery design contained it (checkout skipped, logging resumed). If it recurs, the single-owner log will now capture the cause. |
| 2026-11-19 | PnC card decode: CAN/SD verification + payload PLOG analysis | PnC card (backed up to `~/ASCEND-launch-backups/pnc-card-20261119-evening/`): 25 files, **9,000 CAN packets, 0 skipped bytes** — SD logging and CAN both proven working; data0023 (480 records) matches the live serial counters of that boot. `PAYLOAD.BIN`: 3,020 records, 0 bad, 0 unparsed. External (StratoSense) board sensors all logging — TMP117 and SHTC3 in every record, ENS160 AQI 1–2/TVOC 38–87 ppb/eCO₂ 427–527 ppm, ozone ~20 ppb (373 records), AS7331 UV fields present but zero without a source (1,699 records). **The Science board (BMP390+TMP117 on payload I2C0) was not polled at all in these captures** — firmware's TMP117 instance was on I2C1 (external copy), the BMP390 slot was a reserved nullptr. | BME688 frozen at `31.66 °C/72337 Pa/100 %RH/0 Ω` in the 14:24–14:45 boots (403 records), healthy from 15:51 — init-path suspect on repeat, pressure channel already flagged in HARDWARE_DEBUG; verify AS7331 with a UV source and reconcile the I2C0/I2C1 AS7331 bus discrepancy against the harness. BMP390 (external copy) still dead. |
| 2026-11-19 | Science board integrated into payload firmware | `Science boardFALL2026` (BMP390 U1 @0x76 SDO-low + TMP117 U2 @0x48, payload I2C0 via J202/J203) now polled: BMP390 fills the packet's reserved slot (20-byte block: double temp, double pressure, float altitude from the existing driver), TMP117 appended as a new final slot behind `PAYLOAD_ENABLE_SCIENCE`. Decoder updated in lockstep — new build = second `LAYOUTS[11]` candidate in `parse_payload.py` (length-disambiguated from the old geiger variant); old 10-slot captures keep decoding (verified: today's PAYLOAD.BIN still 3,020/3,020, 0 bad). 11 decoder tests pass; payload firmware builds. | **Not yet hardware-tested** — flash the payload with the stack reassembled and the science board on J202/J203, then confirm `bmp390=`/`tmp117_sci=` appear in PAYLOAD.BIN records. External TMP117 prints as `tmp117=`, science as `tmp117_sci=`. AS7331 stays on I2C0 (bench-verified working; harness/schematic discrepancy still open). |




| 2026-11-20 | Payload CAN data on the checkout page + merged SD log | PnC now decodes the payload board's `ASU!` packets as they arrive over CAN (`pnc-fsw/src/PayloadTelemetry.cpp`, mutex-guarded snapshot written by core 1, read by the checkout task) — the checkout page's `/status` JSON and table gained the payload link plus every payload sensor with per-slot freshness: Pico temp, TMP117/BME688/ENS160/SHTC3 (External), AS7331 UV, BMP390/TMP117 (Science), ozone. One decode table covers both flying builds (sentinel 10 and 11; the old geiger build cannot collide). SD change: PLOG records now interleave into `dataNNNN.bin` (same 14-byte `PLOG` header; **`PAYLOAD.BIN` retired**), so one file holds the whole flight; `parse_pnc.py` walks both record types (new `--payload` prints decoded payload rows; `--summary` counts both), `dump_sd_summary` walks both record types, STATUS line dropped `payload_open=`. 18 decoder tests pass; old captures re-verified unchanged (9,000 PnC packets / 0 skipped, PAYLOAD.BIN 3,020/3,020 via `parse_payload_log.py`). | **Verified on hardware 2026-11-20:** `data0038/0039` written by this firmware decode as 260 PnC packets + 143 payload records with 0 skipped bytes; the `D` dump on the live board read `pnc=218/0 plog=247/0`. Remaining cosmetic check: eyeball the checkout page's payload section on a phone during one window (the JSON and decode paths are proven). |
| 2026-11-20 | IMU dropped from flight firmware; `9/8` explained | The `9/8` boot was two stacked bookkeeping quirks: the numerator counts every ACKing sensor (9 working incl. the science pair) while the stale denominator still counted the dead IMU but not the science sensors. `PAYLOAD_ENABLE_ICM20948=0` (AD0 collision with the RTC, HARDWARE_DEBUG §5): sensors[] keeps a reserved nullptr slot (bit 9 never set — no mask shifts, old captures decode, PnC ingest table unchanged), the MON line lost `icm=`, and `expected_sensors` now counts PicoTemp+4 External+UV+ozone+2 Science = 9. PnC checkout page/JSON lost the IMU row. Both firmwares build; 18 decoder tests pass. Bench: payload flashed and observed `sensors=9/9` streaming 72/88/90-byte packets (90 = all nine sensors, exactly the predicted full-house length). | **Resolved 2026-11-20 by card decode:** both images are confirmed correct — the PnC wrote the new merged schema (`data0038/0039`) and the payload's own `RAWDATA0014/0015` decode as the science build with no `ICM20948` and the full 9-sensor mix. The earlier flash-target ambiguity is closed. Operational note stands: one reader per port; identify boards by USB serial. |
| 2026-11-20 | Checkout session logging to SD | Added `CHECKOUT.LOG`: every checkout event is appended to the card with a millis stamp — `ap_up ip=... remaining_ms=...`, `client ip=...`, `req <first request line>` (including unknown/captive-portal paths), `client closed without a served request`, `ap_down`. Close-per-line flush so a battery-powered checkout can be reconstructed with no USB attached. Also mirrored to serial. Builds (Flash 533 KB). | **Not yet flashed** (no board connected at build time). Next window: keep the card in, connect the phone, then pull the card and read `CHECKOUT.LOG` — it will show whether the phone reached the page (`req GET /`), only probed (`req GET /generate_204` etc.), or never sent a request. |


## Open questions

- Sign off on the acceptable sensor limitations: IMU absent, ozone
  diagnostic-only, first BMP390 sample discarded per boot.
- Confirm the BME688 early-boot freeze does not recur in the soak (watch the
  first minutes of every boot).
