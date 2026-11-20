#!/usr/bin/env python3
"""Decode the PnC's ``PAYLOAD.BIN`` wrapper produced from CAN packets.

The PnC writes one record per reassembled packet::

    uint32 magic       bytes "PLOG"
    uint32 unix_s      PnC UTC at reception (0 if unknown)
    uint32 uptime_ms   PnC millis at reception
    uint8  node        CAN node id (1 = payload)
    uint8  length      embedded packet length
    uint8  data[length] the board's own packet ("ASU!" for the payload)

The wrapper timestamp is when the PnC received the packet. Newer embedded
payload packets also contain their own sample-time UTC fields.

Usage::

    uv run python parse_payload_log.py PAYLOAD.BIN
    uv run python parse_payload_log.py PAYLOAD.BIN --summary
    uv run python parse_payload_log.py PAYLOAD.BIN --raw payload_raw.bin
"""

from __future__ import annotations

import argparse
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path

from parse_payload import decode_packet, fmt_packet

MAGIC = 0x474F4C50  # bytes "PLOG" little endian
HEADER = struct.Struct("<IIIBB")
HEADER_LEN = HEADER.size  # 14


def records(raw: bytes):
    offset = 0
    while offset + HEADER_LEN <= len(raw):
        magic, unix_s, uptime_ms, node, length = HEADER.unpack_from(raw, offset)
        if magic != MAGIC:
            offset += 1
            continue
        if offset + HEADER_LEN + length > len(raw):
            break
        data = raw[offset + HEADER_LEN : offset + HEADER_LEN + length]
        yield unix_s, uptime_ms, node, data
        offset += HEADER_LEN + length


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", help="PAYLOAD.BIN")
    parser.add_argument("--summary", action="store_true", help="counts only")
    parser.add_argument("--raw", default=None, help="write raw packets here")
    args = parser.parse_args()

    raw = Path(args.file).read_bytes()
    count = 0
    decoded = 0
    bad = 0
    nodes: dict[int, int] = {}
    raw_out = bytearray()

    for unix_s, uptime_ms, node, data in records(raw):
        count += 1
        nodes[node] = nodes.get(node, 0) + 1
        raw_out.extend(data)
        try:
            packet = decode_packet(data)
            decoded += 1
        except (ValueError, struct.error):
            bad += 1
            if not args.summary:
                print(f"bad packet node={node} len={len(data)}", file=sys.stderr)
            continue

        if args.summary:
            continue

        stamp = (
            datetime.fromtimestamp(unix_s, timezone.utc).isoformat()
            if unix_s
            else "no-time"
        )
        print(f"rx={stamp} node={node} pnc_up={uptime_ms}ms {fmt_packet(packet)}")

    if args.raw:
        Path(args.raw).write_bytes(raw_out)

    if args.summary:
        print(
            f"{args.file}: {count} records, {decoded} decoded, {bad} bad, "
            f"nodes={nodes}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
