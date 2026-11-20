# ASCEND packet decoders

Tools for the binary packets written by `pnc-fsw` (`dataN.bin`) and
`payload-fsw` (`RAWDATAn.BIN`), plus a live reader for the PnC serial stream.

## PnC packet layout

87 bytes, little endian, packed. See the docstring in `parse_pnc.py` for the
full field list. Every packet starts with `0xDEADCAFE`, carries its own length
at offset 9, and ends with a checksum chosen so the byte sum is `0 mod 256`.

## Payload packet layout

Variable length, little endian, packed. Every packet starts with `ASU!`, then a
32-bit sensor mask, a 16-bit total length, `millis()`, the payloads for the
sensors whose mask bit is set, and a checksum that makes the byte sum
`0 mod 256`. See `parse_payload.py` for the field order and scaling.

Decode a payload capture:

```sh
uv run python parse_payload.py RAWDATA0.BIN --summary
uv run python parse_payload.py RAWDATA*.BIN | head
```

The PnC and payload decoders return a nonzero exit status if any requested
input is missing, even if other files were decoded. Check the reported skipped
bytes separately; a zero exit status alone does not certify data completeness.

The mask, not the packet length, says which sensors are present: each sensor
has its own read period, so packets alternate between 15, 19, 30 and 34 bytes
on the internal-board build.

The current 10-slot layout includes the I2C0 AS7331 (UVA/UVB/UVC in µW/cm²)
and I2C1 ozone (ppb). RTC and BMP390 slots are reserved and omitted; the IMU
is omitted if unavailable. Older layouts remain supported where unambiguous.

Live payload reader (sends `B` for quiet mode and `T` on exit):

```sh
uv run --with pyserial python payload_live.py --list
uv run --with pyserial python payload_live.py --port /dev/cu.usbmodem1101
```

The decoder matches known layouts from the mask sentinel and byte count
(partial packets are normal). When two builds share a sensor count and a
packet's length fits both, the packet is ambiguous; per file, packets that fit
only one build resolve the build and the ambiguous siblings decode under it
(`decode_file` announces the resolution on stderr). Files with missing or
split evidence keep rejecting ambiguous packets. Ambiguous matches are never
guessed in isolation. The current payload build sends seven sensors:
temp, IMU, RTC, TMP117, BME688, ENS160, SHTC3. The geiger and dummy-external
variants remain decodable for older logs.

The checked-in `raw_data/Payload` captures use an unsupported 12-sensor layout;
do not infer their field mapping from the current firmware. The summary's
skipped-byte count includes unsupported packets, corruption, and partial tails,
and does not distinguish them.

`raw_data/ASCEND-PAYL` holds the FA26 flight-card captures (volume
`ASCEND-PAYL`, November 2026 bench runs): files 0000–0006 from the 10-slot
build and 0007–0012 from the 11-slot science build, all decoding with zero
skipped bytes. The science-board BMP390 and TMP117Sci mask bits never appear
in the 11-slot captures. `parsed/RAWDATAnnnn.jsonl` beside them stores the
`--json` decode of each file, and `data-processing/data/ASCEND-PAYL.csv`
holds the same packets as one wide table (`File`, `Session`, `Millis`, one
column per sensor field) for the notebooks.

## Decode the PnC's payload log

The PnC reassembles CAN packets and writes `PAYLOAD.BIN` records (magic `PLOG`,
UTC, node, then the embedded packet). Decode them directly:

```sh
uv run python parse_payload_log.py PAYLOAD.BIN --summary
uv run python parse_payload_log.py PAYLOAD.BIN | head
uv run python parse_payload_log.py PAYLOAD.BIN --raw payload_raw.bin
```

`--raw` extracts the embedded `ASU!` packets so `parse_payload.py` can read
them as a normal capture. The summary also reports unparsed bytes and truncated
record candidates. PLOG has no wrapper checksum, so resynchronization after
corruption is heuristic and recovered records should be checked independently.

## Decode an SD card capture

```sh
cd packet-decoder
uv run python parse_pnc.py /Volumes/SDCARD/data0.bin
uv run python parse_pnc.py data*.bin --summary
```

The decoder re-syncs on the next `0xDEADCAFE` if a packet is truncated or the
checksum fails, so a partially written tail does not hide earlier packets.
Summaries report skipped bytes, including partial tails.

Run offline regression tests with `uv run python -m unittest -v test_decoders`
from `packet-decoder`.

## Live serial

The firmware prints one `PKT,...` CSV line per sample by default. Send `B` on
the serial port (or pass `--binary`) to receive raw packets instead; send `T`
to return to CSV.

```sh
uv run --with pyserial python pnc_live.py --list
uv run --with pyserial python pnc_live.py --port /dev/cu.usbmodem101
uv run --with pyserial python pnc_live.py --port /dev/cu.usbmodem101 --binary
```

`--binary` exercises the exact bytes that are written to the SD card, including
the sync word, length and checksum.
