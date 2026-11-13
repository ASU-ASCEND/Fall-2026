#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["radiacode>=0.4.0", "pyusb>=1.2.1"]
# ///
"""Dump the RadiaCode's internal observation log to disk.

The RadiaCode keeps its own timestamped log of measurements and events (about
a thousand hours). Reading it is a *transfer*: the records leave the device and
are deleted from it, exactly like the phone/desktop app does. So this script
writes everything to disk before printing anything, and refuses to start if the
output file already exists.

    uv run scripts/dump_radiacode.py                 # USB
    uv run scripts/dump_radiacode.py --ble <mac>     # Bluetooth (needs bleak)
    uv run scripts/dump_radiacode.py --out /some/dir

Output, per dump:

    <out>/rc<serial>_<UTC stamp>.jsonl   every record, full fidelity
    <out>/rc<serial>_<UTC stamp>.csv     flat table for pandas/plotting
    <out>/rc<serial>_<UTC stamp>.meta.json  device info + record summary
    <out>/rc<serial>_<UTC stamp>.spectrum.json  current and accumulated spectra
"""

from __future__ import annotations

import argparse
import csv
import dataclasses
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "data-processing" / "data" / "radiacode"

CSV_FIELDS = [
    "kind",
    "dt_utc",
    "count",
    "count_rate",
    "count_rate_err",
    "dose_rate",
    "dose_rate_err",
    "flags",
    "real_time_flags",
    "duration_s",
    "dose",
    "temperature_c",
    "charge_level",
    "event",
    "event_param1",
]


def record_to_row(rec) -> dict:
    """Flatten one device record into CSV columns."""
    row = {f: "" for f in CSV_FIELDS}
    row["kind"] = type(rec).__name__

    dt = getattr(rec, "dt", None)
    if dt is not None:
        row["dt_utc"] = dt.astimezone(timezone.utc).isoformat()

    for field in dataclasses.fields(rec):
        name = field.name
        value = getattr(rec, name)
        if name == "dt":
            continue
        if name == "duration" and isinstance(value, (int, float)):
            row["duration_s"] = value
        elif name == "duration" and hasattr(value, "total_seconds"):
            row["duration_s"] = value.total_seconds()
        elif name == "temperature":
            row["temperature_c"] = value
        elif name == "event":
            row["event"] = getattr(value, "name", value)
        elif name in row:
            row[name] = value
    return row


def record_to_json(rec) -> dict:
    out = {"kind": type(rec).__name__}
    for field in dataclasses.fields(rec):
        value = getattr(rec, field.name)
        if field.name == "dt":
            out["dt_utc"] = value.astimezone(timezone.utc).isoformat()
        elif hasattr(value, "total_seconds"):
            out[field.name + "_s"] = value.total_seconds()
        elif hasattr(value, "name") and not isinstance(value, (int, float)):
            out[field.name] = value.name
        else:
            out[field.name] = value
    return out


def write_atomic(path: Path, write) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", newline="") as fh:
        write(fh)
    os.replace(tmp, path)


def ensure_libusb() -> None:
    """Prime pyusb with Homebrew's libusb on macOS.

    pyusb resolves libusb through dyld's search paths, which do not include
    /opt/homebrew, so a plain `brew install libusb` still gives
    "No backend available". Load the dylib ourselves and hand pyusb a ready
    backend.
    """
    if sys.platform != "darwin":
        return

    import ctypes

    import usb.backend.libusb1 as libusb1

    if libusb1._lib_object is not None:
        return

    candidates = [
        "/opt/homebrew/lib/libusb-1.0.dylib",
        "/usr/local/lib/libusb-1.0.dylib",
        "/opt/homebrew/opt/libusb/lib/libusb-1.0.dylib",
    ]
    for candidate in candidates:
        if not Path(candidate).exists():
            continue
        lib = ctypes.CDLL(candidate)
        libusb1._setup_prototypes(lib)
        libusb1._lib = lib
        libusb1._lib_object = libusb1._LibUSB(lib)
        return

    raise SystemExit("libusb not found: brew install libusb "
                     "(or use --ble <mac>)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT,
                    help=f"output directory (default: {DEFAULT_OUT})")
    ap.add_argument("--ble", metavar="MAC",
                    help="connect over Bluetooth Low Energy to this MAC instead of USB")
    ap.add_argument("--no-spectrum", action="store_true",
                    help="skip the spectrum download")
    args = ap.parse_args()

    try:
        ensure_libusb()
        from radiacode import RadiaCode
    except ImportError:
        print("radiacode is not importable; run through `uv run`", file=sys.stderr)
        return 2

    print("connecting...", end=" ", flush=True)
    device = RadiaCode(bluetooth_mac=args.ble) if args.ble else RadiaCode()
    print("ok")

    meta = {}
    for name in ("serial_number", "hw_serial_number", "fw_version", "status"):
        try:
            value = getattr(device, name)()
            meta[name] = str(value)
        except Exception as exc:  # a missing field must not cost us the log
            meta[name] = f"unavailable: {exc}"
    print(f"serial {meta['serial_number']}  fw {meta['fw_version']}  "
          f"status {meta['status']}")

    args.out.mkdir(parents=True, exist_ok=True)
    stamp = meta["serial_number"].replace("#", "_").replace("/", "_")
    stamp = f"rc{stamp}_{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}"
    base = args.out / stamp
    if any(base.with_suffix(s).exists() for s in (".jsonl", ".csv")):
        print(f"refusing to overwrite an existing dump: {base}.*", file=sys.stderr)
        return 1

    # Destructive: the records are removed from the device as they are read.
    print("reading the device log (records are deleted from the device)...",
          end=" ", flush=True)
    records = device.data_buf()
    print(f"{len(records)} records")

    rows = [record_to_row(r) for r in records]
    jsons = [record_to_json(r) for r in records]

    def dump_jsonl(fh):
        for item in jsons:
            fh.write(json.dumps(item) + "\n")

    def dump_csv(fh):
        writer = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    write_atomic(base.with_suffix(".jsonl"), dump_jsonl)
    write_atomic(base.with_suffix(".csv"), dump_csv)

    spectrum_files = {}
    if not args.no_spectrum:
        for label, fn in (("current", device.spectrum), ("accum", device.spectrum_accum)):
            try:
                spec = fn()
                payload = {
                    "duration_s": spec.duration.total_seconds(),
                    "a0": spec.a0,
                    "a1": spec.a1,
                    "a2": spec.a2,
                    "counts": list(spec.counts),
                }
                path = base.with_suffix(f".spectrum.{label}.json")
                write_atomic(path, lambda fh, p=payload: json.dump(p, fh))
                spectrum_files[label] = str(path.name)
            except Exception as exc:
                spectrum_files[label] = f"unavailable: {exc}"

    kinds: dict[str, int] = {}
    for row in rows:
        kinds[row["kind"]] = kinds.get(row["kind"], 0) + 1
    stamps = sorted(r["dt_utc"] for r in rows if r["dt_utc"])

    meta.update({
        "records": len(records),
        "record_kinds": kinds,
        "first_record_utc": stamps[0] if stamps else None,
        "last_record_utc": stamps[-1] if stamps else None,
        "spectra": spectrum_files,
        "transport": f"ble:{args.ble}" if args.ble else "usb",
        "files": [base.with_suffix(s).name for s in (".jsonl", ".csv")],
    })
    write_atomic(base.with_suffix(".meta.json"),
                 lambda fh: json.dump(meta, fh, indent=2))

    print(f"saved {base.name}.jsonl / .csv / .meta.json in {args.out}")
    if stamps:
        print(f"  span {stamps[0]} .. {stamps[-1]}")
    print(f"  kinds {kinds}")
    if spectrum_files:
        print(f"  spectra {spectrum_files}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
