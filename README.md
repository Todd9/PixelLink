# Pixel Link

Pixel Link is a small UDP protocol for sending raw pixel data over Ethernet to Teensy 4.1 LED receivers, one packet per output channel. Receivers size themselves from the data they get: no config file and no universe math. Several receivers on a switch show each frame together.

This repo has the protocol library, a reference receiver, sender examples, and a FastLED extension that makes a receiver's outputs behave like local FastLED pins.

```
 sender (Teensy 4.1 + FastLED,        ──Ethernet / UDP 5150──▶  receiver (Teensy 4.1)
  or a computer)                          Pixel Link              up to 32 LED outputs
```

## What's here

| Folder | What it is |
| --- | --- |
| [`library/PixelLink`](library/PixelLink) | Arduino library. `PixelLink.h` is the protocol (plain C++, any platform). `PixelLinkFastLED.h` adds FastLED "remote pins". Examples: `RawSender`, `FastLEDSender`. |
| [`firmware/PixelLinkReceiver`](firmware/PixelLinkReceiver) | Reference receiver for Teensy 4.1: 32 parallel outputs, auto-sizing, hold and fade, test pattern, firmware updates over Ethernet. |
| [`touchdesigner/PixelLinkPreviz`](touchdesigner/PixelLinkPreviz) | TouchDesigner previz: the computer stands in for a receiver and draws every LED in 3D. |
| [`docs`](docs) | [Protocol spec](docs/spec.md), [FastLED remote pins](docs/fastled-remote-pins.md), [firmware updates](docs/firmware-updates.md). |
| [`tests`](tests) | Desktop tests for the protocol, receiver and firmware updates, and the Python previz receiver. |

## Getting started

1. Install the library: copy or link `library/PixelLink` into your `Arduino/libraries` folder. Install QNEthernet and, for remote pins, FastLED 3.10.x from the Library Manager.
2. Flash `firmware/PixelLinkReceiver` to the receiver Teensy (see its [README](firmware/PixelLinkReceiver/README.md) for the other libraries it needs). Default address 192.168.177.111.
3. Connect the sender Teensy with an Ethernet cable and flash an example: File > Examples > PixelLink > `FastLEDSender` (FastLED, local and remote strips) or `RawSender` (packets by hand, no FastLED).

With no data within 5 s of boot, the receiver runs its test pattern, which is a quick way to check its wiring before any sender is connected.

## Versions

| Part | Version |
| --- | --- |
| Protocol | 1 |
| Library | 1.0.0 |
| Receiver firmware | 1.1.4 |

## Roadmap

- A TouchDesigner Pixel Link sender.
- Backward compatibility with sACN (E1.31), possibly as a separate receiver firmware.
- Hardware design files for the receiver board, and improvements to it.

## Notes

Pixel limit per pin is 972, two packets' worth of pixels, a design choice rather than a hardware limit. Each packet carries 486 pixels, the most that fits in one unfragmented UDP packet on standard Ethernet. The receiver allows two packets per channel.

It could go up to ObjectFLED's ceiling of 1,365 pixels, but at a cost. Frame rate would drop from about 34 to 24 fps at full length. RAM use would be tight, and the receive queue would need enlarging.

## License

MIT. See [LICENSE](LICENSE). FlasherX and ObjectFLED are separate projects under their own licenses and are not included.
