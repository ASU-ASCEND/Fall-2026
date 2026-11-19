"""Offline tests of bench-soak status parsing."""
import unittest

from monitor_stack import STATUS_RE, parse_status


class MonitorTests(unittest.TestCase):
    def test_pnc_status(self):
        line = b"STATUS sd=1 file=data0004.bin bytes=123 queue_drop=0"
        board, fields = parse_status(line)
        self.assertEqual(board, "pnc")
        self.assertEqual(fields["bytes"], "123")
        self.assertEqual(fields["queue_drop"], "0")

    def test_payload_status_embedded_in_binary(self):
        raw = b"\xff\x00STATUS sensors=7/8 sd=1 bytes=99 sd_drop=0\r\n"
        match = STATUS_RE.search(raw)
        self.assertIsNotNone(match)
        board, fields = parse_status(match.group())
        self.assertEqual(board, "payload")
        self.assertEqual(fields["sensors"], "7/8")
        self.assertEqual(fields["sd_drop"], "0")

    def test_unrelated_status_is_not_identified(self):
        self.assertIsNone(STATUS_RE.search(b"STATUS unrelated=true\n"))


if __name__ == "__main__":
    unittest.main()
