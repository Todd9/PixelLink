# Pixel Link Protocol Spec

Protocol version 1. The reference implementation is `library/PixelLink/src/PixelLink.h`.

## Overview

Pixel Link is a minimal UDP protocol for sending raw pixel data to Teensy 4.1 LED receivers, one packet per output channel. Receivers configure themselves from the data they get: no config file, no universe math.

- **Transport:** UDP over IPv4, default port 5150 (configurable). Payloads never exceed 1,472 bytes, so packets fit a standard 1,500-byte MTU without IP fragmentation.
- **Byte order:** little-endian for all multi-byte fields (native on the Teensy and on PCs).
- **Pixel data:** 3 bytes per pixel, already in the order the strip expects (R, G, B for an RGB strip; G, R, B for a GRB strip). The sender applies color order, gamma, brightness and color correction. The receiver passes bytes through unchanged.
- **Senders:** a Teensy (with `PixelLinkFastLED.h`, or building packets by hand with `PixelLink.h`), or a computer (a TouchDesigner sender is planned).
- **Topologies:** a direct cable to one receiver, or several receivers on a switch with frame-synchronized output.

**Why not an existing protocol:** Art-Net and sACN carry 170-pixel universes that don't line up with strings, and frame sync is bolted on. DDP is close to this design, but it addresses one flat byte buffer per device, so a receiver can't infer per-channel string lengths. Pixel Link addresses channels directly, which is what lets the receiver size its output automatically.

## Packet format

Every packet starts with the same 4-byte header. Receivers silently drop packets with a wrong magic, an unknown version, or an unknown type.

**Common header**

| Bytes | Field | Type | Value |
|---|---|---|---|
| 0–1 | Magic | char[2] | `P` `X` (0x50 0x58) |
| 2 | Version | uint8 | 1 |
| 3 | Type | uint8 | see packet types |

**Packet types**

| Type | Name | Direction | Purpose |
|---|---|---|---|
| 0x01 | PIXELS | sender → receiver | Pixel data for one channel (or part of one) |
| 0x02 | SYNC | sender → receiver(s), often broadcast | Show the frame now |
| 0x03 | DISCOVER | sender → broadcast | Ask receivers to identify themselves |
| 0x04 | INFO | receiver → sender | Reply to DISCOVER |
| 0x05 | STATUS | receiver → sender | Periodic health report (debug mode only) |

**PIXELS (0x01)** — 12-byte header, then pixel data. Max 486 pixels per packet ((1,472 − 12) / 3).

| Bytes | Field | Type | Notes |
|---|---|---|---|
| 0–3 | Common header |  | Type = 0x01 |
| 4–5 | Frame | uint16 | Frame number, increments by 1 per frame, wraps |
| 6 | Channel | uint8 | Output channel, 0-based (0–31 on the reference receiver) |
| 7 | Flags | uint8 | bit 0 = PUSH; bits 1–2 = pixel format (00 = RGB; 01 = RGBW, reserved); other bits 0 |
| 8–9 | Offset | uint16 | First pixel index within the channel (0 unless a channel spans packets) |
| 10–11 | Count | uint16 | Number of pixels in this packet |
| 12… | Data | uint8[Count × 3] | 3 bytes per pixel, in the strip's color order |

**SYNC (0x02)** — 8 bytes.

| Bytes | Field | Type | Notes |
|---|---|---|---|
| 0–3 | Common header |  | Type = 0x02 |
| 4–5 | Frame | uint16 | Frame to show |
| 6–7 | Reserved | uint16 | 0 |

**DISCOVER (0x03)** — the 4-byte common header only.

**INFO (0x04)** — 34 bytes. The receiver's IP is the packet's source address.

| Bytes | Field | Type | Notes |
|---|---|---|---|
| 0–3 | Common header |  | Type = 0x04 |
| 4–6 | Firmware version | uint8[3] | major, minor, patch |
| 7 | Reserved | uint8 | 0 |
| 8–13 | MAC | uint8[6] |  |
| 14 | Max channels | uint8 | e.g. 32 |
| 15 | Reserved | uint8 | 0 |
| 16–17 | Max pixels per channel | uint16 | e.g. 972 |
| 18–33 | Node name | char[16] | ASCII, zero-padded; a 16-character name has no terminator |

**STATUS (0x05)** — 32 bytes, sent about once per second in debug mode.

| Bytes | Field | Type | Notes |
|---|---|---|---|
| 0–3 | Common header |  | Type = 0x05 |
| 4–7 | Uptime | uint32 | seconds |
| 8–9 | Frame rate | uint16 | frames shown per second × 10 |
| 10–11 | Output length | uint16 | pixels currently clocked out per pin |
| 12 | Active channels | uint8 | channels that received data in the last second |
| 13 | State | uint8 | 0 receiving, 1 holding, 2 fading, 3 dark, 4 test |
| 14–15 | CPU temperature | int16 | °C × 10 |
| 16–19 | Frames shown | uint32 | since boot |
| 20–23 | Implicit frames | uint32 | frames shown without PUSH or SYNC |
| 24–27 | Bad packets | uint32 | malformed or out of range |
| 28–31 | Packets received | uint32 | since boot |

## Frame semantics

A frame is a set of PIXELS packets sharing one frame number, shown all at once. The receiver writes incoming data into a back buffer and presents it on one of three triggers.

**Frame numbers** increment by 1 per frame and wrap at 65,535. "Newer" is decided by signed difference, so wraparound is safe: frame A is newer than B when `(int16_t)(A − B) > 0`. PIXELS packets up to 59 frames older than the current one are late arrivals and are dropped. Anything older is treated as a sender restart and accepted as the new current frame.

**Presentation triggers**, in order of preference:

1. **SYNC** — a SYNC whose frame is the current frame or newer shows the back buffer, if anything was written since the last show. This is the multi-receiver path.
2. **PUSH** — a PIXELS packet with the PUSH flag set shows the frame right after that packet's data is written. This is the simple single-receiver path: set PUSH on the frame's last packet.
3. **Implicit** — a PIXELS packet with a newer frame number arrives while the current frame is unshown. The receiver shows the current frame first, then starts the new one, and counts it in STATUS as an implicit frame. This keeps a sender that forgets PUSH and SYNC working, one frame late.

**Partial frames:** the back buffer persists. A channel not sent in a frame keeps its previous pixels, so a lost packet shows one channel one frame stale instead of a black gap.

**Latest wins:** if a trigger arrives while the previous frame is still clocking out, the new frame waits and is shown as soon as output finishes. A still newer frame replaces it, so frames may be skipped but never queue up.

**Channels spanning packets:** a channel longer than 486 pixels is sent as several PIXELS packets with increasing Offset. A channel's length in a frame is its largest Offset + Count.

## Receiver behavior

This describes the reference receiver, `firmware/PixelLinkReceiver`. It sizes its output to the data it receives, holds through dropouts, and needs no configuration beyond its network settings.

**Auto geometry**

- Memory is allocated for the worst case: 32 channels × 972 pixels (two packets each), about 93 KB of pixel data.
- Output length L is the longest channel length seen in a frame. All pins clock out L pixels in parallel, so refresh time is about 30 µs × L plus the latch: roughly 68 fps at 486 pixels and 34 fps at 972.
- **Grow:** if a frame needs more than L, L grows before that frame is shown, so nothing is cut off.
- **Shrink:** L shrinks only after 2 seconds (fixed) of frames that all fit in a smaller length, so one odd frame causes no churn.
- All 32 channels can run at the full 972 px. The receiver uses a patched ObjectFLED with a 32-bit LED count (upstream caps one driver at 21,845 LEDs). Changing L rebuilds the ObjectFLED driver. This happens only when geometry changes, never per frame.
- Strings shorter than L are fine: their extra data runs past the last LED. Pixels beyond a channel's new length after a shrink stop updating and keep their last color.
- Only channels that receive data are driven. The driven set grows at once and drops channels after the same 2 s; a dropped channel keeps its last colors. Channels never addressed are not driven at all.

**Refresh:** when nothing new arrives (holding, test pattern, dark), the receiver resends the current frame at 40 Hz. Some LED chips (e.g. GS8206) start their own built-in test pattern when their data line goes quiet; this keeps the line busy.

**Link loss**

- After HOLD_TIME (default 10 s) with no valid PIXELS packets, the receiver fades the last frame to black over FADE_TIME (default 3 s), then stays dark.
- Valid data at any point returns it to normal output immediately.
- The fade is the only time the receiver changes pixel values.

**Test mode** (TEST_MODE setting)

- **ALWAYS:** runs the test sequence at every boot, then waits for data.
- **IF_NO_SIGNAL:** if no data arrives within 5 s of boot, loops the test until data arrives.
- **NEVER:** boots dark.
- The test runs at TEST_BRIGHTNESS to limit power draw. It shows an RGB sweep, then identifies each channel by lighting its first N+1 pixels (channel 0 lights 1, channel 1 lights 2…), which makes miswired jacks easy to spot from the ground.

**Debug mode** (DEBUG setting)

- Sends a STATUS packet about once per second to the address and port of the most recent PIXELS sender.
- Enables serial logging. With DEBUG off, nothing is printed in the data path.

**Discovery:** always on. A DISCOVER packet gets an INFO reply sent to its source.

**Firmware updates:** over Ethernet on a separate TCP port (default 5151), using FlasherX. A failed upload leaves the old firmware running. See [firmware-updates.md](firmware-updates.md).

**Watchdog:** a hardware watchdog reboots the receiver if the main loop stalls.

## Sender guidance

A sender sends one PIXELS packet per channel per frame, then triggers the show with PUSH (one receiver) or SYNC (several).

**One receiver (direct cable)**

1. Increment the frame number.
2. Send a PIXELS packet for each channel in use.
3. Set the PUSH flag on the last packet.

**Several receivers (switch)**

1. Increment the frame number (one counter shared across all receivers).
2. Unicast each receiver its PIXELS packets, without PUSH.
3. Send one SYNC for that frame to each receiver (or broadcast it), so every receiver shows it at the same moment.

**Pacing**

- Don't send frames faster than the receiver can show them. Its refresh rate follows from Output length in STATUS (about 1 / (30 µs × L + latch)). Extra frames are skipped, not queued, but they waste bandwidth and receive buffers.
- A full frame to one receiver is up to 32 packets, about 47 KB. If a computer sender sees implicit or skipped frames in STATUS, space the packets slightly within the frame.
- Never exceed 1,472-byte payloads. Split long channels with Offset instead of relying on IP fragmentation or jumbo frames.

**FastLED senders (`PixelLinkFastLED.h`)**

- Each remote channel is a FastLED controller, added with `FastLED.addLeds(node.channel(n), leds, count)` next to local pins.
- One frame number per `FastLED.show()`; after all controllers run, a SYNC goes to every node that got data. This holds for one node or many.
- Each channel reports the receiver's sustainable frame rate for its length, so FastLED paces `show()` to the longest remote string (about 194 fps at 146 px).
- Details: [fastled-remote-pins.md](fastled-remote-pins.md).

**Computer senders (TouchDesigner, planned)**

- Same rules as a Teensy sender: one channel per packet, PUSH or SYNC, frame numbers that wrap.
- Use DISCOVER/INFO to list receivers on the network by name, IP, and channel capacity.
- Color order, gamma, brightness, and color correction are the sender's job; the receiver shows raw values.

## Reference receiver settings

All receiver settings are compile-time constants in one clearly marked block at the top of `PixelLinkReceiver.ino`.

| Setting | Default | Notes |
|---|---|---|
| NODE_NAME | "remote" | Up to 15 characters, reported in INFO |
| IP_ADDRESS | 192.168.177.111 | Static by default |
| USE_DHCP | off | When on, falls back to IP_ADDRESS if DHCP fails |
| DATA_PORT | 5150 | UDP: PIXELS, SYNC, DISCOVER |
| UPDATE_PORT | 5151 | TCP: firmware upload |
| PIN_LIST | 32 board pins | Channel order = jack order on the 32-output receiver board; pin 29 is not used on that board |
| COLOR_ORDER | CORDER_RGB | Pass-through: the sender orders each channel's bytes |
| MAX_PIXELS_PER_CHANNEL | 972 | Two packets per channel |
| HOLD_TIME_MS | 10000 | Hold last frame after signal loss |
| FADE_TIME_MS | 3000 | Fade to black after the hold |
| TEST_MODE | IF_NO_SIGNAL | ALWAYS, IF_NO_SIGNAL, or NEVER |
| TEST_BRIGHTNESS | 64 (25%) | Limits power during the test |
| DEBUG | off | STATUS packets and serial logging |

The shrink delay (2 s), the no-signal test timeout (5 s) and the 40 Hz refresh are fixed constants, not settings.

## Open questions

- Confirm or change the default ports 5150/5151.
- Confirm on hardware that the receiver's 64-packet UDP queue holds a full 32-channel burst with two packets per channel.
- RGBW: the format bits are reserved; implement only if a project needs it.
