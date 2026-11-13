# Launch Readiness Checklist

Launch: tomorrow, 2026-11-14. Check items off only when verified; record evidence,
configuration, and unresolved issues below. A checked sensor initialization alone
does not prove valid measurements or successful storage.

**Current decision: NO-GO pending integrated verification.**

## Known status and risks

- PnC SD-enabled operation has experienced watchdog resets and mount/write
  failures. The SD-off test implicates the storage path but does not establish
  whether firmware, shared SPI, power, socket, or card is responsible.
- Payload SD mounted and its byte count increased after the `3V3` to `+3V3`
  rework; long-duration integrity is not yet verified.
- CAN-to-PnC SD logging has worked briefly, but CAN errors and packet drops
  have occurred. Neither CAN nor PnC storage is proven reliable.
- RadiaCode BLE is currently disabled in PnC firmware. Its existing service
  needs to be checked against the required spectrometer trigger behavior.
- Payload BME688 pressure/gas readings and IMU/RTC address conflict remain
  known issues; GPS has not had an indoor fix and UV needs a source test.
- The user can connect the RadiaCode physically for extraction at the end;
  normal operation should use Bluetooth with the Pico.
- ATtiny programming pins are accessible using wires. Programmer, pinout,
  voltage, firmware, and fuse settings still need identification.

## 1. Flight configuration

- [ ] Agree on required sensors, spectrometers, measurement sequence, rates,
      and acceptable data loss.
- [ ] Record hardware, wiring/rework, power, card identities, and firmware
      versions; preserve a recoverable baseline before changes.
- [ ] Identify launch-critical fixes; avoid broad refactoring that does not
      improve reliability, verification, or extraction.

## 2. PnC SD and unexpected resets — launch blocker

- [ ] Seat freshly formatted `ASCEND-PNC` and power-cycle the PnC.
- [ ] Diagnose SD-on resets, including firmware, SPI sharing and locking,
      card/socket, and power; record evidence before assigning a cause.
- [ ] Correct the identified failure and test mount, file creation, writes,
      flushes, and a controlled restart.
- [ ] Verify both PnC sensor data and CAN payload records survive a restart
      and decode correctly from the card.
- [ ] Test missing card, full card, and interrupted-write recovery if time allows.
- [ ] Final integrated soak: no unexpected reboot or SD mount/write failure.

## 3. Payload SD and sensor measurements

- [ ] Check that the `3V3` to `+3V3` rework is secure and insulated.
- [ ] Verify `ASCEND-PAYL` mounts and the local log grows throughout the soak.
- [ ] Extract and decode actual card data; check timestamps, checksums,
      sample rates, sensor values, and coverage.
- [ ] Test the AS7331 with a UV source and verify a response.
- [ ] Resolve or explicitly accept the BME688 pressure/gas and IMU/RTC
      address issues.
- [ ] Verify GPS outdoors and assess fix and position data.
- [ ] Record which sensor limitations are acceptable for launch.

## 4. CAN and redundant logging

- [ ] Verify payload packets traverse CAN and are stored in PnC `PAYLOAD.BIN`.
- [ ] Investigate bus resets, TX errors, and drops, including wiring,
      termination, power, and software.
- [ ] Measure received, queued, logged, and dropped packets during the soak.
- [ ] Compare decoded PnC CAN records with the payload local log.
- [ ] Agree on primary log, redundancy expectations, and acceptable loss.

## 5. ATtiny85 programming and watchdog

- [ ] Identify firmware/source, programmer, pinout, supply voltage, and fuse
      settings; confirm safe wiring before connecting.
- [ ] Read or back up the existing configuration where possible.
- [ ] Build, flash, and verify the intended firmware.
- [ ] Test normal heartbeat, deliberate heartbeat loss, reset and recovery,
      and absence of unwanted resets during SD and BLE activity.

## 6. RadiaCode and spectrometer behavior

- [ ] Identify RadiaCode model and connection details, required spectrometers,
      and exact trigger/measurement sequence.
- [ ] Audit original business logic against the required behavior, including
      what starts and records each spectrum.
- [ ] Enable and verify Pico-to-RadiaCode BLE; verify reconnect and clock
      alignment. `PNC_ENABLE_RADIACODE` is currently `0`.
- [ ] Confirm the expected triggers and spectra actually occur using device
      records or other direct evidence.
- [ ] Test BLE together with SD, CAN, sensors, and both watchdogs.
- [ ] Physically connect the RadiaCode at the end and verify extraction.

## 7. Data extraction

- [ ] Test existing PnC, payload, and PnC CAN-log decoders against real card
      files; fix gaps and document exact commands.
- [ ] Copy raw files unchanged, decode to separate outputs, report invalid or
      truncated records, and back up raw and decoded data.
- [ ] Verify counts, timestamps, coverage, and representative values for all
      sources and compare redundant payload records.
- [ ] Establish and test RadiaCode extraction, including spectra and time.
      **Caution:** `scripts/dump_radiacode.py` states that reading the device
      observation log transfers and deletes records. Verify the behavior and
      agree on a safe backup procedure before running it.

## 8. Final integrated one-hour test and launch decision

- [ ] Freeze and record intended flight firmware, cards, wiring, power,
      watchdogs, sensors, CAN, and BLE configuration.
- [ ] Record start/end time, battery voltage, and firmware versions.
- [ ] Run continuously for one hour while monitoring resets, SD errors,
      CAN errors/drops, queue loss, and sensor health.
- [ ] Verify the expected spectrometer sequence during the test.
- [ ] Safely power down, extract all logs, decode and inspect integrity,
      timestamps, measurement coverage, and CAN loss.
- [ ] Pass: no unexpected reboots or SD mount/write failures; required data
      present; CAN loss within the agreed limit.
- [ ] Record go/no-go decision, accepted limitations, and responsible person;
      keep the tested configuration unchanged.

## Test evidence and decisions

| Date/time | Configuration or test | Result / evidence | Follow-up |
|---|---|---|---|
| | | | |

## Open questions

- Which spectrometers and trigger/spectrum sequence are mandatory?
- What ATtiny programmer is available, and where is its intended firmware?
- Which sensor failures or CAN losses, if any, are acceptable for launch?
