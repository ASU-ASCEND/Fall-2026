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

The mask, not the packet length, says which sensors are present: each sensor
has its own read period, so packets alternate between 15, 19, 30 and 34 bytes
on the internal-board build.

Live payload reader (sends `B` for quiet mode and `T` on exit):

```sh
uv run --with pyserial python payload_live.py --list
uv run --with pyserial python payload_live.py --port /dev/cu.usbmodem1101
```

The decoder picks the sensor layout from the mask sentinel: 4 sensors
(temp, IMU, RTC, Geiger) or 5 with the dummy external block used for bench
bring-up.

## Decode the PnC's payload log

The PnC reassembles CAN packets and writes `PAYLOAD.BIN` records (magic `PLOG`,
UTC, node, then the embedded packet). Decode them directly:

```sh
uv run python parse_payload_log.py PAYLOAD.BIN --summary
uv run python parse_payload_log.py PAYLOAD.BIN | head
uv run python parse_payload_log.py PAYLOAD.BIN --raw payload_raw.bin
```

`--raw` extracts the embedded `ASU!` packets so `parse_payload.py` can read
them as a normal capture.

## Decode an SD card capture

```sh
cd packet-decoder
uv run python parse_pnc.py /Volumes/SDCARD/data0.bin
uv run python parse_pnc.py data*.bin --summary
```

The decoder re-syncs on the next `0xDEADCAFE` if a packet is truncated or the
checksum fails, so a partially written tail does not hide earlier packets.

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
