"""Offline regression tests; no hardware or external dependencies required."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import parse_payload as payload
import parse_payload_log as plog
import parse_pnc as pnc


def payload_packet(mask, data=b"", uptime=123):
    body = payload.HEADER.pack(payload.SYNC, mask, 15 + len(data), uptime) + data
    return body + bytes([-sum(body) & 255])


class DecoderTests(unittest.TestCase):
    def test_payload_partial_and_checksum(self):
        packet = payload_packet(0b1100, struct.pack("<f", 23.5))
        decoded = payload.decode_packet(packet)
        self.assertEqual(decoded["uptime_ms"], 123)
        self.assertEqual(decoded["sensors"]["PicoTemp"]["pico_temp_c"], 23.5)
        with self.assertRaisesRegex(ValueError, "checksum"):
            payload.decode_packet(packet[:-1] + bytes([packet[-1] ^ 1]))

    def test_payload_reserved_rtc_slot(self):
        # Seven mask slots remain for compatibility, with the RTC bit clear.
        packet = payload_packet(
            0b11101111,
            struct.pack("<f", 23.5) + bytes(40) + struct.pack("<f", 24.0)
            + struct.pack("<fIfI", 25.0, 97000, 35.0, 1000)
            + struct.pack("<BHH", 1, 32, 418)
            + struct.pack("<ff", 24.5, 44.0),
        )
        sensors = payload.decode_packet(packet)["sensors"]
        self.assertNotIn("PCF8523", sensors)
        self.assertEqual(sensors["TMP117"]["temp_c"], 24.0)
        self.assertEqual(sensors["SHTC3"]["humidity_pct"], 44.0)

    def test_payload_uv_and_ozone(self):
        # 10 slots: Pico, IMU, RTC, TMP, BME, ENS, SHT, UV, BMP, ozone.
        mask = (1 << 10) | (1 << 9) | (1 << 2) | 1
        data = struct.pack("<ffffh", 23.5, 1.25, 2.5, 3.75, 20)
        packet = payload.decode_packet(payload_packet(mask, data))
        self.assertEqual(packet["sensors"]["AS7331"]["uvb_uw_cm2"], 2.5)
        self.assertEqual(packet["sensors"]["Ozone"]["ppb"], 20)
        self.assertNotIn("PCF8523", packet["sensors"])
        self.assertNotIn("ICM20948", packet["sensors"])

    def test_payload_reserved_bmp_rejected(self):
        with self.assertRaisesRegex(ValueError, "reserved sensor"):
            payload.decode_packet(payload_packet((1 << 10) | (1 << 1)))

    def test_payload_resync_and_trailing_bytes(self):
        packet = payload_packet(0b1000)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            path.write_bytes(b"noise" + packet + b"ASU")
            packets, skipped = payload.decode_file(path)
        self.assertEqual(len(packets), 1)
        self.assertEqual(skipped, 8)

    def test_ambiguous_layout_is_not_guessed(self):
        packet = payload_packet((1 << 8) | 30, bytes(33))
        with self.assertRaisesRegex(ValueError, "ambiguous sensor layout"):
            payload.decode_packet(packet)

    def test_pnc_captured_packet_and_resync(self):
        fixture = Path(__file__).parent / "raw_data/PAC/data10.bin"
        packet = fixture.read_bytes()[:pnc.PACKET_LEN]
        self.assertTrue(pnc.valid(packet))
        self.assertFalse(pnc.valid(packet[:-1] + bytes([packet[-1] ^ 1])))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            path.write_bytes(b"noise" + packet + b"tail")
            packets, skipped = pnc.decode_file(path)
        self.assertEqual(len(packets), 1)
        self.assertEqual(skipped, 9)

    def test_plog_recovers_after_truncated_candidate(self):
        packet = payload_packet(0b1000)
        record = plog.HEADER.pack(plog.MAGIC, 1700000000, 321, 1, len(packet)) + packet
        truncated = plog.HEADER.pack(plog.MAGIC, 0, 0, 1, 255)
        diagnostics = {"truncated_candidates": 0, "record_bytes": 0}
        result = list(plog.records(truncated + record + b"tail", diagnostics))
        self.assertEqual(result, [(1700000000, 321, 1, packet)])
        self.assertEqual(diagnostics["truncated_candidates"], 1)
        self.assertEqual(diagnostics["unparsed_bytes"], len(truncated) + 4)

    def test_unknown_historical_payload_layout_is_not_guessed(self):
        fixture = Path(__file__).parent / "raw_data/Payload/RAWDATA0.BIN"
        raw = fixture.read_bytes()
        length = struct.unpack_from("<H", raw, 8)[0]
        with self.assertRaisesRegex(ValueError, "unknown sensor count 12"):
            payload.decode_packet(raw[:length])

    def test_missing_input_is_a_cli_failure(self):
        directory = Path(__file__).parent
        with tempfile.TemporaryDirectory() as temp:
            missing = str(Path(temp) / "missing.bin")
            for script in ("parse_payload.py", "parse_pnc.py"):
                with self.subTest(script=script):
                    result = subprocess.run(
                        [sys.executable, str(directory / script), missing, "--summary"],
                        capture_output=True, text=True, check=False,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("missing:", result.stderr)


if __name__ == "__main__":
    unittest.main()
