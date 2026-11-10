#!/usr/bin/env python3
"""Decode PnC ``dataN.bin`` packet files written by pnc-fsw.

Packet layout (little endian, packed)::

    uint32 sync      0xDEADCAFE
    uint32 uptime_ms
    uint8  id
    uint8  length    total packet length, 87
    float  pico_temp_c
    uint32 rtc_unix
    float  bme_temp_c
    uint32 bme_pressure_pa
    float  bme_humidity_pct
    uint32 bme_gas_ohm
    float  ina_current_ma
    float  ina_bus_mv
    float  ina_power_mw
    uint32 gps_unix
    uint8  fix_type
    bool   fix_ok
    uint8  siv
    uint8  pad
    int32  lat_e7
    int32  lon_e7
    int32  alt_msl_mm
    int32  vel_n_mmps
    int32  vel_e_mmps
    int32  vel_d_mmps
    int32  hacc_mm
    int32  vacc_mm
    int8   checksum    two's complement so the byte sum is 0 mod 256

Usage::

    uv run python parse_pnc.py data7.bin
    uv run python parse_pnc.py data*.bin --summary
"""

from __future__ import annotations

import argparse
import glob
import struct
import sys
from pathlib import Path

SYNC = 0xDEADCAFE
PACKET_FMT = "<IIBBfIfIfIfffIB?BBiiiiiiiib"
PACKET = struct.Struct(PACKET_FMT)
PACKET_LEN = PACKET.size  # 87

FIELDS = [
    "sync",
    "uptime_ms",
    "id",
    "length",
    "pico_temp_c",
    "rtc_unix",
    "bme_temp_c",
    "bme_pressure_pa",
    "bme_humidity_pct",
    "bme_gas_ohm",
    "ina_current_ma",
    "ina_bus_mv",
    "ina_power_mw",
    "gps_unix",
    "fix_type",
    "fix_ok",
    "siv",
    "pad",
    "lat_e7",
    "lon_e7",
    "alt_msl_mm",
    "vel_n_mmps",
    "vel_e_mmps",
    "vel_d_mmps",
    "hacc_mm",
    "vacc_mm",
    "checksum",
]

assert PACKET_LEN == 87, PACKET_LEN


def valid(packet: bytes) -> bool:
    if len(packet) != PACKET_LEN:
        return False
    values = PACKET.unpack(packet)
    return (
        values[0] == SYNC
        and values[3] == PACKET_LEN
        and sum(packet) & 0xFF == 0
    )


def decode_file(path: Path):
    raw = path.read_bytes()
    offset = 0
    packets = []
    skipped = 0
    while offset + PACKET_LEN <= len(raw):
        packet = raw[offset : offset + PACKET_LEN]
        if valid(packet):
            packets.append(PACKET.unpack(packet))
            offset += PACKET_LEN
        else:
            # Re-sync on the next 0xDEADCAFE instead of dropping the file.
            nxt = raw.find(struct.pack("<I", SYNC), offset + 1)
            skipped += 1
            if nxt < 0:
                break
            offset = nxt
    return packets, skipped


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", help="dataN.bin file(s) or glob(s)")
    parser.add_argument("--summary", action="store_true", help="counts only")
    parser.add_argument("--csv", action="store_true", help="write CSV (no header)")
    args = parser.parse_args()

    paths: list[Path] = []
    for pattern in args.files:
        matches = sorted(glob.glob(pattern))
        paths.extend(Path(p) for p in matches or [pattern])

    total = 0
    skipped = 0
    if not args.summary and not args.csv:
        print(",".join(FIELDS))

    for path in paths:
        try:
            packets, file_skipped = decode_file(path)
        except FileNotFoundError:
            print(f"missing: {path}", file=sys.stderr)
            continue
        total += len(packets)
        skipped += file_skipped
        if not args.summary:
            for values in packets:
                print(",".join(str(v) for v in values))
        else:
            print(f"{path}: {len(packets)} packets, {file_skipped} resyncs")

    if args.summary:
        print(f"total: {total} packets, {skipped} resyncs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
