#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyobjc-framework-CoreWLAN"]
# ///
"""Log the presence of the checkout AP from the Mac, without joining it.

Scans every SCAN_SECONDS and appends "<time> present|GONE rssi" lines to the
log file so the AP shutdown time can be bracketed from outside the ESP.

    uv run scripts/ap_scan_logger.py logs/ap-scan.log
"""

import sys
import time
from datetime import datetime
from pathlib import Path

import CoreWLAN

SSID = "STRATODEVILS-CHECKOUT"
SCAN_SECONDS = 8


def main() -> int:
    out = Path(sys.argv[1])
    out.parent.mkdir(parents=True, exist_ok=True)
    iface = CoreWLAN.CWWiFiClient.sharedWiFiClient().interface()
    while True:
        line = f"{time.strftime('%H:%M:%S')} GONE"
        try:
            nets, err = iface.scanForNetworksWithName_error_(SSID, None)
            for group in nets or ():
                for n in group if not hasattr(group, "rssiValue") else (group,):
                    if hasattr(n, "rssiValue"):
                        line = f"{time.strftime('%H:%M:%S')} present RSSI {n.rssiValue()}"
                        break
        except Exception as exc:  # scan hiccup: record it, keep going
            line = f"{time.strftime('%H:%M:%S')} scan-error {exc}"
        with open(out, "a") as handle:
            handle.write(line + "\n")
        print(line, flush=True)
        time.sleep(SCAN_SECONDS)


if __name__ == "__main__":
    sys.exit(main())
