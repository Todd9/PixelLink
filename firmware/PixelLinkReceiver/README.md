# PixelLinkReceiver

Reference Pixel Link receiver for Teensy 4.1. It shows whatever arrives over Pixel Link on up to 32 LED outputs in parallel (ObjectFLED), sizes its output from the data it receives, holds and fades on signal loss, runs a test pattern when nothing arrives, and takes firmware updates over Ethernet.

Firmware 1.1.4. Behavior is described in [docs/spec.md](../../docs/spec.md#receiver-behavior).

## Building

Arduino IDE or arduino-cli with Teensyduino, board **Teensy 4.1**.

| Library | Where |
| --- | --- |
| PixelLink | `library/PixelLink` in this repo, copied or linked into `Arduino/libraries/PixelLink` |
| QNEthernet | Library Manager |
| ObjectFLED | [github.com/KurtMF/ObjectFLED](https://github.com/KurtMF/ObjectFLED) |
| Watchdog_t4 | bundled with Teensyduino |
| FlasherX | copy `FlashTxx.c` and `FlashTxx.h` from [github.com/joepasquariello/FlasherX](https://github.com/joepasquariello/FlasherX) into this folder |

## Settings

All settings are in the `SETTINGS` block at the top of `PixelLinkReceiver.ino`; the table in [the spec](../../docs/spec.md#reference-receiver-settings) lists them. The ones you're most likely to change:

- `IP_ADDRESS` (default 192.168.177.111) and `NODE_NAME`, once per receiver.
- `PIN_LIST`: one entry per channel in jack order. The default matches the 32-output receiver board (pin 29 unused). For other wiring, list the pins you use; channel numbers follow the list.
- `DEBUG 1` to send STATUS packets to the sender once a second and log to USB serial.

Color order is not a receiver setting: the sender puts each channel's bytes in the order its strip expects.

## Updating over Ethernet

See [docs/firmware-updates.md](../../docs/firmware-updates.md). In short: Sketch > Export Compiled Binary, then

```
python3 extras/pixellink_upload.py 192.168.177.111 PixelLinkReceiver.ino.hex
```

## Limits

- For 60 fps, keep the longest string at about 530 pixels or less.
- Up to 972 pixels per channel (two packets). Channels × output length must stay at or under 21,845 LEDs (ObjectFLED's 16-bit index); over that the output length is clamped, e.g. to 682 with all 32 channels.

## Tests

`tests/host/receiver_test.cpp` runs this sketch on the desktop with stand-ins for the hardware. See [tests/README.md](../../tests/README.md).
