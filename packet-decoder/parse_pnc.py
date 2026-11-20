#!/usr/bin/env python3
"""Decode PnC ``dataN.bin`` packet files written by pnc-fsw.

PnC records (little endian, packed)::

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

Since 2026-11-20 the same file interleaves payload records received over
CAN, in the exact PAYLOAD.BIN format (that file is retired)::

    uint32 magic       bytes "PLOG"
    uint32 unix_s      PnC UTC at reception (0 if unknown)
    uint32 uptime_ms   PnC millis at reception
    uint8  node        CAN node id (1 = payload)
    uint8  length      embedded packet length
    uint8  data[length] the board's own packet ("ASU!" for the payload)

The default output is the PnC CSV; interleaved payload records are
counted (and shown per file on stderr) but not printed. ``--payload``
prints the decoded payload rows instead, and ``--summary`` counts both
record types. ``parse_payload_log.py`` still decodes pre-2026-11-20
``PAYLOAD.BIN`` captures.

Usage::

    uv run python parse_pnc.py data7.bin
    uv run python parse_pnc.py data*.bin --summary
    uv run python parse_pnc.py data*.bin --payload
"""

from __future__ import annotations

import argparse
import glob
import re
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

from parse_payload import decode_packet, fmt_packet

SYNC = 0xDEADCAFE
SYNC_BYTES = struct.pack("<I", SYNC)
PACKET_FMT = "<IIBBfIfIfIfffIB?BBiiiiiiiib"
PACKET = struct.Struct(PACKET_FMT)
PACKET_LEN = PACKET.size  # 87

PLOG_MAGIC = 0x474F4C50  # bytes "PLOG" little endian
PLOG_MAGIC_BYTES = struct.pack("<I", PLOG_MAGIC)
PLOG_HEADER = struct.Struct("<IIIBB")
PLOG_HEADER_LEN = PLOG_HEADER.size  # 14

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
    "session",
]

assert PACKET_LEN == 87, PACKET_LEN


def natural_key(path: str):
    """Sort data2.bin before data10.bin instead of lexicographically."""
    return [int(part) if part.isdigit() else part.lower()
            for part in re.split(r"(\d+)", path)]


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
    """Walk one interleaved data file.

    Returns ``(pnc_packets, plog_records, skipped_bytes)`` where each
    plog record is ``(unix_s, pnc_uptime_ms, node, data)``.
    """
    raw = path.read_bytes()
    offset = 0
    packets = []
    plog_records = []
    skipped = 0

    def resync() -> int:
        """Offset of the next magic of either type, or -1."""
        nxt_pnc = raw.find(SYNC_BYTES, offset + 1)
        nxt_plog = raw.find(PLOG_MAGIC_BYTES, offset + 1)
        candidates = [n for n in (nxt_pnc, nxt_plog) if n >= 0]
        return min(candidates) if candidates else -1

    while offset < len(raw):
        chunk = raw[offset : offset + 4]
        if chunk == SYNC_BYTES and offset + PACKET_LEN <= len(raw):
            packet = raw[offset : offset + PACKET_LEN]
            if valid(packet):
                packets.append(PACKET.unpack(packet))
                offset += PACKET_LEN
                continue
        elif chunk == PLOG_MAGIC_BYTES and offset + PLOG_HEADER_LEN <= len(raw):
            _, unix_s, uptime_ms, node, length = PLOG_HEADER.unpack_from(raw, offset)
            end = offset + PLOG_HEADER_LEN + length
            if 0 < length <= 255 and end <= len(raw):
                plog_records.append(
                    (unix_s, uptime_ms, node, raw[offset + PLOG_HEADER_LEN : end])
                )
                offset = end
                continue
        # Not a valid record start: re-sync on the next magic of either
        # type instead of dropping the file.
        nxt = resync()
        if nxt < 0:
            skipped += len(raw) - offset
            break
        skipped += nxt - offset
        offset = nxt
    return packets, plog_records, skipped


def print_payload_rows(plog_records, file_name: str) -> int:
    """Print one line per interleaved payload record; returns bad count."""
    bad = 0
    for unix_s, uptime_ms, node, data in plog_records:
        stamp = (
            datetime.fromtimestamp(unix_s, timezone.utc).isoformat()
            if unix_s
            else "no-time"
        )
        try:
            packet = decode_packet(data)
        except (ValueError, struct.error):
            bad += 1
            print(
                f"{stamp} node={node} pnc_up={uptime_ms}ms "
                f"bad packet len={len(data)}",
                file=sys.stderr,
            )
            continue
        print(f"{stamp} node={node} pnc_up={uptime_ms}ms {fmt_packet(packet)}")
    return bad


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", help="dataN.bin file(s) or glob(s)")
    parser.add_argument("--summary", action="store_true", help="counts only")
    parser.add_argument("--csv", action="store_true", help="write CSV (no header)")
    parser.add_argument(
        "--payload",
        action="store_true",
        help="print decoded interleaved payload records instead of PnC CSV",
    )
    args = parser.parse_args()

    paths: list[Path] = []
    for pattern in args.files:
        matches = sorted(glob.glob(pattern), key=natural_key)
        paths.extend(Path(p) for p in matches or [pattern])

    total = 0
    payload_total = 0
    skipped = 0
    bad_payload = 0
    missing_input = False
    session = 0
    last_uptime = None
    if not args.summary and not args.csv and not args.payload:
        print(",".join(FIELDS))

    for path in paths:
        try:
            packets, plog_records, file_skipped = decode_file(path)
        except FileNotFoundError:
            print(f"missing: {path}", file=sys.stderr)
            missing_input = True
            continue
        total += len(packets)
        payload_total += len(plog_records)
        skipped += file_skipped

        if args.payload:
            bad_payload += print_payload_rows(plog_records, str(path))
        elif args.summary:
            print(
                f"{path}: {len(packets)} packets, "
                f"{len(plog_records)} payload records, {file_skipped} skipped bytes"
            )
        else:
            if plog_records and not args.summary:
                print(
                    f"# {path}: {len(plog_records)} interleaved payload "
                    f"records; decode them with --payload",
                    file=sys.stderr,
                )
            for values in packets:
                # uptime_ms restarts at every boot; a decrease is the reliable
                # session boundary now that a file may span boots.
                uptime = values[1]
                if last_uptime is not None and uptime < last_uptime:
                    session += 1
                last_uptime = uptime
                print(",".join(str(v) for v in (*values, session)))

    if args.summary:
        print(
            f"total: {total} packets, {payload_total} payload records, "
            f"{bad_payload} bad payload packets, {skipped} skipped bytes"
        )
    return 1 if missing_input else 0


if __name__ == "__main__":
    raise SystemExit(main())
