import struct
import unittest

from parse_payload import HEADER, SYNC, UTC_HEADER, decode_packet, fmt_packet


def make_packet(utc: bool, utc_flags: int = 0x03) -> bytes:
    mask = (1 << 4) | (1 << 3)
    sensor_data = struct.pack("<f", 23.5)
    header_len = UTC_HEADER.size if utc else HEADER.size
    packet_len = header_len + len(sensor_data) + 1

    if utc:
        header = UTC_HEADER.pack(
            SYNC,
            mask,
            packet_len,
            12_345,
            1_800_000_123,
            456,
            1,
            utc_flags,
        )
    else:
        header = HEADER.pack(SYNC, mask, packet_len, 12_345)

    body = header + sensor_data
    return body + bytes(((-sum(body)) & 0xFF,))


class DecodePayloadPacketTests(unittest.TestCase):
    def test_decodes_utc_header_and_sensor_data(self) -> None:
        packet = decode_packet(make_packet(utc=True))

        self.assertEqual(packet["uptime_ms"], 12_345)
        self.assertEqual(packet["utc_seconds"], 1_800_000_123)
        self.assertEqual(packet["utc_milliseconds"], 456)
        self.assertEqual(packet["utc_source"], 1)
        self.assertTrue(packet["utc_valid"])
        self.assertAlmostEqual(
            packet["sensors"]["PicoTemp"]["pico_temp_c"], 23.5
        )
        self.assertIn("utc=", fmt_packet(packet))

    def test_still_decodes_legacy_packet(self) -> None:
        packet = decode_packet(make_packet(utc=False))

        self.assertIsNone(packet["utc_seconds"])
        self.assertFalse(packet["utc_valid"])
        self.assertAlmostEqual(
            packet["sensors"]["PicoTemp"]["pico_temp_c"], 23.5
        )

    def test_marks_unsynchronized_utc_invalid(self) -> None:
        packet = decode_packet(make_packet(utc=True, utc_flags=0))

        self.assertFalse(packet["utc_valid"])
        self.assertIn("utc=unsynced", fmt_packet(packet))


if __name__ == "__main__":
    unittest.main()
