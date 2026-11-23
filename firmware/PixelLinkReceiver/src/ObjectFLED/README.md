# ObjectFLED (vendored, patched)

A copy of [KurtMF/ObjectFLED](https://github.com/KurtMF/ObjectFLED) at commit
[`3ba7da2`](https://github.com/KurtMF/ObjectFLED/tree/3ba7da2) (release 1.1.0
plus README and example updates), MIT licensed (see the header of each file).
The Arduino IDE compiles it as part of the sketch, so a separately installed
ObjectFLED library is not used by PixelLinkReceiver.

## Changes from upstream

- `OjectFLED.cpp` is renamed `ObjectFLED.cpp`.
- The constructor's `numLEDs` and the `stripLen` member are `uint32_t`
  instead of `uint16_t`.
- The loop counters in `genFrameBuffer()` are `uint32_t` instead of
  `uint16_t`.

Upstream, these 16-bit types cap one ObjectFLED instance at 65,535 bytes
(21,845 RGB LEDs across all pins). Past that, the copy loop counter wraps and
`show()` never returns. With the patch, the total is limited only by memory.

## Limit that remains

Each strip is still capped at 4,095 bytes (1,365 RGB LEDs). The eDMA major
loop count (CITER/BITER) is a 15-bit field, and ObjectFLED sets it to bytes
per strip × 8. PixelLinkReceiver checks `MAX_PIXELS_PER_CHANNEL` against this
at compile time.
