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

    def test_payload_science_board(self):
        # 11 slots: Pico, IMU, RTC, TMP, BME, ENS, SHT, UV, BMP390, ozone,
        # TMP117Sci. The Science board fills the old reserved BMP390 slot and
        # appends its TMP117; every existing mask bit shifts up by one.
        mask = (1 << 11) | (1 << 10) | (1 << 3) | (1 << 2) | (1 << 0)
        data = (
            struct.pack("<f", 23.5)
            + struct.pack("<fff", 1.0, 2.0, 3.0)  # AS7331
            + struct.pack("<ddf", 22.5, 97311.0, 102.0)  # science BMP390
            + struct.pack("<f", 22.75)  # science TMP117
        )
        packet = payload.decode_packet(payload_packet(mask, data))
        sensors = packet["sensors"]
        self.assertEqual(sensors["AS7331"]["uvb_uw_cm2"], 2.0)
        self.assertEqual(sensors["BMP390"]["bmp_pressure_pa"], 97311.0)
        self.assertEqual(sensors["BMP390"]["bmp_alt_m"], 102.0)
        self.assertEqual(sensors["TMP117Sci"]["temp_c"], 22.75)
        # The pre-science build must still decode its own 10-slot packets.
        old_mask = (1 << 10) | (1 << 9) | (1 << 2) | 1
        old = payload.decode_packet(
            payload_packet(
                old_mask,
                struct.pack("<ffffh", 23.5, 1.25, 2.5, 3.75, 20),
            )
        )
        self.assertEqual(old["sensors"]["Ozone"]["ppb"], 20)

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

    def ambiguous_science_packet(self):
        # Bits 4-7 are length-coincident across the geiger and science
        # builds: Geiger/TMP/BME/ENS vs TMP/BME/ENS/SHT all sum to 33 bytes.
        return payload_packet(
            (1 << 11) | 0xF0,
            struct.pack("<f", 24.0)
            + struct.pack("<fIfI", 25.0, 97000, 35.0, 1000)
            + struct.pack("<BHH", 1, 32, 418)
            + struct.pack("<ff", 24.5, 44.0),
        )

    def science_exclusive_packet(self):
        # BMP390+ozone+TMP117Sci fits no geiger-build byte count.
        return payload_packet(
            (1 << 11) | (1 << 2) | (1 << 1) | (1 << 0),
            struct.pack("<ddf", 22.5, 97311.0, 102.0)
            + struct.pack("<h", 20)
            + struct.pack("<f", 22.75),
        )

    def test_ambiguous_build_resolved_per_file(self):
        ambiguous = self.ambiguous_science_packet()
        with self.assertRaisesRegex(ValueError, "ambiguous sensor layout"):
            payload.decode_packet(ambiguous)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            path.write_bytes(self.science_exclusive_packet() + ambiguous)
            packets, skipped = payload.decode_file(path)
        self.assertEqual(skipped, 0)
        self.assertEqual(len(packets), 2)
        sensors = packets[1]["sensors"]
        self.assertEqual(sensors["TMP117"]["temp_c"], 24.0)
        self.assertEqual(sensors["BME688"]["gas_ohm"], 1000)
        self.assertEqual(sensors["SHTC3"]["humidity_pct"], 44.0)
        self.assertNotIn("Geiger", sensors)

    def test_ambiguous_without_evidence_stays_skipped(self):
        ambiguous = self.ambiguous_science_packet()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            path.write_bytes(ambiguous)
            packets, skipped = payload.decode_file(path)
        self.assertEqual(packets, [])
        self.assertEqual(skipped, len(ambiguous))

    def test_split_build_evidence_is_not_guessed(self):
        # One packet only the geiger build explains (bit 3: SHTC3's 8 bytes
        # vs AS7331's 12), one only the science build explains.
        geiger_only = payload_packet(
            (1 << 11) | (1 << 3), struct.pack("<ff", 24.5, 44.0)
        )
        ambiguous = self.ambiguous_science_packet()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            path.write_bytes(geiger_only + self.science_exclusive_packet() + ambiguous)
            packets, skipped = payload.decode_file(path)
        self.assertEqual(len(packets), 2)
        self.assertEqual(skipped, len(ambiguous))
        self.assertEqual(packets[0]["sensors"]["SHTC3"]["humidity_pct"], 44.0)
        self.assertEqual(packets[1]["sensors"]["BMP390"]["bmp_alt_m"], 102.0)

    def test_pnc_captured_packet_and_resync(self):
        fixture = Path(__file__).parent / "raw_data/PAC/data10.bin"
        packet = fixture.read_bytes()[:pnc.PACKET_LEN]
        self.assertTrue(pnc.valid(packet))
        self.assertFalse(pnc.valid(packet[:-1] + bytes([packet[-1] ^ 1])))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.bin"
            path.write_bytes(b"noise" + packet + b"tail")
            packets, plog_records, skipped = pnc.decode_file(path)
        self.assertEqual(len(packets), 1)
        self.assertEqual(plog_records, [])
        self.assertEqual(skipped, 9)

    def pnc_plog_capture(self, directory: str) -> Path:
        """data.bin with PnC packets and payload records interleaved."""
        fixture = Path(__file__).parent / "raw_data/PAC/data10.bin"
        pnc_packet = fixture.read_bytes()[:pnc.PACKET_LEN]
        # Sentinel-11 science build: PicoTemp (bit 10), TMP117 (bit 7),
        # TMP117Sci (bit 0).
        mask = (1 << 11) | (1 << 10) | (1 << 7) | (1 << 0)
        pkt = payload_packet(
            mask,
            struct.pack("<f", 23.5)
            + struct.pack("<f", 24.0)
            + struct.pack("<f", 22.75),
            uptime=777,
        )
        record = (
            plog.HEADER.pack(plog.MAGIC, 1700000000, 321, 1, len(pkt)) + pkt
        )
        noise = b"\x00garbage\x00"
        path = Path(directory) / "capture.bin"
        path.write_bytes(
            pnc_packet + record + noise + pnc_packet + pnc_packet
        )
        return path

    def test_pnc_file_with_interleaved_payload_records(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.pnc_plog_capture(directory)
            packets, plog_records, skipped = pnc.decode_file(path)
        self.assertEqual(len(packets), 3)
        self.assertEqual(len(plog_records), 1)
        self.assertEqual(skipped, len(b"\x00garbage\x00"))
        unix_s, uptime_ms, node, data = plog_records[0]
        self.assertEqual((unix_s, uptime_ms, node), (1700000000, 321, 1))
        sensors = payload.decode_packet(data)["sensors"]
        self.assertEqual(sensors["PicoTemp"]["pico_temp_c"], 23.5)
        self.assertEqual(sensors["TMP117"]["temp_c"], 24.0)
        self.assertEqual(sensors["TMP117Sci"]["temp_c"], 22.75)

    def test_pnc_payload_mode_decodes_interleaved_records(self):
        directory = Path(__file__).parent
        with tempfile.TemporaryDirectory() as temp:
            path = self.pnc_plog_capture(temp)
            result = subprocess.run(
                [sys.executable, str(directory / "parse_pnc.py"),
                 str(path), "--payload"],
                capture_output=True, text=True, check=True,
            )
        self.assertIn("node=1", result.stdout)
        self.assertIn("pico=23.50C", result.stdout)
        self.assertIn("tmp117_sci=22.75C", result.stdout)
        # Summary mode counts both record types.
        with tempfile.TemporaryDirectory() as temp:
            path = self.pnc_plog_capture(temp)
            result = subprocess.run(
                [sys.executable, str(directory / "parse_pnc.py"),
                 str(path), "--summary"],
                capture_output=True, text=True, check=True,
            )
        self.assertIn("3 packets, 1 payload records", result.stdout)

    def test_pnc_payload_mode_reports_bad_records(self):
        directory = Path(__file__).parent
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "capture.bin"
            bogus = plog.HEADER.pack(plog.MAGIC, 0, 10, 1, 4) + b"\x01\x02\x03\x04"
            path.write_bytes(bogus)
            result = subprocess.run(
                [sys.executable, str(directory / "parse_pnc.py"),
                 str(path), "--payload", "--summary"],
                capture_output=True, text=True, check=True,
            )
        self.assertIn("0 packets, 1 payload records", result.stdout)
        self.assertIn("1 bad payload packets", result.stdout)

    def test_pnc_csv_mode_notes_interleaved_records(self):
        directory = Path(__file__).parent
        with tempfile.TemporaryDirectory() as temp:
            path = self.pnc_plog_capture(temp)
            result = subprocess.run(
                [sys.executable, str(directory / "parse_pnc.py"), str(path), "--csv"],
                capture_output=True, text=True, check=True,
            )
        self.assertEqual(len(result.stdout.strip().splitlines()), 3)
        self.assertIn("1 interleaved payload records", result.stderr)

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
