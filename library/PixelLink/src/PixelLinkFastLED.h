// PixelLinkFastLED.h — Pixel Link remote channels as FastLED controllers
//
// Makes each channel of a remote Pixel Link receiver look like an ordinary
// FastLED strip. Draw into its CRGB array as usual; FastLED.show() drives
// local pins and sends remote channels over Ethernet in the same call.
//
//   pl::Sender sender;                                      // one per sketch
//   pl::Node   remote(sender, IPAddress(192, 168, 177, 111));
//
//   FastLED.addLeds<WS2811, 2, GRB>(local, 14);             // a real pin
//   FastLED.addLeds(remote.channel(0), strip0, 146);        // a remote pin
//   remote.channel(0)->setColorOrder(GRB);                  // that strip's order
//
// How it works:
//   - Each remote channel is a FastLED controller (CPixelLEDController). Its
//     showPixels() reads the finished pixels, with FastLED's brightness,
//     color correction, and dithering applied, and sends PIXELS packets.
//     Channels over 486 pixels are split across packets automatically.
//   - The Sender listens for FastLED's frame events. Each FastLED.show()
//     gets one frame number; after all controllers have run, it sends SYNC to
//     every node that received data, so all nodes show the frame together.
//   - Each remote channel reports the frame rate the receiver can sustain for
//     its length, so FastLED paces show() to match the longest remote string.
//
// Requirements: FastLED 3.10.x (tested against 3.10.8), QNEthernet, Teensy 4.1.
// Declare the Sender before any Node. Start Ethernet before sender.begin().
// Color order: the sender puts each channel's bytes in the order its strip
// expects (setColorOrder, default RGB). Receivers pass the bytes through as is.
// Gamma is not applied here; apply it to your pixels if you want it.

#pragma once
#include <FastLED.h>
#include <QNEthernet.h>
#include "PixelLink.h"

#ifndef FL_NO_EXCEPT  // older FastLED versions don't use noexcept
#define FL_NO_EXCEPT
#endif

namespace pixellink {

constexpr uint8_t  kMaxNodes          = 8;
constexpr uint8_t  kMaxNodeChannels   = 32;
constexpr uint16_t kDefaultSenderPort = 5152;  // receivers reply to this port

class Node;
class RemoteChannel;

// ---- Sender ---------------------------------------------------------------

class Sender : public fl::EngineEvents::Listener {
 public:
  struct Stats {
    uint32_t frames = 0;        // frames sent
    uint32_t packets = 0;       // packets sent
    uint32_t sendFailures = 0;  // packets the network stack wouldn't take
  };

  Sender() = default;
  ~Sender() { fl::EngineEvents::removeListener(this); }

  // Call after Ethernet is up.
  bool begin(uint16_t localPort = kDefaultSenderPort) {
    fl::EngineEvents::addListener(this);
    return udp_.begin(localPort);
  }

  // Reads INFO and STATUS replies from receivers. Call from loop().
  void poll();

  // Asks every node to identify itself (the reply arrives through poll()).
  void discover();

  uint16_t frame() const { return frame_; }
  const Stats& stats() const { return stats_; }

  // FastLED frame events
  void onBeginFrame() FL_NO_EXCEPT override {
    frame_++;
    inFrame_ = true;
  }
  void onEndFrame() FL_NO_EXCEPT override;

 private:
  friend class Node;
  friend class RemoteChannel;

  bool addNode(Node* n) {
    if (nodeCount_ >= kMaxNodes) return false;
    nodes_[nodeCount_++] = n;
    return true;
  }

  // Called at the start of every channel's send. Returns true if the channel
  // is being shown outside FastLED.show() (no frame events), in which case it
  // starts its own frame and marks its last packet PUSH.
  bool beginChannel() {
    if (inFrame_) return false;
    frame_++;
    stats_.frames++;
    return true;
  }

  bool send(const IPAddress& ip, uint16_t port, const uint8_t* data, size_t len) {
    for (int attempt = 0; attempt < 3; attempt++) {
      if (udp_.send(ip, port, data, len)) {
        stats_.packets++;
        return true;
      }
      yield();  // let the network stack free transmit buffers, then retry
    }
    stats_.sendFailures++;
    return false;
  }

  static bool sameIP(const IPAddress& a, const IPAddress& b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
  }

  qindesign::network::EthernetUDP udp_{16};
  Node*    nodes_[kMaxNodes] = {};
  uint8_t  nodeCount_ = 0;
  uint16_t frame_ = 0;
  bool     inFrame_ = false;
  Stats    stats_;
  uint8_t  buf_[kMaxPayload];
};

// ---- RemoteChannel --------------------------------------------------------

class RemoteChannel : public CPixelLEDController<RGB> {
 public:
  RemoteChannel(Sender& sender, Node& node, uint8_t channel)
      : sender_(sender), node_(node), channel_(channel) {}

  void init() FL_NO_EXCEPT override {}

  // The receiver clocks every pin for as long as its longest string takes:
  // about 30 us per pixel plus the 300 us latch. Reporting that (with 10%
  // headroom) lets FastLED pace show() to what the receiver can display.
  fl::u16 getMaxRefreshRate() const FL_NO_EXCEPT override {
    const uint32_t n = uint32_t(size());
    if (n == 0) return 0;
    const uint32_t us = (n * 30 + 300) * 11 / 10;
    const uint32_t hz = 1000000 / us;
    return fl::u16(hz > 65535 ? 65535 : (hz < 1 ? 1 : hz));
  }

  uint8_t channel() const { return channel_; }

  // The byte order this channel's strip expects (FastLED's EOrder: RGB, GRB,
  // BRG, ...). Applied here, before sending; the receiver doesn't reorder.
  void setColorOrder(EOrder order) { order_ = order; }
  EOrder colorOrder() const { return order_; }

 protected:
  void showPixels(PixelController<RGB>& pixels) FL_NO_EXCEPT override;

 private:
  void sendPacket(uint16_t frame, uint16_t offset, uint16_t count, uint8_t flags);

  Sender& sender_;
  Node&   node_;
  uint8_t channel_;
  EOrder  order_ = RGB;
};

// ---- Node -----------------------------------------------------------------

class Node {
 public:
  Node(Sender& sender, const IPAddress& ip, uint16_t port = kDefaultDataPort)
      : sender_(sender), ip_(ip), port_(port) {
    sender_.addNode(this);
  }

  // The controller for one of this node's channels (0-31), for FastLED.addLeds.
  // Created on first use, so only channels you add exist as controllers.
  RemoteChannel* channel(uint8_t ch) {
    if (ch >= kMaxNodeChannels) return nullptr;
    if (!channels_[ch]) channels_[ch] = new RemoteChannel(sender_, *this, ch);
    return channels_[ch];
  }

  const IPAddress& ip() const { return ip_; }
  uint16_t port() const { return port_; }

  // Asks this node to identify itself (the reply arrives through poll()).
  void requestInfo() {
    uint8_t buf[kDiscoverSize];
    sender_.send(ip_, port_, buf, buildDiscover(buf));
  }

  // Latest INFO reply (after requestInfo or discover).
  bool hasInfo() const { return hasInfo_; }
  const Info& info() const { return info_; }

  // Latest STATUS report. Receivers send these once a second when built with DEBUG 1.
  bool hasStatus() const { return hasStatus_; }
  const Status& status() const { return status_; }
  uint32_t statusAgeMs() const { return millis() - statusMs_; }

 private:
  friend class Sender;
  friend class RemoteChannel;

  Sender&        sender_;
  IPAddress      ip_;
  uint16_t       port_;
  RemoteChannel* channels_[kMaxNodeChannels] = {};
  bool           sentThisFrame_ = false;
  bool           hasInfo_ = false;
  bool           hasStatus_ = false;
  Info           info_ = {};
  Status         status_ = {};
  uint32_t       statusMs_ = 0;
};

// ---- Implementations ------------------------------------------------------

inline void RemoteChannel::sendPacket(uint16_t frame, uint16_t offset, uint16_t count,
                                      uint8_t flags) {
  const size_t len = writePixelsHeader(sender_.buf_, frame, channel_, flags, offset, count);
  sender_.send(node_.ip_, node_.port_, sender_.buf_, len);
}

inline void RemoteChannel::showPixels(PixelController<RGB>& pixels) FL_NO_EXCEPT {
  const bool standalone = sender_.beginChannel();
  const uint16_t frame = sender_.frame_;
  uint8_t* const data = sender_.buf_ + kPixelsHeaderSize;
  uint8_t* px = data;
  uint16_t offset = 0, count = 0;
  bool sent = false;

  // EOrder packs three octal digits, one per output byte, naming the source
  // color (0 = R, 1 = G, 2 = B): RGB = 0012, GRB = 0102, ...
  const uint8_t o0 = (uint16_t(order_) >> 6) & 3;
  const uint8_t o1 = (uint16_t(order_) >> 3) & 3;
  const uint8_t o2 = uint16_t(order_) & 3;

  while (pixels.has(1)) {
    uint8_t c[3];
    c[0] = pixels.loadAndScale0();
    c[1] = pixels.loadAndScale1();
    c[2] = pixels.loadAndScale2();
    *px++ = c[o0];
    *px++ = c[o1];
    *px++ = c[o2];
    pixels.advanceData();
    pixels.stepDithering();
    if (++count == kMaxPixelsPerPacket) {
      const bool last = !pixels.has(1);
      sendPacket(frame, offset, count, (standalone && last) ? kFlagPush : 0);
      sent = true;
      offset += count;
      count = 0;
      px = data;
    }
  }
  if (count > 0) {
    sendPacket(frame, offset, count, standalone ? kFlagPush : 0);
    sent = true;
  }
  if (sent && !standalone) node_.sentThisFrame_ = true;
}

inline void Sender::onEndFrame() FL_NO_EXCEPT {
  inFrame_ = false;
  bool any = false;
  uint8_t buf[kSyncSize];
  buildSync(buf, frame_);
  for (uint8_t i = 0; i < nodeCount_; i++) {
    Node* n = nodes_[i];
    if (!n->sentThisFrame_) continue;
    send(n->ip_, n->port_, buf, kSyncSize);
    n->sentThisFrame_ = false;
    any = true;
  }
  if (any) stats_.frames++;
}

inline void Sender::poll() {
  for (int i = 0; i < 16; i++) {
    const int size = udp_.parsePacket();
    if (size < 0) return;
    const uint8_t* p = udp_.data();
    if (!checkHeader(p, size_t(size))) continue;
    const IPAddress from = udp_.remoteIP();
    for (uint8_t k = 0; k < nodeCount_; k++) {
      Node* n = nodes_[k];
      if (!sameIP(n->ip_, from)) continue;
      if (parseInfo(p, size_t(size), n->info_)) {
        n->hasInfo_ = true;
      } else if (parseStatus(p, size_t(size), n->status_)) {
        n->hasStatus_ = true;
        n->statusMs_ = millis();
      }
      break;
    }
  }
}

inline void Sender::discover() {
  for (uint8_t i = 0; i < nodeCount_; i++) nodes_[i]->requestInfo();
}

}  // namespace pixellink
