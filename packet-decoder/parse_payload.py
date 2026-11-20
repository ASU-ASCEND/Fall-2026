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

The default layout is the FA26 Internal (payload) board with the External
sensor board on J200::

    sensors[0] PicoTemp    float temp_c                                4 bytes
    sensors[1] ICM20948    float ax,ay,az,gx,gy,gz,mx,my,mz,temp      40 bytes
    sensors[2] PCF8523     uint16 year, u8 mon, day, hour, min, sec    7 bytes
    sensors[3] TMP117      float temp_c                                4 bytes
    sensors[4] BME688      f32 temp, u32 pressure, f32 RH, u32 gas    16 bytes
    sensors[5] ENS160      u8 aqi, u16 tvoc, u16 eco2                  5 bytes
    sensors[6] SHTC3       float temp, float RH                        8 bytes

Optional blocks, in this order after the internal sensors: Geiger (8 bytes,
PAYLOAD_ENABLE_GEIGER), the three rework-blocked External parts (AS7331 12,
Ozone 2, and the BMP390 slot — empty in old layouts, but carrying the Science
board's live BMP390 in the new build: 20 bytes, double temp, double pressure,
float altitude), then the Science board's TMP117 (4 bytes). A sensor that has no
new data drops its mask bit, so packet length varies within a layout.

The decoder matches known layouts by sensor count and byte count. Ambiguous
layouts are rejected rather than silently assigning values to the wrong sensor.

Usage::

    uv run python parse_payload.py RAWDATA0.BIN
    uv run python parse_payload.py RAWDATA*.BIN --summary
"""

from __future__ import annotations

import argparse
import glob
import re
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

# External board (J200): the four devices that pass verification. Order matches
# payload-fsw/src/main.cpp when PAYLOAD_ENABLE_EXTERNAL is 1.
EXTERNAL_LAYOUT: tuple[SensorLayout, ...] = (
    SensorLayout("TMP117", (Field("temp_c", "f", 1.0, "C"),)),
    SensorLayout(
        "BME688",
        (
            Field("temp_c", "f", 1.0, "C"),
            Field("pressure_pa", "I", 1.0, "Pa"),
            Field("humidity_pct", "f", 1.0, "%RH"),
            Field("gas_ohm", "I", 1.0, "ohm"),
        ),
    ),
    SensorLayout(
        "ENS160",
        (
            Field("aqi", "B"),
            Field("tvoc_ppb", "H", 1.0, "ppb"),
            Field("eco2_ppm", "H", 1.0, "ppm"),
        ),
    ),
    SensorLayout(
        "SHTC3",
        (
            Field("temp_c", "f", 1.0, "C"),
            Field("humidity_pct", "f", 1.0, "%RH"),
        ),
    ),
)

STRATOSENSE_LAYOUT = (
    SensorLayout("AS7331", (
        Field("uva_uw_cm2", "f"),
        Field("uvb_uw_cm2", "f"),
        Field("uvc_uw_cm2", "f"),
    )),
    SensorLayout("ReservedBMP390", ()),
    SensorLayout("Ozone", (Field("ppb", "h"),)),
)

# Science board (Science boardFALL2026, payload I2C0 via J202/J203): its BMP390
# U1 (0x76) fills the slot the dead External-board copy reserved, and its
# TMP117 U2 (0x48) appends a new final slot. The BMP390 driver writes
# double temperature, double pressure, float altitude = 20 bytes.
STRATOSENSE_SCIENCE_LAYOUT = (
    STRATOSENSE_LAYOUT[0],  # AS7331, same slot
    SensorLayout(
        "BMP390",
        (
            Field("bmp_temp_c", "d", 1.0, "C"),
            Field("bmp_pressure_pa", "d", 1.0, "Pa"),
            Field("bmp_alt_m", "f", 1.0, "m"),
        ),
    ),
    STRATOSENSE_LAYOUT[2],  # Ozone, same slot
)
SCIENCE_LAYOUT = (
    SensorLayout("TMP117Sci", (Field("temp_c", "f", 1.0, "C"),)),
)

# Geiger is compiled out on this build (PAYLOAD_ENABLE_GEIGER 0 in
# payload-fsw/include/PayloadConfig.h), so the same sensor count can describe
# two different packets: {temp, icm, rtc, geiger} or {temp, icm, rtc, dummy}.
INTERNAL_NO_GEIGER_LAYOUT = tuple(s for s in INTERNAL_LAYOUT if s.name != "Geiger")

# The mask sentinel is the compiled sensor count. Each count maps to every
# layout that has it; the packet length (with only the mask-set sensors) then
# picks the one that was built.
LAYOUTS: dict[int, tuple[tuple[SensorLayout, ...], ...]] = {
    3: (INTERNAL_NO_GEIGER_LAYOUT,),
    4: (INTERNAL_LAYOUT, INTERNAL_NO_GEIGER_LAYOUT + (DUMMY_EXTERNAL,)),
    5: (INTERNAL_LAYOUT + (DUMMY_EXTERNAL,),),
    # External board data on the wire (default build).
    7: (INTERNAL_NO_GEIGER_LAYOUT + EXTERNAL_LAYOUT,),
    8: (
        INTERNAL_LAYOUT + EXTERNAL_LAYOUT,
        INTERNAL_NO_GEIGER_LAYOUT + EXTERNAL_LAYOUT + (DUMMY_EXTERNAL,),
    ),
    9: (INTERNAL_LAYOUT + EXTERNAL_LAYOUT + (DUMMY_EXTERNAL,),),
    10: (INTERNAL_NO_GEIGER_LAYOUT + EXTERNAL_LAYOUT + STRATOSENSE_LAYOUT,),
    11: (
        INTERNAL_LAYOUT + EXTERNAL_LAYOUT + STRATOSENSE_LAYOUT,
        # Science-board build (no geiger): live BMP390 in the old reserved slot,
        # TMP117Sci appended. Old 11-sensor geiger captures still match by
        # length against the first candidate.
        INTERNAL_NO_GEIGER_LAYOUT + EXTERNAL_LAYOUT
        + STRATOSENSE_SCIENCE_LAYOUT + SCIENCE_LAYOUT,
    ),
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
    candidates = LAYOUTS.get(sentinel)
    if candidates is None:
        raise ValueError(
            f"unknown sensor count {sentinel}; known layouts: "
            f"{sorted(LAYOUTS)}"
        )

    # Same sensor count, different sensors: match on the packet length. The
    # mask decides which sensors are present, so a packet where ENS160 had no
    # new data is shorter than the full layout.
    def sent_bytes(candidate: tuple[SensorLayout, ...]) -> int:
        total = HEADER_LEN + 1  # header + checksum
        for index, sensor in enumerate(candidate):
            bit = sentinel - 1 - index
            if mask & (1 << bit):
                total += sensor.size
        return total

    matches = [candidate for candidate in candidates if sent_bytes(candidate) == len(packet)]
    if not matches:
        raise ValueError(
            f"no {sentinel}-sensor layout matches a {len(packet)}-byte packet"
        )
    active = lambda candidate: tuple(
        sensor for index, sensor in enumerate(candidate)
        if mask & (1 << (sentinel - 1 - index))
    )
    if any(active(candidate) != active(matches[0]) for candidate in matches[1:]):
        raise ValueError("ambiguous sensor layout; build configuration required")
    layout = matches[0]
    if any(sensor.name.startswith("Reserved") for sensor in active(layout)):
        raise ValueError("reserved sensor bit is set")

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
    while offset < len(raw):
        if raw[offset : offset + 4] != SYNC:
            nxt = raw.find(SYNC, offset + 1)
            if nxt < 0:
                skipped += len(raw) - offset
                break
            skipped += nxt - offset
            offset = nxt
            continue

        if offset + HEADER_LEN > len(raw):
            skipped += len(raw) - offset
            break
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
        elif name == "TMP117":
            parts.append(f"tmp117={values['temp_c']:.2f}C")
        elif name == "TMP117Sci":
            parts.append(f"tmp117_sci={values['temp_c']:.2f}C")
        elif name == "BMP390":
            parts.append(
                "bmp390={:.2f}C {:.0f}Pa {:.0f}m".format(
                    values["bmp_temp_c"],
                    values["bmp_pressure_pa"],
                    values["bmp_alt_m"],
                )
            )
        elif name == "BME688":
            parts.append(
                "bme={:.2f}C {:.0f}Pa {:.1f}%RH {:.0f}ohm".format(
                    values["temp_c"],
                    values["pressure_pa"],
                    values["humidity_pct"],
                    values["gas_ohm"],
                )
            )
        elif name == "ENS160":
            parts.append(
                "aqi={} tvoc={}ppb eco2={}ppm".format(
                    values["aqi"], values["tvoc_ppb"], values["eco2_ppm"]
                )
            )
        elif name == "SHTC3":
            parts.append(
                "shtc3={:.2f}C {:.1f}%RH".format(
                    values["temp_c"], values["humidity_pct"]
                )
            )
        elif name == "AS7331":
            parts.append(
                "uv={:.3f}/{:.3f}/{:.3f}uW/cm2".format(
                    values["uva_uw_cm2"], values["uvb_uw_cm2"], values["uvc_uw_cm2"]
                )
            )
        elif name == "Ozone":
            parts.append(f"ozone={values['ppb']}ppb")
        else:
            parts.append(f"{name}={values}")
    return " ".join(parts)


def natural_key(path: str):
    """Sort RAWDATA2.BIN before RAWDATA10.BIN."""
    return [int(part) if part.isdigit() else part.lower()
            for part in re.split(r"(\d+)", path)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", help="RAWDATAn.BIN file(s) or glob(s)")
    parser.add_argument("--summary", action="store_true", help="counts only")
    parser.add_argument("--json", action="store_true", help="JSON lines")
    args = parser.parse_args()

    paths: list[Path] = []
    for pattern in args.files:
        matches = sorted(glob.glob(pattern), key=natural_key)
        paths.extend(Path(p) for p in matches or [pattern])

    total = 0
    skipped_total = 0
    missing_input = False
    session = 0
    last_uptime = None
    for path in paths:
        try:
            packets, skipped = decode_file(path)
        except FileNotFoundError:
            print(f"missing: {path}", file=sys.stderr)
            missing_input = True
            continue

        total += len(packets)
        skipped_total += skipped
        if args.summary:
            print(f"{path}: {len(packets)} packets, {skipped} skipped bytes")
        else:
            for packet in packets:
                uptime = packet["uptime_ms"]
                if last_uptime is not None and uptime < last_uptime:
                    session += 1
                last_uptime = uptime
                if args.json:
                    import json

                    packet["session"] = session
                    print(json.dumps(packet))
                else:
                    print(f"s={session} {fmt_packet(packet)}")

    if args.summary:
        print(f"total: {total} packets, {skipped_total} skipped bytes")
    return 1 if missing_input else 0


if __name__ == "__main__":
    raise SystemExit(main())
