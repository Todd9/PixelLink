// RawSender — minimal Pixel Link sender, no FastLED
//
// Builds PIXELS packets by hand with PixelLink.h and sends a moving rainbow
// to one receiver over a direct cable, one packet per channel (long channels
// split with Offset), PUSH on the last. Good for bench-testing a receiver and
// as a reference for writing a sender on another platform.
//
// Prints the receiver's INFO reply at startup, and its STATUS packets once a
// second when the receiver is built with DEBUG 1.
//
// Board: Teensy 4.1 (any board QNEthernet supports)
// Libraries: QNEthernet (Library Manager), PixelLink

#include <QNEthernet.h>
#include <PixelLink.h>

namespace qn = qindesign::network;  // no "using namespace": QNEthernet has a "driver" namespace
namespace pl = pixellink;

// =========================== SETTINGS ===================================

const uint8_t MY_IP[4]       = {192, 168, 177, 100};  // this Teensy
const uint8_t NETMASK[4]     = {255, 255, 255, 0};
const uint8_t RECEIVER_IP[4] = {192, 168, 177, 111};  // the remote node
#define RECEIVER_PORT  pl::kDefaultDataPort         // 5150
#define LOCAL_PORT     5152                         // replies come back here

#define NUM_CHANNELS   16     // channels to send, starting at channel 0
#define NUM_PIXELS     100    // pixels per channel (over 486 splits into more packets)
#define FPS            40     // frames per second
#define BRIGHTNESS     32     // 0-255; keep low on the bench to limit current

// ========================================================================

qn::EthernetUDP udp{8};
static uint8_t  pixels[NUM_PIXELS * 3];
static uint8_t  packet[pl::kMaxPayload];
static uint16_t frame = 0;
static uint32_t lastFrameMs = 0;
static uint32_t lastDiscoverMs = 0;
static bool     gotInfo = false;

// Hue 0-255 to RGB at the given brightness.
static void hueToRgb(uint8_t hue, uint8_t* rgb) {
  const uint8_t region = hue / 43;
  const uint8_t rem = (hue - region * 43) * 6;
  const uint8_t up = rem, down = 255 - rem;
  uint8_t r, g, b;
  switch (region) {
    case 0:  r = 255;  g = up;   b = 0;    break;
    case 1:  r = down; g = 255;  b = 0;    break;
    case 2:  r = 0;    g = 255;  b = up;   break;
    case 3:  r = 0;    g = down; b = 255;  break;
    case 4:  r = up;   g = 0;    b = 255;  break;
    default: r = 255;  g = 0;    b = down; break;
  }
  rgb[0] = uint8_t(r * BRIGHTNESS / 255);
  rgb[1] = uint8_t(g * BRIGHTNESS / 255);
  rgb[2] = uint8_t(b * BRIGHTNESS / 255);
}

static void sendFrame() {
  const IPAddress dest(RECEIVER_IP);
  const uint8_t shift = uint8_t(frame * 2);  // animation speed

  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    // Rainbow along the string, offset per channel so channels look different.
    for (uint16_t px = 0; px < NUM_PIXELS; px++) {
      hueToRgb(uint8_t(px * 256 / NUM_PIXELS + ch * 16 + shift), &pixels[px * 3]);
    }
    // Split long channels across packets with Offset.
    for (uint16_t off = 0; off < NUM_PIXELS; off += pl::kMaxPixelsPerPacket) {
      uint16_t count = NUM_PIXELS - off;
      if (count > pl::kMaxPixelsPerPacket) count = pl::kMaxPixelsPerPacket;
      const bool last = (ch == NUM_CHANNELS - 1) && (off + count >= NUM_PIXELS);
      const size_t n = pl::buildPixels(packet, frame, ch, last ? pl::kFlagPush : 0,
                                       off, count, &pixels[off * 3]);
      udp.send(dest, RECEIVER_PORT, packet, n);
    }
  }
  frame++;
}

static void sendDiscover() {
  const size_t n = pl::buildDiscover(packet);
  udp.send(IPAddress(RECEIVER_IP), RECEIVER_PORT, packet, n);
}

static const char* stateName(uint8_t s) {
  switch (s) {
    case pl::kStateReceiving: return "receiving";
    case pl::kStateHolding:   return "holding";
    case pl::kStateFading:    return "fading";
    case pl::kStateDark:      return "dark";
    case pl::kStateTest:      return "test";
    default:                  return "?";
  }
}

static void readReplies() {
  while (true) {
    const int size = udp.parsePacket();
    if (size < 0) return;
    const uint8_t* p = udp.data();
    if (!pl::checkHeader(p, size)) continue;

    pl::Info info;
    pl::Status st;
    if (pl::parseInfo(p, size, info)) {
      gotInfo = true;
      Serial.printf("INFO: \"%s\" fw %u.%u.%u  %u channels x %u px max  MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                    info.name, info.fwVersion[0], info.fwVersion[1], info.fwVersion[2],
                    info.maxChannels, info.maxPixelsPerChannel,
                    info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
    } else if (pl::parseStatus(p, size, st)) {
      Serial.printf("STATUS: %s  %u.%u fps  out %u px  %u ch active  %d.%d C  shown %lu  implicit %lu  bad %lu  rx %lu  up %lus\n",
                    stateName(st.state), st.fpsX10 / 10, st.fpsX10 % 10, st.outputLength,
                    st.activeChannels, st.tempX10 / 10, abs(st.tempX10 % 10),
                    (unsigned long)st.framesShown, (unsigned long)st.implicitFrames,
                    (unsigned long)st.badPackets, (unsigned long)st.packetsReceived,
                    (unsigned long)st.uptimeSec);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  qn::Ethernet.begin(IPAddress(MY_IP), IPAddress(NETMASK), IPAddress(MY_IP));
  udp.begin(LOCAL_PORT);

  Serial.printf("RawSender: %u channels x %u px at %u fps -> %u.%u.%u.%u:%u\n",
                NUM_CHANNELS, NUM_PIXELS, FPS,
                RECEIVER_IP[0], RECEIVER_IP[1], RECEIVER_IP[2], RECEIVER_IP[3], RECEIVER_PORT);
  Serial.println("Waiting for link...");
  if (qn::Ethernet.waitForLink(10000)) {
    Serial.println("Link up.");
  } else {
    Serial.println("No link after 10 s: check the cable and the receiver's power.");
  }
}

void loop() {
  const uint32_t now = millis();

  // Ask the receiver to identify itself until it answers.
  if (!gotInfo && now - lastDiscoverMs >= 1000) {
    lastDiscoverMs = now;
    sendDiscover();
  }

  if (now - lastFrameMs >= 1000 / FPS) {
    lastFrameMs = now;
    sendFrame();
  }

  readReplies();
}
