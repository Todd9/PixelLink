// PixelLink.h — Pixel Link protocol, version 1
//
// Shared definitions for every Pixel Link sender and receiver (Teensy
// receivers and senders, and computer senders such as TouchDesigner). Plain C++,
// no Arduino dependencies. All multi-byte fields are little-endian.
//
// Full description: docs/spec.md in https://github.com/Todd9/PixelLink

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace pixellink {

// ---- Constants ------------------------------------------------------------

constexpr uint8_t  kMagic0           = 'P';
constexpr uint8_t  kMagic1           = 'X';
constexpr uint8_t  kVersion          = 1;
constexpr uint16_t kDefaultDataPort  = 5150;   // UDP: PIXELS, SYNC, DISCOVER
constexpr uint16_t kDefaultUpdatePort = 5151;  // TCP: firmware upload (reserved)

constexpr size_t   kMaxPayload       = 1472;   // fits a 1500-byte MTU, no fragmentation

enum PacketType : uint8_t {
  kPixels   = 0x01,  // sender -> receiver
  kSync     = 0x02,  // sender -> receiver(s), often broadcast
  kDiscover = 0x03,  // sender -> broadcast
  kInfo     = 0x04,  // receiver -> sender (reply to DISCOVER)
  kStatus   = 0x05,  // receiver -> sender (debug mode)
};

constexpr size_t kHeaderSize       = 4;
constexpr size_t kPixelsHeaderSize = 12;
constexpr size_t kSyncSize         = 8;
constexpr size_t kDiscoverSize     = 4;
constexpr size_t kInfoSize         = 34;
constexpr size_t kStatusSize       = 32;
constexpr size_t kNodeNameSize     = 16;

constexpr uint16_t kMaxPixelsPerPacket = (kMaxPayload - kPixelsHeaderSize) / 3;  // 486

// PIXELS flags
constexpr uint8_t kFlagPush    = 0x01;  // show the frame after this packet
constexpr uint8_t kFormatMask  = 0x06;  // bits 1-2
constexpr uint8_t kFormatRGB   = 0x00;
constexpr uint8_t kFormatRGBW  = 0x02;  // reserved, not implemented

// STATUS state values
enum NodeState : uint8_t {
  kStateReceiving = 0,
  kStateHolding   = 1,
  kStateFading    = 2,
  kStateDark      = 3,
  kStateTest      = 4,
};

// ---- Byte helpers ---------------------------------------------------------

inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}

// Frame numbers wrap at 65535. Returns > 0 when a is newer than b.
inline int16_t frameDiff(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)); }

inline void writeHeader(uint8_t* p, PacketType type) {
  p[0] = kMagic0; p[1] = kMagic1; p[2] = kVersion; p[3] = type;
}

// True if the packet has a valid Pixel Link header. The type is p[3].
inline bool checkHeader(const uint8_t* p, size_t len) {
  return len >= kHeaderSize && p[0] == kMagic0 && p[1] == kMagic1 && p[2] == kVersion;
}

// ---- PIXELS ---------------------------------------------------------------

struct Pixels {
  uint16_t frame;
  uint8_t  channel;
  uint8_t  flags;
  uint16_t offset;
  uint16_t count;
  const uint8_t* data;  // count * 3 bytes, in the strip's color order
};

// Parses a PIXELS packet (header already checked). Fails if the packet is
// shorter than its pixel count says.
inline bool parsePixels(const uint8_t* p, size_t len, Pixels& out) {
  if (len < kPixelsHeaderSize || p[3] != kPixels) return false;
  out.frame   = rd16(p + 4);
  out.channel = p[6];
  out.flags   = p[7];
  out.offset  = rd16(p + 8);
  out.count   = rd16(p + 10);
  out.data    = p + kPixelsHeaderSize;
  return size_t(out.count) * 3 <= len - kPixelsHeaderSize;
}

// Writes just the 12-byte PIXELS header, for senders that put the pixel data
// at buf + kPixelsHeaderSize themselves. Returns the full packet size.
inline size_t writePixelsHeader(uint8_t* buf, uint16_t frame, uint8_t channel, uint8_t flags,
                                uint16_t offset, uint16_t count) {
  writeHeader(buf, kPixels);
  wr16(buf + 4, frame);
  buf[6] = channel;
  buf[7] = flags;
  wr16(buf + 8, offset);
  wr16(buf + 10, count);
  return kPixelsHeaderSize + size_t(count) * 3;
}

// Builds a PIXELS packet into buf (at least 12 + count*3 bytes).
// Returns the packet size, or 0 if count is too large for one packet.
inline size_t buildPixels(uint8_t* buf, uint16_t frame, uint8_t channel, uint8_t flags,
                          uint16_t offset, uint16_t count, const uint8_t* rgb) {
  if (count > kMaxPixelsPerPacket) return 0;
  if (count) memcpy(buf + kPixelsHeaderSize, rgb, size_t(count) * 3);
  return writePixelsHeader(buf, frame, channel, flags, offset, count);
}

// ---- SYNC -----------------------------------------------------------------

inline size_t buildSync(uint8_t* buf, uint16_t frame) {
  writeHeader(buf, kSync);
  wr16(buf + 4, frame);
  wr16(buf + 6, 0);
  return kSyncSize;
}

inline bool parseSync(const uint8_t* p, size_t len, uint16_t& frame) {
  if (len < kSyncSize || p[3] != kSync) return false;
  frame = rd16(p + 4);
  return true;
}

// ---- DISCOVER -------------------------------------------------------------

inline size_t buildDiscover(uint8_t* buf) {
  writeHeader(buf, kDiscover);
  return kDiscoverSize;
}

// ---- INFO -----------------------------------------------------------------

struct Info {
  uint8_t  fwVersion[3];          // major, minor, patch
  uint8_t  mac[6];
  uint8_t  maxChannels;
  uint16_t maxPixelsPerChannel;
  char     name[kNodeNameSize + 1];  // zero-terminated copy
};

inline size_t buildInfo(uint8_t* buf, const Info& in) {
  writeHeader(buf, kInfo);
  memcpy(buf + 4, in.fwVersion, 3);
  buf[7] = 0;
  memcpy(buf + 8, in.mac, 6);
  buf[14] = in.maxChannels;
  buf[15] = 0;
  wr16(buf + 16, in.maxPixelsPerChannel);
  // Fixed 16-byte field, zero-padded. A 16-character name fills it with no
  // terminator, which is allowed (parseInfo adds one).
  memset(buf + 18, 0, kNodeNameSize);
  memcpy(buf + 18, in.name, strnlen(in.name, kNodeNameSize));
  return kInfoSize;
}

inline bool parseInfo(const uint8_t* p, size_t len, Info& out) {
  if (len < kInfoSize || p[3] != kInfo) return false;
  memcpy(out.fwVersion, p + 4, 3);
  memcpy(out.mac, p + 8, 6);
  out.maxChannels = p[14];
  out.maxPixelsPerChannel = rd16(p + 16);
  memcpy(out.name, p + 18, kNodeNameSize);
  out.name[kNodeNameSize] = '\0';
  return true;
}

// ---- STATUS ---------------------------------------------------------------

struct Status {
  uint32_t uptimeSec;
  uint16_t fpsX10;           // frames shown per second x 10
  uint16_t outputLength;     // pixels clocked out per pin
  uint8_t  activeChannels;   // channels that received data in the last second
  uint8_t  state;            // NodeState
  int16_t  tempX10;          // CPU temperature, degC x 10
  uint32_t framesShown;
  uint32_t implicitFrames;   // shown without PUSH or SYNC
  uint32_t badPackets;
  uint32_t packetsReceived;
};

inline size_t buildStatus(uint8_t* buf, const Status& s) {
  writeHeader(buf, kStatus);
  wr32(buf + 4, s.uptimeSec);
  wr16(buf + 8, s.fpsX10);
  wr16(buf + 10, s.outputLength);
  buf[12] = s.activeChannels;
  buf[13] = s.state;
  wr16(buf + 14, uint16_t(s.tempX10));
  wr32(buf + 16, s.framesShown);
  wr32(buf + 20, s.implicitFrames);
  wr32(buf + 24, s.badPackets);
  wr32(buf + 28, s.packetsReceived);
  return kStatusSize;
}

inline bool parseStatus(const uint8_t* p, size_t len, Status& s) {
  if (len < kStatusSize || p[3] != kStatus) return false;
  s.uptimeSec       = rd32(p + 4);
  s.fpsX10          = rd16(p + 8);
  s.outputLength    = rd16(p + 10);
  s.activeChannels  = p[12];
  s.state           = p[13];
  s.tempX10         = int16_t(rd16(p + 14));
  s.framesShown     = rd32(p + 16);
  s.implicitFrames  = rd32(p + 20);
  s.badPackets      = rd32(p + 24);
  s.packetsReceived = rd32(p + 28);
  return true;
}

}  // namespace pixellink
