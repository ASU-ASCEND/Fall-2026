#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Log every attached Pico with tio, all into one file: logs/ascend.log.

Both firmwares prefix every text line with their board name ("PNC " or
"Payload "), so the log file does not need to know which board it came from.
That is deliberate: macOS has no /dev/serial/by-id and both Pico 2W boards
enumerate as "Pico 2W" with /dev names that move when a board is replugged, so
routing a file by USB serial used to rot silently. A board that came back on the
other /dev name kept writing the other board's log, which reads as "board X
stopped logging" while it is actually logging into board Y's file.

One log, board name on every line, nothing to keep in sync:

    uv run scripts/tio_serial.py start    # one tio per Pico, detached
    uv run scripts/tio_serial.py status   # per-board last line, age, PIDs
    uv run scripts/tio_serial.py stop
    uv run scripts/tio_serial.py list     # every USB serial device, marked

Watch it in a terminal:

    tail -f logs/ascend.log
    tail -f logs/ascend.log | grep --line-buffered -a ' PNC '

tio reconnects on its own when a board drops and comes back on the same path;
the supervisor restarts it if the process exits, so a logging gap shows up in
`status` instead of silently ending the log.
"""

from __future__ import annotations

import argparse
import json
import os
import pty
import re
import select
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LOG_DIR = REPO_ROOT / "logs"
LOG_FILE = LOG_DIR / "ascend.log"
STATUS_FILE = LOG_DIR / "tio-status.log"  # tio's own connect/disconnect messages
STATE_FILE = LOG_DIR / ".tio-pids.json"

# tio's text output. The two boards print these tags themselves; see
# pnc-fsw/include/Logger.h and payload-fsw/include/Logger.h.
BOARD_RE = re.compile(
    r"^\[(?P<ts>\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d+)\] (?P<board>PNC|Payload)\b"
)
TAIL_BYTES = 512 * 1024
SILENT_AFTER_S = 15

ENTRY_RE = re.compile(r"^(?P<indent>[ |]*)\+-o ")
PROPERTY_RE = re.compile(
    r'^[ |]*"(?P<key>USB Serial Number|kUSBSerialNumberString|IOCalloutDevice)"'
    r'\s*=\s*"(?P<value>[^"]+)"'
)


@dataclass
class Device:
    serial: str | None
    port: str | None
    name: str


def ioreg_lines() -> list[str]:
    try:
        output = subprocess.run(
            ["ioreg", "-p", "IOService", "-w0", "-l"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"Cannot read USB device tree with ioreg: {exc}") from exc
    return output.splitlines()


def underlying_usb_devices(lines: list[str]) -> list[Device]:
    """Return every USB device with its serial number and callout port.

    ioreg prints a depth-indented tree, so descendants (the CDC ACM serial
    client) carry a deeper indent than the USB device that owns the serial
    number. Frames track that ancestry.
    """
    frames: list[dict] = []
    devices: list[Device] = []
    for line in lines:
        entry = ENTRY_RE.match(line)
        if entry:
            column = len(entry.group("indent"))
            while frames and frames[-1]["column"] >= column:
                frames.pop()
            label = line.split("+-o ", 1)[1].split("  <class")[0].strip()
            frames.append({"column": column, "label": label, "serial": None})
            continue

        prop = PROPERTY_RE.match(line)
        if not prop or not frames:
            continue
        key, value = prop.group("key"), prop.group("value")
        if key == "IOCalloutDevice":
            # The nearest frame with a serial number is the interface, which
            # inherits it from the USB device above it; take the topmost so the
            # device name is reported rather than "IOUSBHostInterface".
            owner = next((f for f in frames if f.get("serial")), None)
            if owner:
                devices.append(Device(owner["serial"], value, owner["label"]))
        else:
            frames[-1]["serial"] = value
    return devices


def pico_ports() -> list[Device]:
    """Attached Pico 2W boards, in device-path order.

    The name filter keeps the ESP32-S3 checkout board (also a /dev/cu.usbmodem*
    device) out of this log; it has its own logger.
    """
    return [
        d
        for d in sorted(underlying_usb_devices(ioreg_lines()), key=lambda d: d.port or "")
        if "Pico" in d.name and d.port
    ]


def port_busy(port: str) -> bool:
    """True if another process already has the port open."""
    if not shutil.which("lsof"):
        return False
    result = subprocess.run(["lsof", "-t", port], capture_output=True, text=True)
    return result.returncode == 0 and bool(result.stdout.strip())


def running() -> list[dict]:
    """Live logger processes recorded in the state file."""
    try:
        entries = json.loads(STATE_FILE.read_text())
    except (OSError, json.JSONDecodeError):
        return []
    live = []
    for entry in entries:
        try:
            os.kill(entry["pid"], 0)
        except (ProcessLookupError, KeyError, TypeError):
            continue
        except PermissionError:
            live.append(entry)
            continue
        live.append(entry)
    return live


def save_state(entries: list[dict]) -> None:
    if not entries:
        STATE_FILE.unlink(missing_ok=True)
        return
    STATE_FILE.write_text(json.dumps(entries, indent=2) + "\n")


def note(message: str) -> None:
    """Append a lifecycle line to the tio status log."""
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    with STATUS_FILE.open("a") as handle:
        handle.write(f"[{datetime.now().isoformat(timespec='seconds')}] {message}\n")


def supervise(device: str) -> int:
    """Keep one tio attached to `device`, forever.

    tio exits when it loses its terminal, and without a terminal it goes
    non-interactive and stops writing the log file at all, so it gets a real
    pty here. The pty cannot be left unread without filling up and blocking
    tio, so this loop drains and discards it; tio writes the log file itself.
    """
    master, slave = pty.openpty()
    os.set_blocking(master, False)
    try:
        while True:
            note(f"tio {device} starting")
            process = subprocess.Popen(
                ["tio", "--log", "--log-file", str(LOG_FILE), "--log-append", device],
                stdin=slave,
                stdout=slave,
                stderr=slave,
            )
            while process.poll() is None:
                readable, _, _ = select.select([master], [], [], 1.0)
                if readable:
                    try:
                        os.read(master, 65536)
                    except OSError:
                        time.sleep(0.1)
            note(f"tio {device} exited ({process.wait()}), restarting")
            time.sleep(2)
    finally:
        os.close(slave)


def start(ports: list[str]) -> int:
    if not shutil.which("tio"):
        print("tio is not installed: brew install tio", file=sys.stderr)
        return 1

    active = running()
    taken = {entry["device"] for entry in active}
    if ports:
        wanted = [Device(None, port, "requested") for port in ports]
    else:
        wanted = pico_ports()
        if not wanted:
            print("No Pico found on USB. Check the cable, then retry.", file=sys.stderr)
            return 1

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    LOG_FILE.touch()
    new = list(active)
    for device in wanted:
        assert device.port
        if device.port in taken:
            print(f"{device.port}: already logging")
            continue
        if port_busy(device.port):
            print(f"{device.port}: busy, another reader has it open; skipped")
            continue

        process = subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve()), "_supervise", device.port],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            cwd=REPO_ROOT,
        )
        new.append({"pid": process.pid, "device": device.port})
        print(f"{device.port}: logging to {LOG_FILE.relative_to(REPO_ROOT)}")

    save_state(new)
    print(f"\nWatch with:  tail -f {LOG_FILE.relative_to(REPO_ROOT)}")
    return 0


def stop() -> int:
    entries = running()
    if not entries:
        save_state([])
        print("No logger running.")
        return 0
    for entry in entries:
        try:
            os.killpg(entry["pid"], signal.SIGTERM)
        except ProcessLookupError:
            continue
        print(f"stopped pid {entry['pid']} ({entry['device']})")
    # Give the shell wrapper and tio a moment to close the port cleanly.
    for _ in range(20):
        if not running():
            break
        time.sleep(0.1)
    for entry in running():
        try:
            os.killpg(entry["pid"], signal.SIGKILL)
        except ProcessLookupError:
            continue
    save_state([])
    return 0


def last_lines() -> dict[str, tuple[str, str]]:
    """Board name -> (timestamp, line) for the newest line of each board."""
    if not LOG_FILE.exists():
        return {}
    size = LOG_FILE.stat().st_size
    with LOG_FILE.open("rb") as handle:
        handle.seek(max(0, size - TAIL_BYTES))
        blob = handle.read()
    seen: dict[str, tuple[str, str]] = {}
    for raw in blob.decode("utf-8", "replace").splitlines():
        match = BOARD_RE.match(raw)
        if match:
            seen[match["board"]] = (match["ts"], raw.strip())
    return seen


def status() -> int:
    entries = running()
    print(f"log      {LOG_FILE}")
    if LOG_FILE.exists():
        info = LOG_FILE.stat()
        size = info.st_size
        shown = f"{size / 1e6:.1f} MB" if size >= 1e6 else f"{size / 1e3:.1f} kB"
        print(
            f"         {shown}, last write "
            f"{time.time() - info.st_mtime:.1f} s ago"
        )
    if entries:
        for entry in entries:
            print(f"logger   pid {entry['pid']}  {entry['device']}")
    else:
        print("logger   not running (start it: uv run scripts/tio_serial.py start)")

    seen = last_lines()
    if not seen:
        print("\nNo board lines in the log yet.")
        return 0

    now = datetime.now()
    print("\nboard    age      last line")
    for board in ("PNC", "Payload"):
        if board not in seen:
            print(f"{board:<8} missing  no line tagged {board} in the tail")
            continue
        stamp, line = seen[board]
        try:
            age = (now - datetime.fromisoformat(stamp)).total_seconds()
        except ValueError:
            age = float("nan")
        note = "  <-- silent" if age > SILENT_AFTER_S else ""
        print(f"{board:<8} {age:>5.0f} s  {line[:110]}{note}")
    return 0


def list_devices() -> int:
    devices = underlying_usb_devices(ioreg_lines())
    if not devices:
        print("No USB serial devices found.")
        return 1
    logging = {entry["device"]: entry["pid"] for entry in running()}
    for device in sorted(devices, key=lambda d: d.port or ""):
        who = "logged" if "Pico" in device.name else "ignored"
        pid = logging.get(device.port or "")
        tail = f"pid {pid}" if pid else ""
        busy = "busy" if device.port and not pid and port_busy(device.port) else ""
        print(
            f"{device.port or 'no port':<24} {who:<8} "
            f"{device.name:<20} {device.serial} {tail} {busy}".rstrip()
        )
    return 0


def main() -> int:
    # Hidden entry point: one of these runs detached per board, so `start` can
    # exit and the logging outlives the terminal.
    if len(sys.argv) >= 3 and sys.argv[1] == "_supervise":
        return supervise(sys.argv[2])

    parser = argparse.ArgumentParser(
        description="Log the attached Pico boards to one timestamped log file."
    )
    parser.add_argument("command", choices=["start", "stop", "status", "list"])
    parser.add_argument(
        "ports", nargs="*", help="start only these ports instead of every Pico"
    )
    args = parser.parse_args()

    if args.command == "start":
        return start(args.ports)
    if args.command == "stop":
        return stop()
    if args.command == "status":
        return status()
    return list_devices()


if __name__ == "__main__":
    sys.exit(main())
