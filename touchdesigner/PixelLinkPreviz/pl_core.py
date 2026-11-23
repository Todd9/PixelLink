# pl_core — Pixel Link receiver logic for the TouchDesigner previz
#
# Paste into a Text DAT named "pl_core". Other DATs reach it with
# op('pl_core').module. It behaves like a Pixel Link receiver: it assembles
# PIXELS packets into a back buffer, shows a frame on SYNC / PUSH / a newer
# frame number (same rules as the Teensy), answers DISCOVER with INFO, and
# sends STATUS back to the sender once a second so the master's status line
# works.
#
# Plain Python + numpy; no TouchDesigner calls, so it can be tested outside TD.

import struct
import time
import numpy as np

NODE_NAME = "previz"
FW_VERSION = (1, 0, 0)
MAX_CHANNELS = 32
MAX_PIXELS = 972          # per channel, same as the receiver
STATUS_INTERVAL = 1.0     # seconds

MAGIC = b"PX\x01"
T_PIXELS, T_SYNC, T_DISCOVER, T_INFO, T_STATUS = 1, 2, 3, 4, 5
FLAG_PUSH = 0x01

back = np.zeros((MAX_CHANNELS, MAX_PIXELS, 3), np.uint8)
front = np.zeros_like(back)

stats = {
    "packets": 0, "bad": 0, "shown": 0, "implicit": 0,
    "fps": 0.0, "active": 0,
}
_frame = None             # current frame number
_dirty = False            # data written since the last show
_seen_channels = set()    # channels with data since the last STATUS
_fps_count = 0
_last_status = time.monotonic()
_start = time.monotonic()


def _frame_diff(a, b):
    """Signed difference a - b with 16-bit wraparound (> 0 means a is newer)."""
    return ((a - b + 0x8000) & 0xFFFF) - 0x8000


def _show():
    global _dirty, _fps_count
    front[:] = back
    _dirty = False
    _fps_count += 1
    stats["shown"] += 1


def handle(data, reply=None):
    """Processes one UDP payload. reply(bytes) sends a packet back to its sender."""
    global _frame, _dirty
    if len(data) < 4 or data[:3] != MAGIC:
        stats["bad"] += 1
        return
    stats["packets"] += 1
    kind = data[3]

    if kind == T_PIXELS:
        if len(data) < 12:
            stats["bad"] += 1
            return
        frame, ch, flags, offset, count = struct.unpack_from("<HBBHH", data, 4)
        if (ch >= MAX_CHANNELS or offset + count > MAX_PIXELS
                or len(data) < 12 + count * 3 or (flags & 0x06) != 0):
            stats["bad"] += 1
            return
        if _frame is None:
            _frame = frame
        d = _frame_diff(frame, _frame)
        if -60 < d < 0:
            return                      # late packet from an older frame
        if d != 0:
            if _dirty:                  # newer frame without PUSH/SYNC: show the old one
                _show()
                stats["implicit"] += 1
            _frame = frame
        if count:
            px = np.frombuffer(data, np.uint8, count * 3, 12).reshape(count, 3)
            back[ch, offset:offset + count] = px
            _dirty = True
            _seen_channels.add(ch)
        if (flags & FLAG_PUSH) and _dirty:
            _show()

    elif kind == T_SYNC:
        if len(data) < 8:
            stats["bad"] += 1
            return
        (frame,) = struct.unpack_from("<H", data, 4)
        if _dirty and _frame is not None and _frame_diff(frame, _frame) >= 0:
            _show()

    elif kind == T_DISCOVER:
        if reply:
            reply(info_packet())
        return

    else:
        stats["bad"] += 1
        return

    if reply:
        _maybe_send_status(reply)


def info_packet():
    name = NODE_NAME.encode("ascii")[:16].ljust(16, b"\0")
    return (b"PX\x01" + bytes([T_INFO]) + bytes(FW_VERSION) + b"\0"
            + b"\x02\x00\x00\x00\x00\x01"          # MAC: locally administered, made up
            + bytes([MAX_CHANNELS, 0]) + struct.pack("<H", MAX_PIXELS) + name)


def _maybe_send_status(reply):
    global _last_status, _fps_count
    now = time.monotonic()
    elapsed = now - _last_status
    if elapsed < STATUS_INTERVAL:
        return
    stats["fps"] = _fps_count / elapsed
    stats["active"] = len(_seen_channels)
    out_len = 0
    for ch in _seen_channels:
        out_len = max(out_len, _channel_length(ch))
    reply(b"PX\x01" + bytes([T_STATUS]) + struct.pack(
        "<IHHBBhIIII",
        int(now - _start), int(stats["fps"] * 10), out_len, stats["active"], 0,
        400,  # temperature field: reports 40.0 C
        stats["shown"], stats["implicit"], stats["bad"], stats["packets"]))
    _fps_count = 0
    _seen_channels.clear()
    _last_status = now


_lengths = {}


def _channel_length(ch):
    return _lengths.get(ch, 0)


# ---- Layout --------------------------------------------------------------

class Layout:
    """Pixel positions and their channel/pixel addresses, from layout.csv."""

    def __init__(self, rows):
        # rows: [channel, pixel, feature, x, y, z] as strings (header removed)
        self.channel = np.array([int(r[0]) for r in rows], np.int32)
        self.pixel = np.array([int(r[1]) for r in rows], np.int32)
        self.feature = [r[2] for r in rows]
        xyz = np.array([[float(r[3]), float(r[4]), float(r[5])] for r in rows], np.float32)
        self.x, self.y, self.z = xyz[:, 0], xyz[:, 1], xyz[:, 2]
        # LEDs that aren't sent over the network (channel -1) stay dark.
        self.networked = self.channel >= 0
        self.ch_safe = np.where(self.networked, self.channel, 0)
        self.px_safe = np.clip(self.pixel, 0, MAX_PIXELS - 1)
        _lengths.clear()
        for c, p in zip(self.channel, self.pixel):
            if c >= 0:
                _lengths[int(c)] = max(_lengths.get(int(c), 0), int(p) + 1)

    def __len__(self):
        return len(self.channel)

    def colors(self):
        """Current colors per layout row, as floats 0-1, shape (n, 3)."""
        c = front[self.ch_safe, self.px_safe].astype(np.float32) / 255.0
        c[~self.networked] = 0.0
        return c


_layout = None
_layout_key = None


def layout_from_table(table):
    """Builds (and caches) the Layout from a Table DAT holding layout.csv."""
    global _layout, _layout_key
    n = table.numRows
    # Cheap change check: row count plus the first and last rows' text.
    key = (n, str(table.row(1)) if n > 1 else "", str(table.row(n - 1)) if n > 1 else "")
    if _layout is None or key != _layout_key:
        rows = [[c.val for c in row] for row in table.rows()[1:]]
        _layout = Layout(rows)
        _layout_key = key
    return _layout
