#!/usr/bin/env python3
"""Decode payload-board ``RAWDATAn.BIN`` packet files written by payload-fsw.

Packet layout (little endian, packed)::

    char[4]  sync        "ASU!"
    uint32   sensor mask bit (N-1-i) is set when sensors[i] is in this packet
    uint16   length      total packet length including the checksum
    uint32   millis      board uptime when the packet was built
    ...      sensor payloads, in sensors[] order, only for set mask bits
    int8     checksum    two's complement so the byte sum is 0 mod 256

The mask has one sentinel bit on top: for N compiled sensors, the highest set
bit is bit N, and sensors[i] owns bit (N-1-i). That matches Sensor::getDataPacket
in payload-fsw.

The default layout is the FA26 Internal (payload) board build::

    sensors[0] PicoTemp    float temp_c                   4 bytes
    sensors[1] ICM20948    float ax,ay,az,gx,gy,gz,mx,my,mz,temp  40 bytes
    sensors[2] PCF8523     uint16 year, u8 mon, day, hour, min, sec  7 bytes
    sensors[3] Geiger      float cps, float dose          8 bytes

If PAYLOAD_ENABLE_STRATOSENSE is turned on, add the StratoSense entries in the
same order they appear in payload-fsw/src/main.cpp before decoding those files.

Usage::

    uv run python parse_payload.py RAWDATA0.BIN
    uv run python parse_payload.py RAWDATA*.BIN --summary
"""

from __future__ import annotations

import argparse
import glob
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

SYNC = b"ASU!"
HEADER = struct.Struct("<4sIHI")  # sync, mask, length, millis
HEADER_LEN = HEADER.size  # 14


@dataclass(frozen=True)
class Field:
    name: str
    fmt: str
    scale: float = 1.0
    unit: str = ""


@dataclass(frozen=True)
class SensorLayout:
    name: str
    fields: tuple[Field, ...]

    @property
    def size(self) -> int:
        return struct.calcsize("<" + "".join(f.fmt for f in self.fields))


INTERNAL_LAYOUT: tuple[SensorLayout, ...] = (
    SensorLayout("PicoTemp", (Field("pico_temp_c", "f", 1.0, "C"),)),
    SensorLayout(
        "ICM20948",
        (
            Field("accel_x", "f", 1.0, "m/s2"),
            Field("accel_y", "f", 1.0, "m/s2"),
            Field("accel_z", "f", 1.0, "m/s2"),
            Field("gyro_x", "f", 1.0, "rad/s"),
            Field("gyro_y", "f", 1.0, "rad/s"),
            Field("gyro_z", "f", 1.0, "rad/s"),
            Field("mag_x", "f", 1.0, "uT"),
            Field("mag_y", "f", 1.0, "uT"),
            Field("mag_z", "f", 1.0, "uT"),
            Field("icm_temp_c", "f", 1.0, "C"),
        ),
    ),
    SensorLayout(
        "PCF8523",
        (
            Field("year", "H"),
            Field("month", "B"),
            Field("day", "B"),
            Field("hour", "B"),
            Field("minute", "B"),
            Field("second", "B"),
        ),
    ),
    SensorLayout(
        "Geiger",
        (Field("cps", "f", 1.0, "cps"), Field("dose", "f", 1.0, "uSv/h")),
    ),
)

DUMMY_EXTERNAL = SensorLayout(
    "DummyExternal",
    tuple(Field(f"dummy_{i}", "f") for i in range(16)),
)

# The mask sentinel is the compiled sensor count, so a payload built with the
# dummy external block (5 sensors) and one without it (4) can both be decoded.
LAYOUTS: dict[int, tuple[SensorLayout, ...]] = {
    4: INTERNAL_LAYOUT,
    5: INTERNAL_LAYOUT + (DUMMY_EXTERNAL,),
}


def unpack_fields(fields: tuple[Field, ...], data: bytes) -> dict[str, float | int]:
    values: dict[str, float | int] = {}
    fmt = struct.Struct("<" + "".join(f.fmt for f in fields))
    raw = fmt.unpack_from(data)
    for field, value in zip(fields, raw):
        if isinstance(value, float) and field.scale != 1.0:
            value = value * field.scale
        values[field.name] = value
    return values


def decode_packet(packet: bytes) -> dict:
    sync, mask, length, uptime_ms = HEADER.unpack_from(packet)

    if sync != SYNC or length != len(packet):
        raise ValueError("bad header")

    # The stored byte is the two's complement of the sum of the other bytes,
    # so the whole frame (including that byte) sums to 0 mod 256.
    if sum(packet) & 0xFF != 0:
        raise ValueError("bad checksum")

    sentinel = mask.bit_length() - 1
    layout = LAYOUTS.get(sentinel)
    if layout is None:
        raise ValueError(
            f"unknown sensor count {sentinel}; known layouts: "
            f"{sorted(LAYOUTS)}"
        )

    out: dict = {"uptime_ms": uptime_ms, "mask": mask, "sensors": {}}
    offset = HEADER_LEN
    for index, sensor in enumerate(layout):
        bit = sentinel - 1 - index
        if not (mask & (1 << bit)):
            continue
        chunk = packet[offset : offset + sensor.size]
        if len(chunk) != sensor.size:
            raise ValueError(f"truncated {sensor.name}")
        out["sensors"][sensor.name] = unpack_fields(sensor.fields, chunk)
        offset += sensor.size

    if offset != len(packet) - 1:
        raise ValueError("packet length does not match sensors")
    return out


def decode_file(path: Path) -> tuple[list[dict], int]:
    raw = path.read_bytes()
    packets: list[dict] = []
    skipped = 0
    offset = 0
    while offset + HEADER_LEN <= len(raw):
        if raw[offset : offset + 4] != SYNC:
            skipped += 1
            nxt = raw.find(SYNC, offset + 1)
            if nxt < 0:
                break
            offset = nxt
            continue

        length = struct.unpack_from("<H", raw, offset + 8)[0]
        if length < HEADER_LEN + 1 or offset + length > len(raw):
            skipped += 1
            offset += 1
            continue

        try:
            packets.append(decode_packet(raw[offset : offset + length]))
            offset += length
        except (ValueError, struct.error):
            skipped += 1
            offset += 1

    return packets, skipped


def fmt_packet(packet: dict) -> str:
    parts = [f"t={packet['uptime_ms'] / 1000.0:8.2f}s"]
    for name, values in packet["sensors"].items():
        if name == "ICM20948":
            parts.append(
                "icm={:.2f}C {:.2f}/{:.2f}/{:.2f}m/s2".format(
                    values["icm_temp_c"],
                    values["accel_x"],
                    values["accel_y"],
                    values["accel_z"],
                )
            )
        elif name == "PCF8523":
            parts.append(
                "rtc={:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}".format(
                    values["year"],
                    values["month"],
                    values["day"],
                    values["hour"],
                    values["minute"],
                    values["second"],
                )
            )
        elif name == "Geiger":
            parts.append(f"geiger={values['cps']:.2f}cps/{values['dose']:.3f}uSv")
        elif name == "PicoTemp":
            parts.append(f"pico={values['pico_temp_c']:.2f}C")
        elif name == "DummyExternal":
            parts.append(
                "dummy=[{:.2f},{:.2f},{:.2f},...]".format(
                    values["dummy_0"], values["dummy_1"], values["dummy_2"]
                )
            )
        else:
            parts.append(f"{name}={values}")
    return " ".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", help="RAWDATAn.BIN file(s) or glob(s)")
    parser.add_argument("--summary", action="store_true", help="counts only")
    parser.add_argument("--json", action="store_true", help="JSON lines")
    args = parser.parse_args()

    paths: list[Path] = []
    for pattern in args.files:
        matches = sorted(glob.glob(pattern))
        paths.extend(Path(p) for p in matches or [pattern])

    total = 0
    skipped_total = 0
    for path in paths:
        try:
            packets, skipped = decode_file(path)
        except FileNotFoundError:
            print(f"missing: {path}", file=sys.stderr)
            continue

        total += len(packets)
        skipped_total += skipped
        if args.summary:
            print(f"{path}: {len(packets)} packets, {skipped} skipped bytes")
        else:
            for packet in packets:
                if args.json:
                    import json

                    print(json.dumps(packet))
                else:
                    print(fmt_packet(packet))

    if args.summary:
        print(f"total: {total} packets, {skipped_total} skipped bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
