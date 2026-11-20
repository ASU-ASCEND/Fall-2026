#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial"]
# ///
"""Timestamped serial logger with DTR/RTS asserted and auto-reconnect.

The ESP32-S3's hardware USB CDC only emits data while DTR is asserted, which
tio 3.9 does not do, so tio logs stay empty on this board. This logger keeps
DTR/RTS asserted, prefixes each line with an ISO 8601 timestamp, appends to
the log file, and reconnects when the USB device re-enumerates.

    uv run scripts/serial_logger.py /dev/cu.usbmodem2101 logs/esp-checkout.log
"""

import sys
import time
from datetime import datetime
from pathlib import Path

import serial


def main() -> int:
    port, log_path = sys.argv[1], sys.argv[2]
    Path(log_path).parent.mkdir(parents=True, exist_ok=True)
    while True:
        try:
            ser = serial.Serial(port, 115200, timeout=1)
            ser.dtr = True
            ser.rts = True
        except (OSError, serial.SerialException) as exc:
            print(f"open {port} failed: {exc}; retrying", flush=True)
            time.sleep(2)
            continue
        print(f"connected to {port}, logging to {log_path}", flush=True)
        buf = b""
        try:
            while True:
                data = ser.read(512)
                if not data:
                    continue
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    stamp = datetime.now().isoformat(timespec="milliseconds")
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    out = f"[{stamp}] {text}\n"
                    with open(log_path, "a") as handle:
                        handle.write(out)
                    print(out, end="", flush=True)
        except (OSError, serial.SerialException) as exc:
            print(f"lost {port}: {exc}; reconnecting", flush=True)
            time.sleep(2)


if __name__ == "__main__":
    sys.exit(main())
