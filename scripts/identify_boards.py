#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Identify connected ASCEND boards from their serial output without commands.

Run after each USB reconnection:
    uv run scripts/identify_boards.py
    uv run scripts/identify_boards.py --seconds 15 --json

An unknown or ambiguous port is never assigned to a board. Quiet boards,
busy ports and disconnected devices can require a retry.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import re
import sys
import time
from dataclasses import asdict, dataclass

import serial
from serial.tools import list_ports


@dataclass
class Result:
    port: str
    board: str
    evidence: list[str]
    error: str | None = None


def valid_payload_packet(data: bytes) -> bool:
    start = 0
    while (start := data.find(b"ASU!", start)) != -1:
        if start + 10 <= len(data):
            length = int.from_bytes(data[start + 8 : start + 10], "little")
            if 15 <= length <= 255 and start + length <= len(data):
                if sum(data[start : start + length]) & 0xFF == 0:
                    return True
        start += 1
    return False


def valid_pnc_packet(data: bytes) -> bool:
    start = 0
    while (start := data.find(b"\xfe\xca\xad\xde", start)) != -1:
        if start + 87 <= len(data) and data[start + 9] == 87:
            if sum(data[start : start + 87]) & 0xFF == 0:
                return True
        start += 1
    return False


def classify(data: bytes) -> tuple[str, list[str]]:
    evidence = []
    if re.search(rb"\bPKT,\d+,\d+,87,", data):
        evidence.append("PnC text packet")
    if re.search(rb"STATUS sd=[01] file=", data):
        evidence.append("PnC status")
    if valid_pnc_packet(data):
        evidence.append("PnC checksum-valid binary packet")
    if valid_payload_packet(data):
        evidence.append("payload checksum-valid ASU! packet")
    if re.search(rb"STATUS sensors=\d+/\d+ sd=[01]", data):
        evidence.append("payload status")
    if b"ASCEND payload (FA26 Internal Board)" in data:
        evidence.append("payload boot banner")

    pnc = any(item.startswith("PnC ") for item in evidence)
    payload = any(item.startswith("payload ") for item in evidence)
    if pnc and payload:
        return "ambiguous", evidence
    return ("pnc" if pnc else "payload" if payload else "unknown"), evidence


def inspect(port: str, seconds: float) -> Result:
    data = bytearray()
    try:
        # 115200 only; never issue serial commands or the 1200-baud reset.
        with serial.Serial(port, 115200, timeout=0.2) as connection:
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                data.extend(connection.read(4096))
                board, evidence = classify(data)
                if board in ("pnc", "payload", "ambiguous"):
                    return Result(port, board, evidence)
                if len(data) > 1_000_000:
                    del data[:-65536]
    except (serial.SerialException, OSError) as exc:
        return Result(port, "unknown", [], str(exc))
    board, evidence = classify(data)
    return Result(port, board, evidence)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=10,
                        help="maximum listening time per port (default: 10)")
    parser.add_argument("--json", action="store_true", help="output machine-readable results")
    parser.add_argument("ports", nargs="*", help="specific ports; defaults to macOS usbmodem ports")
    args = parser.parse_args()
    if not 0 < args.seconds <= 120:
        parser.error("--seconds must be greater than 0 and at most 120")

    ports = sorted(set(args.ports or [
        info.device for info in list_ports.comports()
        if re.fullmatch(r"/dev/cu\.usbmodem[^/]*", info.device)
    ]))
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, len(ports)))) as pool:
        results = list(pool.map(lambda port: inspect(port, args.seconds), ports))

    counts = {board: sum(r.board == board for r in results) for board in ("pnc", "payload")}
    complete = counts == {"pnc": 1, "payload": 1} and all(
        r.board in ("pnc", "payload") for r in results
    )
    if args.json:
        print(json.dumps({"complete": complete, "ports": [asdict(r) for r in results]}, indent=2))
    elif not results:
        print("No /dev/cu.usbmodem ports found.")
    else:
        for result in results:
            detail = ", ".join(result.evidence) or result.error or "no recognizable output"
            print(f"{result.port}: {result.board.upper()} ({detail})")
        if not complete:
            print("Incomplete or ambiguous mapping; do not flash until both boards are identified.")
    return 0 if complete else 1


if __name__ == "__main__":
    sys.exit(main())
