#!/usr/bin/env python3
"""Send battery voltage samples to Pico Sense over USB CDC (requires pyserial)."""
import argparse
from decimal import Decimal, InvalidOperation
import math
import sys
import time


def voltage(text):
    try:
        value = Decimal(text)
        if not value.is_finite() or not 0 <= value <= Decimal('99.99'):
            raise ValueError
        if value != value.quantize(Decimal('0.01')):
            raise ValueError
    except (InvalidOperation, ValueError):
        raise argparse.ArgumentTypeError('voltage must be 0..99.99 with at most two decimal places')
    return format(abs(value), '.2f')


def send_voltage(port, value):
    port.write(f'voltage {value}\n'.encode('ascii'))
    port.flush()
    expected = f'OK voltage {value}'
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        line = port.readline().decode('ascii', errors='replace').strip()
        if line == expected:
            print(f'Display: {value} V', flush=True)
            return
        if line.startswith('ERR '):
            raise RuntimeError(line)
    raise RuntimeError(f'No display acknowledgement for {value} V; check firmware and close other serial terminals')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True, help='CDC port, e.g. /dev/ttyACM0 or COM5')
    parser.add_argument('--voltage', type=voltage, help='send one value and exit; otherwise play a short demo')
    parser.add_argument('--interval', type=float, default=0.5, help='demo interval in seconds (default: 0.5)')
    args = parser.parse_args()
    if not math.isfinite(args.interval) or args.interval < 0.25:
        parser.error('--interval must be finite and at least 0.25 seconds')
    try:
        import serial
    except ImportError:
        parser.exit(1, 'Install pyserial first: python -m pip install pyserial\n')
    try:
        with serial.Serial(args.port, 115200, timeout=0.2, write_timeout=2) as port:
            time.sleep(1)  # Allow CDC/DTR connection to settle.
            port.reset_input_buffer()
            samples = [args.voltage] if args.voltage is not None else [
                '12.64', '12.61', '12.58', '12.52', '12.41',
                '12.48', '12.55', '12.62', '12.68', '12.64',
            ]
            for index, sample in enumerate(samples):
                if index:
                    time.sleep(args.interval)
                send_voltage(port, sample)
    except (serial.SerialException, OSError, RuntimeError) as exc:
        print(f'Error: {exc}', file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == '__main__':
    sys.exit(main())
