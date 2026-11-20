#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5"]
# ///
"""Passively record both boards' status lines for a supervised bench soak.

First run scripts/identify_boards.py; then pass the identified ports explicitly:
    uv run scripts/monitor_stack.py --pnc /dev/cu.usbmodem101 \\
        --payload /dev/cu.usbmodem1101 --output soak.jsonl

The output is created exclusively, never appended or overwritten. Serial output
is evidence only: this program cannot prove SD persistence or flight readiness.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

import serial

STATUS_RE = re.compile(rb"STATUS (?:sd=[01] file=|sensors=\d+/\d+ sd=[01])[^\r\n]*")
FIELD_RE = re.compile(r"([A-Za-z_]+)=([^ ]+)")


def parse_status(line: bytes) -> tuple[str, dict[str, str]]:
    text = line.decode("ascii", "replace")
    board = "pnc" if text.startswith("STATUS sd=") else "payload"
    return board, dict(FIELD_RE.findall(text))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pnc", required=True, help="identified PnC serial port")
    parser.add_argument("--payload", required=True, help="identified payload serial port")
    parser.add_argument("--output", required=True, type=Path, help="new JSONL output path")
    parser.add_argument("--minutes", type=float, default=60, help="duration (default 60 minutes)")
    args = parser.parse_args()
    if args.pnc == args.payload:
        parser.error("PnC and payload ports must differ")
    if not 0 < args.minutes <= 1440:
        parser.error("--minutes must be greater than 0 and at most 1440")

    try:
        output = args.output.open("x", encoding="utf-8")
    except OSError as exc:
        print(f"Cannot create output: {exc}", file=sys.stderr)
        return 1

    lock = threading.Lock()
    stop = threading.Event()
    counts = {"pnc": 0, "payload": 0}
    failures = []

    def record(event: dict) -> None:
        event["host_utc"] = datetime.now(timezone.utc).isoformat()
        with lock:
            output.write(json.dumps(event) + "\n")
            output.flush()

    def monitor(expected: str, port: str) -> None:
        try:
            with serial.Serial(port, 115200, timeout=0.2) as connection:
                buffer = b""
                while not stop.is_set():
                    buffer += connection.read(4096)
                    # Binary payload packets may contain newlines, so search for
                    # status markers in bytes instead of decoding line by line.
                    for match in STATUS_RE.finditer(buffer):
                        end = match.end()
                        if end == len(buffer):
                            break  # status may still be arriving
                        board, fields = parse_status(match.group())
                        record({"type": "status", "port": port, "board": board, "fields": fields})
                        if board != expected:
                            failures.append(f"{port}: expected {expected}, observed {board}")
                            stop.set()
                            return
                        counts[board] += 1
                        buffer = buffer[end:]
                        break
                    if len(buffer) > 65536:
                        buffer = buffer[-4096:]
        except (serial.SerialException, OSError) as exc:
            failures.append(f"{port}: {exc}")
            record({"type": "serial_error", "port": port, "error": str(exc)})
            stop.set()

    threads = [
        threading.Thread(target=monitor, args=("pnc", args.pnc), daemon=True),
        threading.Thread(target=monitor, args=("payload", args.payload), daemon=True),
    ]
    started = time.monotonic()
    try:
        record({"type": "start", "pnc": args.pnc, "payload": args.payload,
                "requested_minutes": args.minutes})
        for thread in threads:
            thread.start()
        stop.wait(args.minutes * 60)
    except KeyboardInterrupt:
        failures.append("interrupted")
    finally:
        stop.set()
        for thread in threads:
            if thread.ident is not None:
                thread.join(timeout=2)
        elapsed = time.monotonic() - started
        record({"type": "end", "elapsed_seconds": round(elapsed, 3),
                "status_counts": counts, "errors": failures})
        output.close()

    print(f"Recorded {counts['pnc']} PnC and {counts['payload']} payload statuses to {args.output}")
    print("Review resets, SD state, drops and decoded card contents; this is not a pass/fail verdict.")
    for failure in failures:
        print(f"ERROR: {failure}", file=sys.stderr)
    if not all(counts.values()):
        print("ERROR: at least one board produced no status lines", file=sys.stderr)
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
