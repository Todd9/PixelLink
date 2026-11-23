// Protocol test: every PixelLink.h builder and parser, checked against the
// byte layout in docs/spec.md.

#include <cstdio>
#include <cstring>
#include <string>
#include "PixelLink.h"

namespace pl = pixellink;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main() {
  uint8_t buf[pl::kMaxPayload];

  // Constants
  CHECK(pl::kMaxPixelsPerPacket == 486);
  CHECK(pl::kPixelsHeaderSize + pl::kMaxPixelsPerPacket * 3 <= pl::kMaxPayload);

  // Header
  pl::writeHeader(buf, pl::kSync);
  CHECK(buf[0] == 'P' && buf[1] == 'X' && buf[2] == 1 && buf[3] == 0x02);
  CHECK(pl::checkHeader(buf, 4));
  CHECK(!pl::checkHeader(buf, 3));
  buf[2] = 2;
  CHECK(!pl::checkHeader(buf, 4));  // unknown version

  // PIXELS: byte layout
  uint8_t rgb[6] = {1, 2, 3, 4, 5, 6};
  size_t n = pl::buildPixels(buf, 0x1234, 7, pl::kFlagPush, 0x0102, 2, rgb);
  CHECK(n == 12 + 6);
  CHECK(buf[3] == pl::kPixels);
  CHECK(buf[4] == 0x34 && buf[5] == 0x12);  // frame, little-endian
  CHECK(buf[6] == 7 && buf[7] == pl::kFlagPush);
  CHECK(buf[8] == 0x02 && buf[9] == 0x01);  // offset
  CHECK(buf[10] == 2 && buf[11] == 0);      // count
  CHECK(memcmp(buf + 12, rgb, 6) == 0);

  pl::Pixels px;
  CHECK(pl::checkHeader(buf, n) && pl::parsePixels(buf, n, px));
  CHECK(px.frame == 0x1234 && px.channel == 7 && px.flags == pl::kFlagPush);
  CHECK(px.offset == 0x0102 && px.count == 2 && px.data == buf + 12);
  CHECK(!pl::parsePixels(buf, n - 1, px));  // shorter than its count says
  CHECK(!pl::parsePixels(buf, 11, px));

  // PIXELS: limits
  static uint8_t big[(pl::kMaxPixelsPerPacket + 1) * 3];
  CHECK(pl::buildPixels(buf, 0, 0, 0, 0, pl::kMaxPixelsPerPacket, big) == pl::kMaxPayload - 2);
  CHECK(pl::buildPixels(buf, 0, 0, 0, 0, pl::kMaxPixelsPerPacket + 1, big) == 0);
  CHECK(pl::buildPixels(buf, 0, 0, 0, 0, 0, nullptr) == 12);

  // writePixelsHeader leaves the data alone
  memset(buf, 0xEE, sizeof(buf));
  n = pl::writePixelsHeader(buf, 1, 2, 0, 0, 10);
  CHECK(n == 12 + 30 && buf[12] == 0xEE);

  // SYNC
  n = pl::buildSync(buf, 0xBEEF);
  CHECK(n == 8 && buf[3] == pl::kSync && buf[4] == 0xEF && buf[5] == 0xBE && buf[6] == 0 && buf[7] == 0);
  uint16_t f = 0;
  CHECK(pl::parseSync(buf, n, f) && f == 0xBEEF);
  CHECK(!pl::parseSync(buf, 7, f));

  // DISCOVER
  CHECK(pl::buildDiscover(buf) == 4 && buf[3] == pl::kDiscover);

  // INFO, including a name that fills all 16 bytes with no terminator
  pl::Info in = {};
  in.fwVersion[0] = 1; in.fwVersion[1] = 1; in.fwVersion[2] = 4;
  for (int i = 0; i < 6; i++) in.mac[i] = uint8_t(0xA0 + i);
  in.maxChannels = 32;
  in.maxPixelsPerChannel = 972;
  strcpy(in.name, "sixteen-chars-ok");
  n = pl::buildInfo(buf, in);
  CHECK(n == 34 && buf[3] == pl::kInfo);
  CHECK(buf[4] == 1 && buf[6] == 4 && buf[7] == 0 && buf[8] == 0xA0 && buf[13] == 0xA5);
  CHECK(buf[14] == 32 && buf[15] == 0 && buf[16] == (972 & 0xFF) && buf[17] == (972 >> 8));
  CHECK(memcmp(buf + 18, "sixteen-chars-ok", 16) == 0);
  pl::Info out;
  CHECK(pl::parseInfo(buf, n, out));
  CHECK(std::string(out.name) == "sixteen-chars-ok" && out.maxPixelsPerChannel == 972);
  CHECK(!pl::parseInfo(buf, 33, out));
  strcpy(in.name, "remote");
  n = pl::buildInfo(buf, in);
  CHECK(buf[24] == 0 && buf[33] == 0);  // zero-padded
  CHECK(pl::parseInfo(buf, n, out) && std::string(out.name) == "remote");

  // STATUS
  pl::Status s = {};
  s.uptimeSec = 0x01020304; s.fpsX10 = 600; s.outputLength = 486; s.activeChannels = 16;
  s.state = pl::kStateHolding; s.tempX10 = -55; s.framesShown = 7; s.implicitFrames = 8;
  s.badPackets = 9; s.packetsReceived = 10;
  n = pl::buildStatus(buf, s);
  CHECK(n == 32 && buf[3] == pl::kStatus && buf[4] == 0x04 && buf[7] == 0x01);
  CHECK(buf[12] == 16 && buf[13] == pl::kStateHolding && buf[28] == 10);
  pl::Status t;
  CHECK(pl::parseStatus(buf, n, t));
  CHECK(t.uptimeSec == s.uptimeSec && t.fpsX10 == 600 && t.outputLength == 486 && t.tempX10 == -55);
  CHECK(t.framesShown == 7 && t.implicitFrames == 8 && t.badPackets == 9 && t.packetsReceived == 10);
  CHECK(!pl::parseStatus(buf, 31, t));

  // A parser never accepts another packet type
  pl::buildSync(buf, 1);
  CHECK(!pl::parsePixels(buf, 8, px) && !pl::parseInfo(buf, 34, out) && !pl::parseStatus(buf, 32, t));

  // Frame numbers wrap
  CHECK(pl::frameDiff(1, 0) == 1);
  CHECK(pl::frameDiff(0, 65535) == 1);
  CHECK(pl::frameDiff(65535, 0) == -1);
  CHECK(pl::frameDiff(10, 70) == -60);

  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("protocol_test: ALL PASS\n");
  return 0;
}
