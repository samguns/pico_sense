#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Host checks for PCM buffering and the actual PIO instruction stream."""
import collections
import ctypes
import pathlib
import random
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class AudioTests(unittest.TestCase):
    def test_pcm_ring_wrap_overflow_and_underflow(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "ring.c"
            library = pathlib.Path(directory) / "ring.so"
            source.write_text('''
#include "pcm_ring.h"
static struct pcm_ring ring;
void reset(unsigned int channels) {
    ring = (struct pcm_ring){ .channels = channels };
}
void push(const int16_t *src, size_t n) { pcm_ring_push(&ring, src, n); }
size_t pull(int16_t *dst, size_t n) { return pcm_ring_pull(&ring, dst, n); }
size_t count(void) { return ring.count; }
''')
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-shared", "-fPIC", "-I", str(ROOT / "src"),
                            str(source), "-o", str(library)], check=True)
            ring = ctypes.CDLL(str(library))
            pointer = ctypes.POINTER(ctypes.c_int16)
            ring.reset.argtypes = [ctypes.c_uint]
            ring.push.argtypes = [pointer, ctypes.c_size_t]
            ring.pull.argtypes = [pointer, ctypes.c_size_t]
            ring.pull.restype = ctypes.c_size_t
            ring.count.restype = ctypes.c_size_t
            rng = random.Random(98357)
            for channels in (1, 2):
                ring.reset(channels)
                expected = collections.deque(maxlen=384)
                for _ in range(1000):
                    n = rng.randrange(901)
                    if rng.choice((True, False)):
                        samples = [rng.randrange(-32768, 32768) for _ in range(n * channels)]
                        data = (ctypes.c_int16 * len(samples))(*samples)
                        ring.push(data, n)
                        expected.extend(tuple(samples[i:i + channels])
                                        for i in range(0, len(samples), channels))
                    else:
                        data = (ctypes.c_int16 * (n * channels + 1))()
                        data[n * channels] = 12345
                        want = [expected.popleft() for _ in range(min(n, len(expected)))]
                        got = ring.pull(data, n)
                        self.assertEqual(got, len(want))
                        self.assertEqual(list(data[:got * channels]),
                                         [sample for frame in want for sample in frame])
                        self.assertEqual(data[n * channels], 12345)
                    self.assertEqual(ring.count(), len(expected))

    def test_i2s_bit_order_delay_and_clock_period(self):
        instructions = [int(value, 16) for value in re.findall(
            r"^\s*(0x[0-9a-f]+),", (ROOT / "src/i2s_program.h").read_text(), re.M)]
        words = [0x80017FFE, 0xFFFF0000, 0x1234ABCD, 0x00000000]
        expected = [(word >> bit) & 1 for word in words for bit in range(31, -1, -1)]
        pc, x, bclk, data, shifted, cycle = 7, 0, 1, 0, 0, 0
        edges = []
        while len(edges) < len(expected):
            instruction = instructions[pc]
            side = (instruction >> 11) & 3
            op = instruction >> 13
            following = (pc + 1) % len(instructions)
            if op == 3:  # OUT pins, 1 (autopull, shift left)
                self.assertEqual(instruction & 0xff, 1)
                data = expected[shifted]
                shifted += 1
            elif op == 0:  # JMP X--
                self.assertEqual((instruction >> 5) & 7, 2)
                if x != 0:
                    following = instruction & 31
                x = (x - 1) & 0xffffffff
            elif op == 7:  # SET X
                self.assertEqual((instruction >> 5) & 7, 1)
                x = instruction & 31
            else:
                self.fail(f"Unexpected PIO opcode {op}")
            if not bclk and side & 1:
                edges.append((data, side >> 1, cycle))
            bclk = side & 1
            pc = following
            cycle += 1
            self.assertLess(cycle, 1000)
        self.assertEqual([edge[0] for edge in edges], expected)
        # Word select changes while the PREVIOUS sample's last bit is sent.
        self.assertEqual([edge[1] for edge in edges],
                         ([0] * 15 + [1] * 16 + [0]) * len(words))
        self.assertTrue(all(b[2] - a[2] == 2 for a, b in zip(edges, edges[1:])))


if __name__ == "__main__":
    unittest.main()
