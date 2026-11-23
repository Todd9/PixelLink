// PixelLinkReceiver — Pixel Link remote node for Teensy 4.1
//
// Receives raw RGB pixel data over Ethernet (Pixel Link protocol v1) and
// drives up to 32 WS2811-style LED channels in parallel with ObjectFLED.
// The node sizes its output from the data it receives: no config file.
//
// Libraries:
//   QNEthernet   (Library Manager)
//   ObjectFLED   (github.com/KurtMF/ObjectFLED)
//   Watchdog_t4  (bundled with Teensyduino)
//   FlasherX     copy FlashTxx.c and FlashTxx.h from
//                github.com/joepasquariello/FlasherX into this sketch folder
//
// Firmware updates over Ethernet: export the compiled binary (.hex) and send
// it with extras/pixellink_upload.py. See PixelLinkOta.h for details.
//
// Notes
//   - For 60 fps, the longest string needs to be about 530 px or less
//   - To avoid multi-packeting, keep the pixel count at or below 486 per string
//
// Behavior summary (details in docs/spec.md):
//   - Each PIXELS packet carries one channel (or part of one) of one frame.
//   - Frames are shown on PUSH, on SYNC, or implicitly when a newer frame starts.
//   - Only channels that receive data are driven. Output length follows the
//     longest channel: it grows at once and shrinks after 2 s of shorter frames.
//   - No data for HOLD_TIME_MS: fade to black over FADE_TIME_MS, then stay dark.
//   - Pixel values are never changed except by the fade and the test pattern.

#include <QNEthernet.h>
#include <ObjectFLED.h>
#include <Watchdog_t4.h>
#include "PixelLink.h"
#include "PixelLinkOta.h"

namespace qn = qindesign::network;  // no "using namespace": QNEthernet has a "driver" namespace
namespace pl = pixellink;

// =========================== SETTINGS ===================================

#define NODE_NAME               "remote"     // up to 15 characters, shown in INFO

// Network
const uint8_t IP_ADDRESS[4] = {192, 168, 177, 111};
const uint8_t NETMASK[4]    = {255, 255, 255, 0};
const uint8_t GATEWAY[4]    = {192, 168, 177, 1};  // unused on a direct link
#define USE_DHCP                0            // 1 = try DHCP first, fall back to IP_ADDRESS
#define DHCP_TIMEOUT_MS         5000
#define DATA_PORT               pl::kDefaultDataPort   // 5150
#define UPDATE_PORT             pl::kDefaultUpdatePort // 5151, TCP firmware updates

// Hardware: one entry per channel, in jack order (jack 1 = channels 0-3).
// Pin 29 isn't used on this board.
const uint8_t PIN_LIST[] = {
  34, 35, 36, 37, 38, 39, 14, 15,
  16, 17, 18, 19, 20, 21, 22, 23,
   6,  7,  8,  9, 10, 11, 12, 13,
  24, 25, 26, 27, 28, 30, 31, 32,
};
#define COLOR_ORDER             CORDER_RGB   // pass-through: the sender orders each channel's bytes
#define LED_LATCH_US            300          // LED reset time between frames

#define MAX_PIXELS_PER_CHANNEL  972          // two packets per channel

// Link loss
#define HOLD_TIME_MS            10000        // hold the last frame this long
#define FADE_TIME_MS            3000         // then fade to black over this long

// Test mode
#define TEST_ALWAYS             0            // run once at every boot
#define TEST_IF_NO_SIGNAL       1            // loop if no data within 5 s of boot
#define TEST_NEVER              2
#define TEST_MODE               TEST_IF_NO_SIGNAL
#define TEST_BRIGHTNESS         64           // 0-255 (64 = 25%), limits power draw
#define TEST_PIXELS_PER_CHANNEL 486          // max 682 with 32 channels

// Debug: STATUS packets to the sender once a second, plus serial logging
#define DEBUG                   0

// ========================================================================

// Fixed constants
const uint8_t  FW_VERSION[3]           = {1, 1, 4};
const uint32_t SHRINK_DELAY_MS         = 2000;  // output shrinks after this long
const uint32_t NO_SIGNAL_TEST_DELAY_MS = 5000;  // IF_NO_SIGNAL waits this long
// Resend the current frame this often when nothing new arrives (test pattern,
// hold, dark). Some LED chips (e.g. GS8206) start their own built-in test
// pattern when their data line goes quiet; this keeps the line busy.
const uint32_t REFRESH_MS              = 25;    // 40 Hz
const uint32_t TEST_STEP_MS            = 1000;  // red, green, blue
const uint32_t TEST_ID_MS              = 3000;  // channel ID pattern
const uint32_t FADE_STEP_MS            = 20;    // fade update interval
const uint32_t STATUS_INTERVAL_MS      = 1000;
const uint32_t HOLDING_AFTER_MS        = 1000;  // STATUS reports "holding" after this gap
const int16_t  STALE_FRAME_WINDOW      = 60;    // older frames within this are late packets
const int      MAX_PACKETS_PER_LOOP    = 64;
const size_t   UDP_QUEUE_SIZE          = 64;    // packets buffered between loop passes
const int      WATCHDOG_TIMEOUT_S      = 4;

// ObjectFLED indexes its frame buffer with a 16-bit counter, so one driver
// can handle at most 65535 bytes: 21845 RGB LEDs across all driven channels.
const uint32_t MAX_DRIVER_LEDS = 21845;

const uint8_t  NUM_CHANNELS = sizeof(PIN_LIST);
const uint32_t ALL_CHANNELS = (NUM_CHANNELS == 32) ? 0xFFFFFFFFu : ((1u << NUM_CHANNELS) - 1);

static_assert(NUM_CHANNELS <= 32, "PIN_LIST supports at most 32 channels");
static_assert(sizeof(NODE_NAME) - 1 <= 15, "NODE_NAME is limited to 15 characters");
static_assert(TEST_PIXELS_PER_CHANNEL <= MAX_PIXELS_PER_CHANNEL,
              "TEST_PIXELS_PER_CHANNEL must not exceed MAX_PIXELS_PER_CHANNEL");
static_assert(uint32_t(TEST_PIXELS_PER_CHANNEL) * NUM_CHANNELS <= MAX_DRIVER_LEDS,
              "TEST_PIXELS_PER_CHANNEL x channels exceeds the ObjectFLED limit");

#if DEBUG
#define DBG(...) Serial.printf(__VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#endif

// ---- Buffers -------------------------------------------------------------
// DMAMEM (RAM2) is not cleared at boot; setup() zeroes both buffers.

// Back buffer: every channel at full size. Incoming data lands here and
// persists between frames.
DMAMEM static uint8_t backBuf[NUM_CHANNELS * MAX_PIXELS_PER_CHANNEL * 3];

// Staging buffer: the next frame in ObjectFLED's layout (driven channels in
// ascending order, outputLen pixels each). ObjectFLED copies it in show().
DMAMEM static uint8_t stageBuf[MAX_DRIVER_LEDS * 3];

static inline uint8_t* channelPtr(uint8_t ch) {
  return backBuf + size_t(ch) * MAX_PIXELS_PER_CHANNEL * 3;
}

// ---- State ---------------------------------------------------------------

qn::EthernetUDP udp{UDP_QUEUE_SIZE};
WDT_T4<WDT1> wdt;
pl::OtaUpdater ota{UPDATE_PORT};

// The update check looks for this string in a new image. Keeping it in this
// firmware means this firmware can itself be updated over Ethernet later.
__attribute__((used)) const char FIRMWARE_ID[] = FLASH_ID;

enum class Mode : uint8_t { WaitForData, Test, Receiving, Fading, Dark };
static Mode mode = Mode::Dark;

// LED driver, rebuilt whenever the driven channels or output length change
static ObjectFLED* driver   = nullptr;
static uint32_t    drvMask  = 0;
static uint16_t    drvLen   = 0;

// Staged frame waiting to be shown (latest wins)
static bool     staged      = false;
static uint32_t stagedMask  = 0;
static uint16_t stagedLen   = 0;

// Frame assembly
static bool     haveFrame    = false;
static uint16_t currentFrame = 0;
static bool     frameDirty   = false;   // data written since the last present
static uint32_t frameMask    = 0;       // channels written in this frame
static uint16_t frameLen     = 0;       // longest channel in this frame

// Output geometry for data frames
static uint32_t geoMask  = 0;
static uint16_t geoLen   = 0;
static uint32_t winMask  = 0;           // shrink window: what recent frames used
static uint16_t winLen   = 0;
static uint32_t winStart = 0;

// Timing
static uint32_t bootMs       = 0;
static uint32_t lastDataMs   = 0;
static bool     everReceived = false;
static uint32_t fadeStartMs  = 0;
static uint32_t lastFadeMs   = 0;
static uint32_t testStartMs  = 0;
static int      testPhase    = -1;
static bool     testInterruptible = false;

// Who to send STATUS to: the most recent PIXELS sender
static IPAddress senderIP;
static uint16_t  senderPort = 0;

// Counters
static uint32_t framesShown     = 0;
static uint32_t implicitFrames  = 0;
static uint32_t badPackets      = 0;
static uint32_t packetsReceived = 0;
static uint32_t limitClamps     = 0;
static uint32_t fpsCount        = 0;
static uint32_t chanSeenMask    = 0;    // channels with data since last STATUS
static uint32_t lastStatusMs    = 0;

// ---- LED output ----------------------------------------------------------

static void rebuildDriver(uint32_t mask, uint16_t len) {
  delete driver;  // waits for any transmission in progress
  driver = nullptr;
  drvMask = mask;
  drvLen = len;

  uint8_t pins[32];
  uint8_t n = 0;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (mask & (1u << ch)) pins[n++] = PIN_LIST[ch];
  }
  if (n == 0 || len == 0) return;

  // Must be constructed after the old driver is deleted: ObjectFLED keeps its
  // frame buffer pointer in a static that the destructor frees.
  driver = new ObjectFLED(uint16_t(uint32_t(len) * n), stageBuf, COLOR_ORDER, n, pins);
  driver->begin(static_cast<uint16_t>(LED_LATCH_US));
  DBG("Output: %u channels x %u pixels\n", n, len);
}

static uint32_t lastShowMs = 0;

// Shows the staged frame once the previous one has finished clocking out.
// With nothing new staged, resends the last frame every REFRESH_MS so the
// LEDs never see an idle data line. stageBuf still holds that frame.
static void serviceOutput() {
  if (!staged) {
    if (driver && !driver->busy() && millis() - lastShowMs >= REFRESH_MS) {
      driver->show();
      lastShowMs = millis();
    }
    return;
  }
  if (driver && driver->busy()) return;
  if (!driver || stagedMask != drvMask || stagedLen != drvLen) {
    rebuildDriver(stagedMask, stagedLen);
  }
  if (driver) {
    driver->show();
    lastShowMs = millis();
    framesShown++;
    fpsCount++;
  }
  staged = false;
}

// Copies the back buffer into the staging buffer for the given geometry.
// scale 255 = unchanged; lower values dim (used only by the fade).
static void stageFromBack(uint32_t mask, uint16_t len, uint8_t scale) {
  uint8_t* dst = stageBuf;
  const size_t bytes = size_t(len) * 3;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    if (!(mask & (1u << ch))) continue;
    const uint8_t* src = channelPtr(ch);
    if (scale == 255) {
      memcpy(dst, src, bytes);
    } else if (scale == 0) {
      memset(dst, 0, bytes);
    } else {
      for (size_t i = 0; i < bytes; i++) dst[i] = uint8_t((uint16_t(src[i]) * scale) / 255);
    }
    dst += bytes;
  }
  staged = true;
  stagedMask = mask;
  stagedLen = len;
}

// Blanks the given channels right away, waiting for any frame in progress.
static void showBlackNow(uint32_t mask, uint16_t len) {
  while (driver && driver->busy()) {}
  memset(stageBuf, 0, size_t(len) * __builtin_popcount(mask) * 3);
  staged = true;
  stagedMask = mask;
  stagedLen = len;
  serviceOutput();
}

// ---- Geometry ------------------------------------------------------------

// Called once per frame. Grows at once; shrinks to what the last
// SHRINK_DELAY_MS of frames actually used.
static void updateGeometry() {
  const uint32_t now = millis();
  geoMask |= frameMask;
  if (frameLen > geoLen) geoLen = frameLen;

  winMask |= frameMask;
  if (frameLen > winLen) winLen = frameLen;
  if (now - winStart >= SHRINK_DELAY_MS) {
    if (winMask != geoMask || winLen != geoLen) {
      geoMask = winMask;
      geoLen = winLen;
    }
    winMask = 0;
    winLen = 0;
    winStart = now;
  }

  // Keep within ObjectFLED's limit by shortening the output.
  const uint32_t n = __builtin_popcount(geoMask);
  if (n && uint32_t(geoLen) * n > MAX_DRIVER_LEDS) {
    geoLen = uint16_t(MAX_DRIVER_LEDS / n);
    limitClamps++;
    DBG("LED limit: output clamped to %u pixels\n", geoLen);
  }
}

static void presentFrame() {
  updateGeometry();
  frameMask = 0;
  frameLen = 0;
  frameDirty = false;
  if (mode == Mode::Receiving) stageFromBack(geoMask, geoLen, 255);
}

// ---- Test pattern --------------------------------------------------------

static void stageTestPhase(int phase) {
  const uint8_t b = TEST_BRIGHTNESS;
  uint8_t r = 0, g = 0, bl = 0;
  if (phase == 0) r = b;
  if (phase == 1) g = b;
  if (phase == 2) bl = b;

  uint8_t* dst = stageBuf;
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    for (uint16_t px = 0; px < TEST_PIXELS_PER_CHANNEL; px++) {
      if (phase == 3) {
        // Channel ID: channel n lights its first n+1 pixels white.
        const uint8_t v = (px <= ch) ? b : 0;
        *dst++ = v; *dst++ = v; *dst++ = v;
      } else {
        *dst++ = r; *dst++ = g; *dst++ = bl;
      }
    }
  }
  staged = true;
  stagedMask = ALL_CHANNELS;
  stagedLen = TEST_PIXELS_PER_CHANNEL;
}

static void startTest(bool interruptible) {
  mode = Mode::Test;
  testStartMs = millis();
  testPhase = -1;
  testInterruptible = interruptible;
  DBG("Test pattern started\n");
}

static void endTest() {
  showBlackNow(ALL_CHANNELS, TEST_PIXELS_PER_CHANNEL);
  DBG("Test pattern ended\n");
}

static void runTest(uint32_t now) {
  const uint32_t t = now - testStartMs;
  const uint32_t cycle = 3 * TEST_STEP_MS + TEST_ID_MS;

  if (t >= cycle) {
    if (testInterruptible) {   // IF_NO_SIGNAL: loop until data arrives
      testStartMs = now;
      testPhase = -1;
      return;
    }
    // ALWAYS: one cycle, then show live data if any arrived meanwhile.
    endTest();
    if (everReceived && now - lastDataMs < HOLD_TIME_MS) {
      mode = Mode::Receiving;
      stageFromBack(geoMask, geoLen, 255);
    } else {
      mode = Mode::Dark;
    }
    return;
  }

  const int phase = (t < 3 * TEST_STEP_MS) ? int(t / TEST_STEP_MS) : 3;
  if (phase != testPhase) {
    testPhase = phase;
    stageTestPhase(phase);
  }
}

// ---- Mode handling -------------------------------------------------------

// Called for every accepted PIXELS packet.
static void onData() {
  lastDataMs = millis();
  everReceived = true;
  switch (mode) {
    case Mode::Test:
      if (testInterruptible) {
        endTest();
        mode = Mode::Receiving;
      }
      break;  // TEST_ALWAYS finishes its cycle first
    case Mode::WaitForData:
    case Mode::Fading:
    case Mode::Dark:
      mode = Mode::Receiving;
      break;
    case Mode::Receiving:
      break;
  }
}

static void updateMode(uint32_t now) {
  switch (mode) {
    case Mode::WaitForData:
      if (now - bootMs >= NO_SIGNAL_TEST_DELAY_MS) startTest(true);
      break;

    case Mode::Test:
      runTest(now);
      break;

    case Mode::Receiving:
      if (now - lastDataMs >= HOLD_TIME_MS) {
        mode = Mode::Fading;
        fadeStartMs = now;
        lastFadeMs = 0;
        DBG("No data: fading out\n");
      }
      break;

    case Mode::Fading: {
      const uint32_t elapsed = now - fadeStartMs;
      if (elapsed >= FADE_TIME_MS) {
        stageFromBack(geoMask, geoLen, 0);
        mode = Mode::Dark;
        DBG("Dark\n");
      } else if (now - lastFadeMs >= FADE_STEP_MS) {
        // Squared curve: looks more even to the eye than a linear ramp.
        const float remaining = 1.0f - float(elapsed) / FADE_TIME_MS;
        stageFromBack(geoMask, geoLen, uint8_t(255.0f * remaining * remaining));
        lastFadeMs = now;
      }
      break;
    }

    case Mode::Dark:
      break;
  }
}

// ---- Packet handling -----------------------------------------------------

static void onPixels(const uint8_t* p, size_t len, const IPAddress& ip, uint16_t port) {
  pl::Pixels px;
  if (!pl::parsePixels(p, len, px) ||
      (px.flags & pl::kFormatMask) != pl::kFormatRGB ||
      px.channel >= NUM_CHANNELS ||
      uint32_t(px.offset) + px.count > MAX_PIXELS_PER_CHANNEL) {
    badPackets++;
    return;
  }

  senderIP = ip;
  senderPort = port;

  if (!haveFrame) {
    currentFrame = px.frame;
    haveFrame = true;
  }
  const int16_t d = pl::frameDiff(px.frame, currentFrame);
  if (d < 0 && d > -STALE_FRAME_WINDOW) return;  // late packet from an older frame
  if (d != 0) {
    // A newer frame (or the sender restarted its counter).
    if (frameDirty) {
      presentFrame();
      implicitFrames++;
    }
    currentFrame = px.frame;
  }

  onData();

  if (px.count) {
    memcpy(channelPtr(px.channel) + size_t(px.offset) * 3, px.data, size_t(px.count) * 3);
    frameMask |= 1u << px.channel;
    chanSeenMask |= 1u << px.channel;
    const uint16_t end = px.offset + px.count;
    if (end > frameLen) frameLen = end;
    frameDirty = true;
  }

  if ((px.flags & pl::kFlagPush) && frameDirty) presentFrame();
}

static void onSync(const uint8_t* p, size_t len) {
  uint16_t frame;
  if (!pl::parseSync(p, len, frame)) {
    badPackets++;
    return;
  }
  if (frameDirty && pl::frameDiff(frame, currentFrame) >= 0) presentFrame();
}

static void sendInfo(const IPAddress& ip, uint16_t port) {
  pl::Info info;
  memcpy(info.fwVersion, FW_VERSION, 3);
  qn::Ethernet.macAddress(info.mac);
  info.maxChannels = NUM_CHANNELS;
  info.maxPixelsPerChannel = MAX_PIXELS_PER_CHANNEL;
  strncpy(info.name, NODE_NAME, sizeof(info.name));
  info.name[sizeof(info.name) - 1] = '\0';

  uint8_t buf[pl::kInfoSize];
  const size_t n = pl::buildInfo(buf, info);
  udp.send(ip, port, buf, n);
}

static void handlePacket(const uint8_t* p, size_t len, const IPAddress& ip, uint16_t port) {
  if (!pl::checkHeader(p, len)) {
    badPackets++;
    return;
  }
  packetsReceived++;
  switch (p[3]) {
    case pl::kPixels:   onPixels(p, len, ip, port); break;
    case pl::kSync:     onSync(p, len); break;
    case pl::kDiscover: sendInfo(ip, port); break;
    default:            badPackets++; break;
  }
}

static void receivePackets() {
  for (int i = 0; i < MAX_PACKETS_PER_LOOP; i++) {
    const int size = udp.parsePacket();
    if (size < 0) break;
    handlePacket(udp.data(), size_t(size), udp.remoteIP(), udp.remotePort());
  }
}

// ---- Status (debug mode) -------------------------------------------------
#if DEBUG

static uint8_t reportedState(uint32_t now) {
  switch (mode) {
    case Mode::Receiving:
      return (now - lastDataMs > HOLDING_AFTER_MS) ? pl::kStateHolding : pl::kStateReceiving;
    case Mode::Fading: return pl::kStateFading;
    case Mode::Test:   return pl::kStateTest;
    default:           return pl::kStateDark;
  }
}

static void sendStatus(uint32_t now) {
  const uint32_t elapsed = now - lastStatusMs;
  if (elapsed < STATUS_INTERVAL_MS) return;

  pl::Status s;
  s.uptimeSec       = now / 1000;
  s.fpsX10          = uint16_t(fpsCount * 10000 / elapsed);
  s.outputLength    = drvLen;
  s.activeChannels  = uint8_t(__builtin_popcount(chanSeenMask));
  s.state           = reportedState(now);
  s.tempX10         = int16_t(tempmonGetTemp() * 10.0f);
  s.framesShown     = framesShown;
  s.implicitFrames  = implicitFrames;
  s.badPackets      = badPackets;
  s.packetsReceived = packetsReceived;

  if (senderPort != 0) {
    uint8_t buf[pl::kStatusSize];
    udp.send(senderIP, senderPort, buf, pl::buildStatus(buf, s));
  }

  DBG("fps %u.%u  out %u ch x %u px  active %u  state %u  temp %d.%d C  bad %lu  implicit %lu  clamps %lu\n",
      s.fpsX10 / 10, s.fpsX10 % 10, __builtin_popcount(drvMask), drvLen, s.activeChannels,
      s.state, s.tempX10 / 10, abs(s.tempX10 % 10), (unsigned long)badPackets,
      (unsigned long)implicitFrames, (unsigned long)limitClamps);

  fpsCount = 0;
  chanSeenMask = 0;
  lastStatusMs = now;
}

#endif  // DEBUG

// ---- Firmware updates ----------------------------------------------------

static void otaFeed() { wdt.feed(); }

// LEDs hold the frame they're showing for the whole update.
static void otaStart() {
  while (driver && driver->busy()) {}
  staged = false;
  DBG("Firmware update: receiving\n");
}

static void otaFlash() {
  DBG("Firmware update: installing, rebooting when done\n");
  while (driver && driver->busy()) {}
  // Installing can take several seconds with no chance to feed the watchdog,
  // so raise its timeout to the maximum (128 s) first. WT is bits 8-15 of WCR.
  WDOG1_WCR = uint16_t((WDOG1_WCR & 0x00FF) | 0xFF00);
  wdt.feed();
  qn::Ethernet.end();
}

static void otaEnd() {
  DBG("Firmware update: cancelled (%s)\n", ota.lastError());
  lastDataMs = millis();  // restart the hold timer instead of fading at once
}

// While an update runs, pixel data is read and discarded so it can't pile up.
static void drainPackets() {
  for (int i = 0; i < MAX_PACKETS_PER_LOOP && udp.parsePacket() >= 0; i++) {}
}

// ---- Setup and loop ------------------------------------------------------

static void startNetwork() {
  qn::Ethernet.setHostname(NODE_NAME);
  bool ok = false;
#if USE_DHCP
  if (qn::Ethernet.begin()) ok = qn::Ethernet.waitForLocalIP(DHCP_TIMEOUT_MS);
  if (!ok) DBG("DHCP failed, using static address\n");
#endif
  if (!ok) {
    qn::Ethernet.setDHCPEnabled(false);
    qn::Ethernet.begin(IPAddress(IP_ADDRESS), IPAddress(NETMASK), IPAddress(GATEWAY));
  }
  udp.begin(DATA_PORT);
  ota.begin(otaFeed, otaStart, otaFlash, otaEnd);

#if DEBUG
  uint8_t mac[6];
  qn::Ethernet.macAddress(mac);
  IPAddress ip = qn::Ethernet.localIP();
  DBG("%s fw %u.%u.%u (%s)  IP %u.%u.%u.%u  data port %u  update port %u  MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
      NODE_NAME, FW_VERSION[0], FW_VERSION[1], FW_VERSION[2], FIRMWARE_ID,
      ip[0], ip[1], ip[2], ip[3], DATA_PORT, UPDATE_PORT,
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
}

void setup() {
#if DEBUG
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}
#endif

  // Drive every output low so unused jacks don't float.
  for (uint8_t ch = 0; ch < NUM_CHANNELS; ch++) {
    pinMode(PIN_LIST[ch], OUTPUT);
    digitalWriteFast(PIN_LIST[ch], LOW);
  }

  memset(backBuf, 0, sizeof(backBuf));
  memset(stageBuf, 0, sizeof(stageBuf));

  // Reference FIRMWARE_ID so the linker can't discard it (see its comment).
  asm volatile("" : : "r"(FIRMWARE_ID));

  // Start from a known dark state.
  showBlackNow(ALL_CHANNELS, TEST_PIXELS_PER_CHANNEL);

  startNetwork();

  WDT_timings_t cfg;
  cfg.timeout = WATCHDOG_TIMEOUT_S;
  wdt.begin(cfg);

  bootMs = millis();
  winStart = bootMs;
  lastStatusMs = bootMs;

#if TEST_MODE == TEST_ALWAYS
  startTest(false);
#elif TEST_MODE == TEST_IF_NO_SIGNAL
  mode = Mode::WaitForData;
#else
  mode = Mode::Dark;
#endif
}

void loop() {
  wdt.feed();

  ota.poll();
  if (ota.active()) {
    drainPackets();
    return;
  }

  receivePackets();
  const uint32_t now = millis();
  updateMode(now);
  serviceOutput();
#if DEBUG
  sendStatus(now);
#endif
}
