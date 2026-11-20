"""Regression tests for passive serial identification."""
import struct
import unittest

from identify_boards import classify, valid_payload_packet, valid_pnc_packet


class IdentifyBoardsTests(unittest.TestCase):
    def test_pnc_text_and_payload_status(self):
        self.assertEqual(classify(b"PKT,123,0,87,23.0\n")[0], "pnc")
        self.assertEqual(
            classify(b"DATA pnc pico=31.82C rtc=0 ... | payload none\n")[0],
            "pnc",
        )
        self.assertEqual(classify(b"STATUS sensors=7/8 sd=1 bytes=99\n")[0], "payload")

    def test_checksum_valid_payload_and_bad_checksum(self):
        body = struct.pack("<4sIHI", b"ASU!", 1 << 7, 15, 123)
        packet = body + bytes([-sum(body) & 255])
        self.assertTrue(valid_payload_packet(packet))
        self.assertEqual(classify(b"noise" + packet)[0], "payload")
        self.assertFalse(valid_payload_packet(packet[:-1] + bytes([packet[-1] ^ 1])))

    def test_checksum_valid_pnc_and_bad_checksum(self):
        packet = bytearray(87)
        packet[:4] = b"\xfe\xca\xad\xde"
        packet[9] = 87
        packet[-1] = -sum(packet) & 255
        self.assertTrue(valid_pnc_packet(packet))
        self.assertEqual(classify(bytes(packet))[0], "pnc")
        packet[-1] ^= 1
        self.assertFalse(valid_pnc_packet(packet))

    def test_unknown_and_ambiguous(self):
        self.assertEqual(classify(b"no board output")[0], "unknown")
        self.assertEqual(
            classify(b"STATUS sd=1 file=data0000.bin\nSTATUS sensors=7/8 sd=1\n")[0],
            "ambiguous",
        )


if __name__ == "__main__":
    unittest.main()
