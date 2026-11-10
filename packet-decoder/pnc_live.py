#!/usr/bin/env python3
"""Read a live PnC board on USB serial and decode its packets.

Two firmware output modes are supported:

* ``csv``    - the default ``PKT,...`` line printed once per sample.
* ``binary`` - raw 87-byte packets. The script sends ``B`` to switch the board
               into this mode and ``T`` to switch it back.

Examples::

    uv run --with pyserial python pnc_live.py --port /dev/cu.usbmodem101
    uv run --with pyserial python pnc_live.py --port /dev/cu.usbmodem101 --binary
    uv run --with pyserial python pnc_live.py --list
"""

from __future__ import annotations

import argparse
import struct
import sys
import time

SYNC = 0xDEADCAFE
SYNC_BYTES = struct.pack("<I", SYNC)
PACKET_FMT = "<IIBBfIfIfIfffIB?BBiiiiiiiib"
PACKET = struct.Struct(PACKET_FMT)
PACKET_LEN = PACKET.size  # 87

CSV_FIELDS = 26


def fmt(values: tuple) -> str:
    (
        _sync,
        uptime,
        _pid,
        _length,
        pico_c,
        rtc_unix,
        bme_c,
        bme_pa,
        bme_rh,
        bme_ohm,
        ina_ma,
        ina_mv,
        ina_mw,
        gps_unix,
        fix_type,
        fix_ok,
        siv,
        _pad,
        lat,
        lon,
        alt,
        _vn,
        _ve,
        _vd,
        _hacc,
        _vacc,
        checksum,
    ) = values
    return (
        f"t={uptime / 1000.0:7.2f}s "
        f"pico={pico_c:5.1f}C rtc={rtc_unix} "
        f"bme={bme_c:5.1f}C/{bme_pa}Pa/{bme_rh:5.1f}%RH/{bme_ohm}ohm "
        f"ina={ina_ma:7.2f}mA/{ina_mv:8.2f}mV/{ina_mw:7.2f}mW "
        f"gps=fix{fix_type}/ok{int(fix_ok)}/siv{siv} "
        f"lat={lat / 1e7:.7f} lon={lon / 1e7:.7f} alt={alt / 1000.0:.1f}m "
        f"ck={checksum}"
    )


def parse_csv_line(line: str):
    parts = line.strip().split(",")
    if len(parts) != CSV_FIELDS or parts[0] != "PKT":
        return None
    try:
        return (
            parts[0],
            int(parts[1]),
            int(parts[2]),
            int(parts[3]),
            float(parts[4]),
            int(parts[5]),
            float(parts[6]),
            int(parts[7]),
            float(parts[8]),
            int(parts[9]),
            float(parts[10]),
            float(parts[11]),
            float(parts[12]),
            int(parts[13]),
            int(parts[14]),
            int(parts[15]),
            int(parts[16]),
            0,
            int(parts[17]),
            int(parts[18]),
            int(parts[19]),
            int(parts[20]),
            int(parts[21]),
            int(parts[22]),
            int(parts[23]),
            int(parts[24]),
            int(parts[25]),
        )
    except ValueError:
        return None


def binary_frames(buffer: bytearray):
    while True:
        start = buffer.find(SYNC_BYTES)
        if start < 0:
            # Keep the tail in case a sync straddles reads.
            if len(buffer) > 3:
                del buffer[:-3]
            return
        if start:
            del buffer[:start]
        if len(buffer) < PACKET_LEN:
            return
        packet = bytes(buffer[:PACKET_LEN])
        length = packet[9]
        if length != PACKET_LEN or sum(packet) & 0xFF != 0:
            del buffer[:1]
            continue
        yield PACKET.unpack(packet)
        del buffer[:PACKET_LEN]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None, help="serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--binary", action="store_true", help="request raw packets")
    parser.add_argument("--list", action="store_true", help="list serial ports")
    parser.add_argument(
        "--duration", type=float, default=0, help="seconds to run (0 = forever)"
    )
    args = parser.parse_args()

    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("pyserial is required: uv run --with pyserial python pnc_live.py", file=sys.stderr)
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
        port = candidates[0]

    with serial.Serial(port, args.baud, timeout=0.25) as link:
        if args.binary:
            link.write(b"B")
            link.flush()
        else:
            link.write(b"T")
            link.flush()

        print(f"# {port} mode={'binary' if args.binary else 'csv'}", file=sys.stderr)

        buffer = bytearray()
        count = 0
        bad = 0
        started = time.monotonic()
        try:
            while True:
                if args.duration and time.monotonic() - started > args.duration:
                    break
                chunk = link.read(4096)
                if not chunk:
                    continue
                if args.binary:
                    buffer.extend(chunk)
                    for values in binary_frames(buffer):
                        print(fmt(values), flush=True)
                        count += 1
                else:
                    buffer.extend(chunk)
                    while b"\n" in buffer:
                        line, _, rest = buffer.partition(b"\n")
                        buffer = bytearray(rest)
                        text = line.decode(errors="replace")
                        if not text.startswith("PKT,"):
                            print(text, file=sys.stderr)
                            continue
                        values = parse_csv_line(text)
                        if values is None:
                            bad += 1
                            print(f"bad line: {text}", file=sys.stderr)
                            continue
                        print(fmt(values), flush=True)
                        count += 1
        except KeyboardInterrupt:
            pass
        finally:
            if args.binary:
                link.write(b"T")
                link.flush()
            print(f"# {count} packets, {bad} bad", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
