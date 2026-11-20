# ASU StratoDevils ASCEND Software Fall 2026
Software for the ASCEND Fall 2026 Payload and Power and Control Boards 


## Power and Control Board (PnC Board)
Handles power system monitoring, overall flight control, backup systems, state management. 

## Payload Board
Handles data collection from *External* and *Science* Boards

## Identify connected boards

After connecting or reconnecting USB, identify both ports before flashing:

```sh
uv run scripts/identify_boards.py
```

The script listens passively for firmware output, sends no commands, and returns
a nonzero exit status if the mapping is incomplete or ambiguous. A port number
can change between connections; never use an old mapping without checking it.
Use `--seconds 15` for a longer listen or `--json` for machine-readable output.

## Serial consoles with tio

`tio` is installed and configured on this Mac. One-time setup (repeat only on a
fresh machine):

```sh
brew install tio
mkdir -p ~/.config/tio
ln -sf "$PWD/scripts/tio-config" ~/.config/tio/config
```

`scripts/tio-config` enables ISO 8601 line timestamps and append-only logging in
`logs/`, and includes `~/.config/tio/ports.conf`, which holds the `pnc` and
`payload` profiles. It also sets `map = INLCRNL`: tio leaves the terminal in raw
mode, so a bare LF from the board does not return the cursor to the left edge
and long payload packet lines stair-step across the pane. The mapping keeps
every line starting at column 0; log files stay plain LF.

macOS has no `/dev/serial/by-id`, and both Pico 2W boards enumerate with the
same name, so ports are matched by USB serial number instead of port number:

```sh
uv run scripts/tio_serial.py pnc      # PnC console
uv run scripts/tio_serial.py payload  # payload console
uv run scripts/tio_serial.py both     # both, in a tmux session
uv run scripts/tio_serial.py status   # current serial -> port mapping
uv run scripts/tio_serial.py list     # every USB serial device with its port
```

Each launch reads the USB tree, rewrites `ports.conf` with the current device
paths, and runs `tio <profile>`. tio reconnects to the same device path on its
own; if a board is moved to a different USB port, rerun the same command to
re-resolve it. Quit with `ctrl-t q`.

The board USB serial numbers are recorded in `BOARDS` in
`scripts/tio_serial.py`; update them if a Pico is replaced (`list` prints the
serial of every attached device).

Output is appended to `logs/pnc.log` and `logs/payload.log`, one timestamped
line each:

```sh
tail -f logs/pnc.log
grep -a "STATUS" logs/payload.log
grep -a "2026-11-16T13:3" logs/*.log
```

`logs/` is gitignored. Payload packets are binary, so use `grep -a` on them.

For a supervised soak, use the newly identified ports and a new output path:

```sh
uv run scripts/monitor_stack.py --pnc /dev/cu.usbmodem101 \
  --payload /dev/cu.usbmodem1101 --output soak.jsonl
```

This records status lines for 60 minutes by default. It does not certify SD
persistence or pass/fail; inspect the records and decode both cards afterward.
