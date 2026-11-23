// PixelLinkOta.h — firmware updates over Ethernet for the Pixel Link receiver
//
// A TCP server (default port 5151) that accepts an Intel HEX firmware image,
// stores it in spare flash, verifies it, and installs it with FlasherX's
// flash_move(). Use extras/pixellink_upload.py to send a .hex file.
//
// Requires FlashTxx.c and FlashTxx.h from FlasherX
// (github.com/joepasquariello/FlasherX), copied into the sketch folder.
//
// Session (one line per message, LF-terminated):
//   1. Client connects and sends the .hex file line by line.
//   2. After the EOF record the node checks the image and replies
//        READY <lines> <bytes>        or    ERROR <reason>
//   3. Client compares <lines> with its own count, then sends
//        GO                           or    CANCEL
//   4. On GO the node replies FLASHING, installs the image, and reboots.
//
// Checks before anything is overwritten:
//   - every HEX record's checksum
//   - the image starts at the flash base and fits in the buffer
//   - the image contains the FlasherX FLASH_ID string, which only firmware
//     built with FlashTxx has. A sketch without OTA support (the test sender,
//     for example) is rejected, so an update can never remove OTA support.
//   - the line count the node received matches the client's
//
// Any error, disconnect, or 10 s of silence before GO cancels the update,
// erases the buffer, and leaves the running firmware untouched.

#pragma once
#include <QNEthernet.h>

extern "C" {
#include "FlashTxx.h"
}

namespace pixellink {

class OtaUpdater {
 public:
  using Hook = void (*)();

  explicit OtaUpdater(uint16_t port) : server_(port) {}

  // feed:    called often during a session (feed the watchdog here)
  // onStart: called when a session begins (stop LED output here)
  // onFlash: called right before the image is installed; must make sure
  //          nothing else touches flash or times out during the copy
  // onEnd:   called when a session ends without installing
  void begin(Hook feed, Hook onStart, Hook onFlash, Hook onEnd) {
    feed_ = feed;
    onStart_ = onStart;
    onFlash_ = onFlash;
    onEnd_ = onEnd;
    server_.begin();
  }

  bool active() const { return state_ != State::Idle; }

  void poll() {
    if (state_ == State::Idle) {
      qindesign::network::EthernetClient c = server_.accept();
      if (c) start(c);
      return;
    }

    // Only one session at a time.
    qindesign::network::EthernetClient extra = server_.accept();
    if (extra) {
      extra.writeFully("ERROR busy\n");
      extra.flush();
      extra.stop();
    }

    // Read whatever has arrived, a line at a time.
    uint8_t chunk[256];
    for (int pass = 0; pass < 16 && state_ != State::Idle; pass++) {
      const int n = client_.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      lastRxMs_ = millis();
      for (int i = 0; i < n && state_ != State::Idle; i++) addByte(chunk[i]);
      feed_();
    }
    if (state_ == State::Idle) return;

    if (!client_.connected()) {
      fail("disconnected", false);
    } else if (millis() - lastRxMs_ > kTimeoutMs) {
      fail("timeout", true);
    }
  }

 private:
  enum class State : uint8_t { Idle, Receiving, AwaitingGo };

  static constexpr uint32_t kTimeoutMs = 10000;
  static constexpr size_t kMaxLine = 600;  // longest legal HEX line is 523 chars

  void start(qindesign::network::EthernetClient& c) {
    client_ = c;
    onStart_();
    feed_();
    const int type = firmware_buffer_init(&bufAddr_, &bufSize_);
    feed_();
    if (type == NO_BUFFER_TYPE) {
      reply("ERROR no buffer\n");
      client_.stop();
      onEnd_();
      return;
    }
    state_ = State::Receiving;
    lineLen_ = 0;
    lineOverflow_ = false;
    lines_ = 0;
    base_ = 0;
    minAddr_ = 0xFFFFFFFFu;
    maxAddr_ = 0;
    lastRxMs_ = millis();
  }

  void addByte(uint8_t b) {
    if (b == '\r' || b == '\n') {
      if (lineOverflow_) {
        fail("line too long", true);
      } else if (lineLen_ > 0) {
        line_[lineLen_] = '\0';
        processLine();
      }
      lineLen_ = 0;
      lineOverflow_ = false;
      return;
    }
    if (lineLen_ < kMaxLine) {
      line_[lineLen_++] = char(b);
    } else {
      lineOverflow_ = true;
    }
  }

  void processLine() {
    if (state_ == State::AwaitingGo) {
      if (strcmp(line_, "GO") == 0) {
        install();
      } else {
        fail(strcmp(line_, "CANCEL") == 0 ? "cancelled" : "expected GO", true);
      }
      return;
    }

    lines_++;
    uint8_t data[255];
    uint8_t count, type;
    uint16_t offset;
    if (!parseHexLine(line_, data, count, offset, type)) {
      fail("bad hex record", true);
      return;
    }

    switch (type) {
      case 0x00: {  // data
        const uint32_t addr = base_ + offset;
        if (addr < FLASH_BASE_ADDR || addr + count > FLASH_BASE_ADDR + bufSize_) {
          fail("address out of range", true);
          return;
        }
        const uint32_t dst = bufAddr_ + (addr - FLASH_BASE_ADDR);
        if (IN_FLASH(bufAddr_)) {
          if (flash_write_block(dst, reinterpret_cast<char*>(data), count) != 0) {
            fail("flash write failed", true);
            return;
          }
        } else {
          memcpy(reinterpret_cast<void*>(dst), data, count);
        }
        if (addr < minAddr_) minAddr_ = addr;
        if (addr + count > maxAddr_) maxAddr_ = addr + count;
        break;
      }
      case 0x01:  // end of file
        finishImage();
        break;
      case 0x02:  // extended segment address
        base_ = uint32_t((data[0] << 8) | data[1]) << 4;
        break;
      case 0x04:  // extended linear address
        base_ = uint32_t((data[0] << 8) | data[1]) << 16;
        break;
      case 0x03:  // start segment address: not needed
      case 0x05:  // start linear address: not needed
        break;
      default:
        fail("unknown record type", true);
        break;
    }
  }

  void finishImage() {
    if (maxAddr_ <= minAddr_ || minAddr_ != FLASH_BASE_ADDR) {
      fail("image does not start at flash base", true);
      return;
    }
    feed_();
    if (!check_flash_id(bufAddr_, maxAddr_ - minAddr_)) {
      fail("not OTA-capable firmware (no FLASH_ID)", true);
      return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "READY %lu %lu\n",
             (unsigned long)lines_, (unsigned long)(maxAddr_ - minAddr_));
    reply(msg);
    state_ = State::AwaitingGo;
  }

  void install() {
    reply("FLASHING\n");
    // Give the reply time to leave before the network goes quiet.
    const uint32_t t0 = millis();
    while (millis() - t0 < 200) {
      feed_();
      yield();
    }
    client_.stop();
    onFlash_();
    flash_move(FLASH_BASE_ADDR, bufAddr_, maxAddr_ - minAddr_);  // reboots; does not return
  }

  void fail(const char* reason, bool tellClient) {
    if (tellClient) {
      char msg[64];
      snprintf(msg, sizeof(msg), "ERROR %s\n", reason);
      reply(msg);
    }
    client_.stop();
    feed_();
    firmware_buffer_free(bufAddr_, bufSize_);
    feed_();
    state_ = State::Idle;
    lastError_ = reason;
    onEnd_();
  }

  void reply(const char* s) {
    client_.writeFully(s);
    client_.flush();
  }

  static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  static int hexByte(const char* p) {
    const int hi = hexNibble(p[0]), lo = hexNibble(p[1]);
    return (hi < 0 || lo < 0) ? -1 : (hi << 4) | lo;
  }

  // Parses ":LLAAAATT<data>CC" and verifies the checksum.
  static bool parseHexLine(const char* s, uint8_t* data, uint8_t& count,
                           uint16_t& offset, uint8_t& type) {
    if (s[0] != ':') return false;
    const size_t len = strlen(s);
    if (len < 11) return false;
    const int n = hexByte(s + 1);
    if (n < 0 || len != size_t(11 + n * 2)) return false;
    uint8_t sum = 0;
    int bytes[4];
    for (int i = 0; i < 4; i++) {
      bytes[i] = hexByte(s + 1 + i * 2);
      if (bytes[i] < 0) return false;
      sum += uint8_t(bytes[i]);
    }
    for (int i = 0; i < n; i++) {
      const int b = hexByte(s + 9 + i * 2);
      if (b < 0) return false;
      data[i] = uint8_t(b);
      sum += uint8_t(b);
    }
    const int check = hexByte(s + 9 + n * 2);
    if (check < 0 || uint8_t(sum + check) != 0) return false;
    count = uint8_t(n);
    offset = uint16_t((bytes[1] << 8) | bytes[2]);
    type = uint8_t(bytes[3]);
    return true;
  }

 public:
  const char* lastError() const { return lastError_; }

 private:
  qindesign::network::EthernetServer server_;
  qindesign::network::EthernetClient client_;
  Hook feed_ = nullptr, onStart_ = nullptr, onFlash_ = nullptr, onEnd_ = nullptr;

  State    state_ = State::Idle;
  char     line_[kMaxLine + 1];
  size_t   lineLen_ = 0;
  bool     lineOverflow_ = false;
  uint32_t lines_ = 0;
  uint32_t base_ = 0;
  uint32_t minAddr_ = 0, maxAddr_ = 0;
  uint32_t bufAddr_ = 0, bufSize_ = 0;
  uint32_t lastRxMs_ = 0;
  const char* lastError_ = "";
};

}  // namespace pixellink
