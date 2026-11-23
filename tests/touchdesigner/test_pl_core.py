"""Tests for touchdesigner/PixelLinkPreviz/pl_core.py, the Python receiver.

Run from the repo root:  python3 -m unittest discover tests/touchdesigner
Needs numpy. No TouchDesigner required.
"""

import importlib
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "touchdesigner", "PixelLinkPreviz"))
import pl_core  # noqa: E402


def pixels(frame, channel, rgb, offset=0, push=False, flags=0):
    count = len(rgb) // 3
    return (b"PX\x01\x01" + struct.pack("<HBBHH", frame, channel, flags | (1 if push else 0), offset, count)
            + bytes(rgb))


def sync(frame):
    return b"PX\x01\x02" + struct.pack("<HH", frame, 0)


class PlCoreTest(unittest.TestCase):
    def setUp(self):
        importlib.reload(pl_core)
        self.core = pl_core

    def test_push_shows_frame(self):
        self.core.handle(pixels(1, 0, [10, 20, 30]))
        self.assertEqual(self.core.stats["shown"], 0)
        self.core.handle(pixels(1, 1, [40, 50, 60], push=True))
        self.assertEqual(self.core.stats["shown"], 1)
        self.assertEqual(list(self.core.front[0, 0]), [10, 20, 30])
        self.assertEqual(list(self.core.front[1, 0]), [40, 50, 60])

    def test_sync_shows_frame(self):
        self.core.handle(pixels(5, 3, [1, 2, 3]))
        self.core.handle(sync(4))  # older SYNC does nothing
        self.assertEqual(self.core.stats["shown"], 0)
        self.core.handle(sync(5))
        self.assertEqual(self.core.stats["shown"], 1)
        self.assertEqual(list(self.core.front[3, 0]), [1, 2, 3])

    def test_implicit_frame(self):
        self.core.handle(pixels(1, 0, [9, 9, 9]))
        self.core.handle(pixels(2, 0, [8, 8, 8]))
        self.assertEqual(self.core.stats["implicit"], 1)
        self.assertEqual(list(self.core.front[0, 0]), [9, 9, 9])

    def test_offset_and_late_packets(self):
        self.core.handle(pixels(100, 2, [7, 7, 7], offset=500, push=True))
        self.assertEqual(list(self.core.front[2, 500]), [7, 7, 7])
        shown = self.core.stats["shown"]
        self.core.handle(pixels(97, 2, [1, 1, 1], push=True))  # late: dropped
        self.assertEqual(self.core.stats["shown"], shown)
        self.core.handle(pixels(65535 & (100 - 1000), 2, [2, 2, 2], push=True))  # restart: accepted
        self.assertEqual(list(self.core.front[2, 0]), [2, 2, 2])

    def test_frame_wraps(self):
        self.core.handle(pixels(65535, 0, [1, 1, 1], push=True))
        self.core.handle(pixels(0, 0, [2, 2, 2], push=True))
        self.assertEqual(list(self.core.front[0, 0]), [2, 2, 2])

    def test_bad_packets(self):
        bad = self.core.stats["bad"]
        self.core.handle(b"XX\x01\x01")                        # wrong magic
        self.core.handle(b"PX\x01\x01" + b"\0" * 4)            # short PIXELS
        self.core.handle(pixels(1, 32, [1, 2, 3]))             # channel out of range
        self.core.handle(pixels(1, 0, [1, 2, 3], flags=0x02))  # RGBW: not implemented
        self.core.handle(b"PX\x01\x09")                        # unknown type
        self.assertEqual(self.core.stats["bad"], bad + 5)

    def test_discover_gets_info(self):
        replies = []
        self.core.handle(b"PX\x01\x03", replies.append)
        self.assertEqual(len(replies), 1)
        info = replies[0]
        self.assertEqual(len(info), 34)
        self.assertEqual(info[:4], b"PX\x01\x04")
        self.assertEqual(info[14], 32)
        self.assertEqual(struct.unpack_from("<H", info, 16)[0], 972)
        self.assertEqual(info[18:34].rstrip(b"\0"), b"previz")

    def test_status_layout(self):
        replies = []
        self.core._last_status -= 2  # due now
        self.core.handle(pixels(1, 0, [1, 2, 3], push=True), replies.append)
        self.assertEqual(len(replies), 1)
        st = replies[0]
        self.assertEqual(len(st), 32)
        self.assertEqual(st[:4], b"PX\x01\x05")
        shown, implicit, bad, packets = struct.unpack_from("<IIII", st, 16)
        self.assertEqual((shown, packets), (1, 1))


if __name__ == "__main__":
    unittest.main()
