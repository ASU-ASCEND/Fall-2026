#!/usr/bin/env python3
"""Read a live payload board on USB serial and decode its packets.

The firmware sends raw ``ASU!`` packets. Send ``B`` first to silence the log
lines from both cores; the script restores verbose mode (``T``) on exit.

Examples::

    uv run --with pyserial python payload_live.py --list
    uv run --with pyserial python payload_live.py --port /dev/cu.usbmodemXXXX
    uv run --with pyserial python payload_live.py --port /dev/cu.usbmodemXXXX --duration 10

The USB path can swap between the two Pico boards. Confirm you have the payload
by sending ``I`` and checking that I2C0 sees only ``0x68``; the PnC sees
``0x40/0x42/0x68/0x76`` and prints ``PKT,`` lines instead of ``ASU!``.
"""

from __future__ import annotations

import argparse
import struct
import sys
import time

from parse_payload import HEADER_LEN, SYNC, decode_packet, fmt_packet


def frames(buffer: bytearray):
    while True:
        start = buffer.find(SYNC)
        if start < 0:
            if len(buffer) > 3:
                del buffer[:-3]
            return
        if start:
            del buffer[:start]
        if len(buffer) < HEADER_LEN:
            return

        length = struct.unpack_from("<H", buffer, 8)[0]
        if length < HEADER_LEN + 1 or length > 512:
            del buffer[:1]
            continue
        if len(buffer) < length:
            return

        packet = bytes(buffer[:length])
        del buffer[:length]
        try:
            yield decode_packet(packet)
        except (ValueError, struct.error):
            continue


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None, help="serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--list", action="store_true", help="list serial ports")
    parser.add_argument(
        "--duration", type=float, default=0, help="seconds to run (0 = forever)"
    )
    args = parser.parse_args()

    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print(
            "pyserial is required: uv run --with pyserial python payload_live.py",
            file=sys.stderr,
        )
        return 2

    if args.list:
        for port in list_ports.comports():
            print(f"{port.device}  {port.description}")
        return 0

    port = args.port
    if port is None:
        candidates = [p.device for p in list_ports.comports() if "usbmodem" in p.device]
        if not candidates:
            print("no /dev/cu.usbmodem* port found", file=sys.stderr)
            return 1
        port = candidates[-1]

    with serial.Serial(port, args.baud, timeout=0.25) as link:
        link.write(b"B")
        link.flush()
        print(f"# {port} (quiet packet mode)", file=sys.stderr)

        buffer = bytearray()
        count = 0
        started = time.monotonic()
        try:
            while True:
                if args.duration and time.monotonic() - started > args.duration:
                    break
                chunk = link.read(4096)
                if not chunk:
                    continue
                buffer.extend(chunk)
                for packet in frames(buffer):
                    print(fmt_packet(packet), flush=True)
                    count += 1
        except KeyboardInterrupt:
            pass
        finally:
            link.write(b"T")
            link.flush()
            print(f"# {count} packets", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
