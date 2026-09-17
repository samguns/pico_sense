"""Host protocol tests; no attached board or pyserial required."""
import argparse
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('voltage_demo', Path(__file__).parents[1] / 'scripts/voltage_demo.py')
demo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(demo)

class FakePort:
    def __init__(self, lines):
        self.lines = iter(lines)
        self.sent = b''
    def write(self, data):
        self.sent += data
    def flush(self):
        pass
    def readline(self):
        return next(self.lines, b'')

class ProtocolTests(unittest.TestCase):
    def test_values(self):
        for text, expected in [('12.64', '12.64'), ('1.2', '1.20'), ('-0', '0.00')]:
            self.assertEqual(demo.voltage(text), expected)
        for text in ['nan', 'inf', '-1', '100', '12.345']:
            with self.assertRaises(argparse.ArgumentTypeError):
                demo.voltage(text)
    def test_echo_logs_and_ack(self):
        port = FakePort([b'voltage 12.64\r\n', b'demo: touch ready\n', b'OK voltage 12.64\r\n'])
        demo.send_voltage(port, '12.64')
        self.assertEqual(port.sent, b'voltage 12.64\n')
    def test_display_error(self):
        with self.assertRaisesRegex(RuntimeError, 'ERR display'):
            demo.send_voltage(FakePort([b'ERR display -5\n']), '12.64')
    def test_timeout(self):
        with patch.object(demo.time, 'monotonic', side_effect=[0, 0, 6]):
            with self.assertRaisesRegex(RuntimeError, 'No display acknowledgement'):
                demo.send_voltage(FakePort([b'OK voltage 12.00\n']), '12.64')

if __name__ == '__main__':
    unittest.main()
