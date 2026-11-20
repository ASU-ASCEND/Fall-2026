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
`logs/`. It also sets `map = INLCRNL`: tio leaves the terminal in raw mode, so a
bare LF from the board does not return the cursor to the left edge and long
payload packet lines stair-step across the pane. The mapping keeps every line
starting at column 0; log files stay plain LF.

Both firmwares prefix every line with their board name (`PNC ` / `Payload `), so
there is one log file and nothing maps a board to a file name. That mapping used
to rot: macOS has no `/dev/serial/by-id` and a replugged Pico comes back on a
different `/dev/cu.usbmodem*` name, so a board could silently start writing the
other board's log.

```sh
uv run scripts/tio_serial.py start    # one detached tio per attached Pico
uv run scripts/tio_serial.py status   # log size, PIDs, last line per board
uv run scripts/tio_serial.py stop
uv run scripts/tio_serial.py list     # devices, marked logged or ignored
```

`start` finds the Pico boards by USB name, skips any port another process already
has open, and runs one `tio` per board appending to `logs/ascend.log`. tio
reconnects on its own and the wrapper restarts it if it exits, so a gap shows up
in `status` instead of silently ending the log.

Output is appended to `logs/ascend.log`, one timestamped line each:

```sh
tail -f logs/ascend.log
tail -f logs/ascend.log | grep --line-buffered -a ' PNC '
grep -a 'STATUS' logs/ascend.log
```

`logs/` is gitignored. Payload packets are binary, so use `grep -a` on them.

For a supervised soak, use the newly identified ports and a new output path:

```sh
uv run scripts/monitor_stack.py --pnc /dev/cu.usbmodem101 \
  --payload /dev/cu.usbmodem1101 --output soak.jsonl
```

This records status lines for 60 minutes by default. It does not certify SD
persistence or pass/fail; inspect the records and decode both cards afterward.
