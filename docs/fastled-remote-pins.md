# FastLED remote pins (`PixelLinkFastLED.h`)

`PixelLinkFastLED.h` makes each channel of a Pixel Link receiver look like an ordinary FastLED strip on the sending Teensy: a "remote pin". You draw into a `CRGB` array as usual, and one `FastLED.show()` clocks out the local pins and sends every remote channel over Ethernet.

FastLED's brightness, color correction, temperature and dithering apply to remote strips exactly as to local ones, because the remote controller reads the same finished pixels a local controller would.

## Quick start

```cpp
#include <FastLED.h>
#include <QNEthernet.h>
#include <PixelLinkFastLED.h>

pixellink::Sender sender;                                   // one per sketch, before any Node
pixellink::Node   receiver(sender, IPAddress(192, 168, 177, 111));

CRGB local[30], strip0[150];

void setup() {
  qindesign::network::Ethernet.begin(IPAddress(192, 168, 177, 100),
                                     IPAddress(255, 255, 255, 0),
                                     IPAddress(192, 168, 177, 100));
  sender.begin();                                           // after Ethernet

  FastLED.addLeds<WS2812B, 2, GRB>(local, 30);              // a real pin
  FastLED.addLeds(receiver.channel(0), strip0, 150);        // a remote pin
  receiver.channel(0)->setColorOrder(GRB);                  // that strip's order
}

void loop() {
  // ... draw into local and strip0 ...
  FastLED.show();
  sender.poll();                                            // INFO/STATUS replies
}
```

A complete sketch is the `FastLEDSender` example (File > Examples > PixelLink).

## How it works

**Each remote channel is a FastLED controller.** `RemoteChannel` derives from FastLED's `CPixelLEDController<RGB>`. When FastLED shows it, its `showPixels()` walks the finished pixels (`loadAndScale0/1/2`, so brightness and dithering are applied), puts each pixel's three bytes in the channel's color order, and sends PIXELS packets to its node.

**One frame number per `FastLED.show()`.** The `Sender` registers as a FastLED engine-event listener. `onBeginFrame` increments the frame number; every remote channel sent during that `show()` uses it. `onEndFrame` runs after all controllers, and sends one SYNC to each node that received data in that frame. Every receiver then shows the frame together, whether there is one node or several.

**Long strips are split for you.** A channel longer than 486 pixels is sent as several PIXELS packets with increasing Offset. The reference receiver accepts up to 972 pixels per channel.

**Showing one controller on its own.** If a remote channel is shown outside `FastLED.show()` (no frame events, e.g. calling a controller's `showLeds()` directly), it starts its own frame and sets PUSH on its last packet, so the receiver still shows it.

**Color order is applied on the sender.** `setColorOrder()` takes FastLED's `EOrder` (`RGB`, `GRB`, `BRG`, ...); the default is `RGB`. FastLED encodes each order as three octal digits naming the source color of each output byte (`RGB` = 0012, `GRB` = 0102), and the channel uses those digits to reorder. The receiver passes bytes through unchanged, so each channel of one receiver can have a different order.

**Pacing.** The receiver clocks every pin for as long as its longest string takes: about 30 µs per pixel plus the 300 µs latch. Each remote channel reports that rate, with 10% headroom, through `getMaxRefreshRate()`, so FastLED paces `show()` to what the receiver can display (about 194 fps at 146 px, about 60 fps at 500 px). Local pins report their own limits as usual.

**Sending.** All channels share one 1,472-byte packet buffer in the `Sender`. A send the network stack refuses is retried up to 3 times with a `yield()` between attempts, then counted in `stats().sendFailures`.

## API

### `Sender`

| Member | What it does |
|---|---|
| `bool begin(uint16_t localPort = 5152)` | Starts listening for FastLED frame events and opens the UDP socket. Call after Ethernet is up. Receivers send INFO and STATUS replies to this port. |
| `void poll()` | Reads up to 16 pending INFO/STATUS replies and stores them on the matching `Node` (matched by source IP). Call from `loop()`. |
| `void discover()` | Sends DISCOVER to every node. |
| `uint16_t frame()` | The current frame number. |
| `const Stats& stats()` | `frames` sent, `packets` sent, `sendFailures`. |

### `Node`

| Member | What it does |
|---|---|
| `Node(Sender&, const IPAddress&, uint16_t port = 5150)` | One receiver. Registers itself with the sender. |
| `RemoteChannel* channel(uint8_t ch)` | The controller for channel 0–31, for `FastLED.addLeds`. Created on first use, so only channels you use exist. Returns `nullptr` above 31. |
| `void requestInfo()` | Sends DISCOVER to this node; the reply arrives through `poll()`. |
| `bool hasInfo()`, `const Info& info()` | Latest INFO: firmware version, MAC, max channels and pixels, name. |
| `bool hasStatus()`, `const Status& status()`, `uint32_t statusAgeMs()` | Latest STATUS (receivers built with `DEBUG 1` send one a second) and its age. |

### `RemoteChannel`

| Member | What it does |
|---|---|
| `void setColorOrder(EOrder)`, `EOrder colorOrder()` | The byte order this strip expects. Default `RGB`. |
| `uint8_t channel()` | The receiver channel it drives. |

## Rules and limits

- **Declare the `Sender` before any `Node`.** Each `Node` registers with its sender when it is constructed, so the sender has to exist first. Global objects in one file are constructed in declaration order.
- **Start Ethernet before `sender.begin()`.**
- **Up to 8 nodes per sender and 32 channels per node** (`kMaxNodes`, `kMaxNodeChannels`).
- **No gamma.** Apply it to your pixels if you want it; Pixel Link carries the values you send.
- **Remote strips appear one receiver refresh later than local pins.** Local pins clock out during `show()`; the receiver starts clocking out when the SYNC arrives at the end of `show()`.
- **Requirements:** Teensy 4.1, QNEthernet, FastLED 3.10.x (tested against 3.10.8).

## FastLED version coupling

This header uses parts of FastLED that are not its documented sketch API, so a FastLED upgrade can break it:

- `fl::EngineEvents::Listener` with `onBeginFrame()` / `onEndFrame()`, and `fl::EngineEvents::addListener` / `removeListener`.
- `CPixelLEDController<RGB>` with `init()`, `showPixels(PixelController<RGB>&)` and `getMaxRefreshRate()` returning `fl::u16`.
- `PixelController::has()`, `loadAndScale0/1/2()`, `advanceData()`, `stepDithering()`.
- `FL_NO_EXCEPT` on overrides. The header defines it as empty if FastLED doesn't, for older versions.

Pin the FastLED version in projects that use remote pins, and test a new FastLED release with the `FastLEDSender` example before upgrading.
