// FastLEDSender — FastLED with local pins and remote pins
//
// Drives one strip on a pin of this Teensy and four strips on a Pixel Link
// receiver, all from one FastLED.show(). The receiver's channels are added
// with FastLED.addLeds() like any other strip, so FastLED's brightness, color
// correction and dithering apply to them too. See docs/fastled-remote-pins.md.
//
// Prints the receiver's INFO reply once it answers, and a status line every
// 2 s (the receiver's part appears when it is built with DEBUG 1).
//
// Board: Teensy 4.1
// Libraries: FastLED 3.10.x (tested against 3.10.8), QNEthernet, PixelLink
//
// More receivers: declare one Node per receiver and add their channels the
// same way. Every FastLED.show() then ends with a SYNC to each receiver that
// got data, so they all show the frame together.

#include <FastLED.h>
#include <QNEthernet.h>
#include <PixelLinkFastLED.h>

namespace qn = qindesign::network;  // no "using namespace": QNEthernet has a "driver" namespace
namespace pl = pixellink;

// =========================== SETTINGS ===================================

const uint8_t MY_IP[4]       = {192, 168, 177, 100};  // this Teensy
const uint8_t NETMASK[4]     = {255, 255, 255, 0};
const uint8_t RECEIVER_IP[4] = {192, 168, 177, 111};  // the receiver's IP_ADDRESS

#define LOCAL_PIN      2      // a strip wired to this Teensy
#define LOCAL_LEDS     30
#define REMOTE_STRIPS  4      // receiver channels 0 .. REMOTE_STRIPS-1
#define REMOTE_LEDS    150    // per strip; over 486 is split across packets for you
#define BRIGHTNESS     32     // 0-255; keep low on the bench to limit current
#define REPORT_MS      2000   // status print interval

// ========================================================================

pl::Sender sender;                                  // one per sketch, declared before any Node
pl::Node   receiver(sender, IPAddress(RECEIVER_IP));

CRGB localLeds[LOCAL_LEDS];
CRGB remoteLeds[REMOTE_STRIPS][REMOTE_LEDS];

// Asks the receiver to identify itself until it answers, then prints it.
static void identifyReceiver() {
  static bool shown = false;
  static uint32_t lastAskMs = 0;
  if (shown) return;
  if (receiver.hasInfo()) {
    const pl::Info& in = receiver.info();
    Serial.printf("receiver: \"%s\" fw %u.%u.%u, %u channels x %u px max\n", in.name,
                  in.fwVersion[0], in.fwVersion[1], in.fwVersion[2],
                  in.maxChannels, in.maxPixelsPerChannel);
    shown = true;
  } else if (millis() - lastAskMs >= 1000) {
    lastAskMs = millis();
    receiver.requestInfo();
  }
}

static void report() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < REPORT_MS) return;
  lastMs = millis();

  const pl::Sender::Stats& st = sender.stats();
  Serial.printf("%u fps  sent %lu frames, %lu packets, %lu failures\n", FastLED.getFPS(),
                (unsigned long)st.frames, (unsigned long)st.packets,
                (unsigned long)st.sendFailures);
  if (receiver.hasStatus() && receiver.statusAgeMs() < 3000) {
    const pl::Status& rs = receiver.status();
    Serial.printf("receiver: %u.%u fps  out %u px  %u ch  %d.%d C  implicit %lu  bad %lu\n",
                  rs.fpsX10 / 10, rs.fpsX10 % 10, rs.outputLength, rs.activeChannels,
                  rs.tempX10 / 10, abs(rs.tempX10 % 10),
                  (unsigned long)rs.implicitFrames, (unsigned long)rs.badPackets);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  // Ethernet first, then the sender.
  qn::Ethernet.begin(IPAddress(MY_IP), IPAddress(NETMASK), IPAddress(MY_IP));
  sender.begin();

  // A local pin, exactly as usual.
  FastLED.addLeds<WS2812B, LOCAL_PIN, GRB>(localLeds, LOCAL_LEDS);

  // Remote pins: one receiver channel per strip. The color order is applied
  // here on the sender; the receiver passes bytes through unchanged.
  for (uint8_t ch = 0; ch < REMOTE_STRIPS; ch++) {
    FastLED.addLeds(receiver.channel(ch), remoteLeds[ch], REMOTE_LEDS);
    receiver.channel(ch)->setColorOrder(GRB);
  }

  FastLED.setBrightness(BRIGHTNESS);

  Serial.printf("FastLEDSender: %u local + %u x %u remote -> %u.%u.%u.%u\n", LOCAL_LEDS,
                REMOTE_STRIPS, REMOTE_LEDS,
                RECEIVER_IP[0], RECEIVER_IP[1], RECEIVER_IP[2], RECEIVER_IP[3]);
  if (!qn::Ethernet.waitForLink(5000)) {
    Serial.println("No Ethernet link yet: check the cable and the receiver's power.");
  }
}

void loop() {
  static uint8_t hue = 0;

  fill_rainbow(localLeds, LOCAL_LEDS, hue, 8);
  for (uint8_t ch = 0; ch < REMOTE_STRIPS; ch++) {
    fill_rainbow(remoteLeds[ch], REMOTE_LEDS, uint8_t(hue + ch * 64), 2);
  }
  hue++;

  // Clocks out the local pin and sends PIXELS + SYNC to the receiver.
  // FastLED paces this call to what the receiver can show for REMOTE_LEDS.
  FastLED.show();

  sender.poll();   // INFO and STATUS replies
  identifyReceiver();
  report();
}
