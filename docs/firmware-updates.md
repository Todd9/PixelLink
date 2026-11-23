# Firmware updates over Ethernet

The reference receiver can be updated over the network, so a receiver mounted out of reach never needs a USB cable. The update server is `firmware/PixelLinkReceiver/PixelLinkOta.h`; the computer side is `firmware/PixelLinkReceiver/extras/pixellink_upload.py`.

## Setup (once, when building the receiver)

The receiver uses [FlasherX](https://github.com/joepasquariello/FlasherX) to write the new image into spare flash and install it. FlasherX isn't an Arduino library: copy `FlashTxx.c` and `FlashTxx.h` from that repository into `firmware/PixelLinkReceiver/` next to the sketch. They are not included in this repo.

The first install is over USB, as usual. Every later update can go over Ethernet.

## Updating a receiver

1. In the Arduino IDE, open `PixelLinkReceiver.ino` and choose **Sketch > Export Compiled Binary**. The `.hex` lands in the sketch folder (or a `build/` subfolder) as `PixelLinkReceiver.ino.hex`.
2. On a computer on the same network as the receiver:

   ```
   python3 pixellink_upload.py 192.168.177.111 PixelLinkReceiver.ino.hex
   ```

   It needs only the Python standard library. Add `--yes` to install without the confirmation prompt, or `--port` if the receiver's `UPDATE_PORT` isn't 5151.

The tool prints the receiver's name and current firmware version (from DISCOVER/INFO), sends the image, waits for the receiver to check it, asks before installing, and then waits for the receiver to come back and report its new version.

LEDs hold the frame they're showing for the whole update. Pixel data that arrives meanwhile is read and discarded.

## Session protocol

TCP, default port 5151. One line per message, LF-terminated.

1. The client connects and sends the `.hex` file line by line.
2. After the EOF record the node checks the image and replies `READY <lines> <bytes>` or `ERROR <reason>`.
3. The client compares `<lines>` with its own count, then sends `GO` or `CANCEL`.
4. On `GO` the node replies `FLASHING`, installs the image, and reboots.

A second client that connects while a session is running gets `ERROR busy`.

## Checks before anything is overwritten

- Every HEX record's checksum.
- The image starts at the flash base and fits in the buffer.
- The image contains the FlasherX `FLASH_ID` string, which only firmware built with `FlashTxx` has. A sketch without update support (one of the sender examples, for example) is rejected, so an update can never remove update support.
- The line count the node received matches the client's.

Any error, disconnect, or 10 s of silence before `GO` cancels the update, erases the buffer, and leaves the running firmware untouched.

| Reply | Meaning |
|---|---|
| `ERROR bad hex record` | A line isn't valid Intel HEX or its checksum is wrong |
| `ERROR address out of range` | A record falls outside the flash buffer |
| `ERROR image does not start at flash base` | Not a Teensy 4.1 application image |
| `ERROR not OTA-capable firmware (no FLASH_ID)` | The sketch wasn't built with FlasherX |
| `ERROR line too long`, `ERROR unknown record type` | Malformed file |
| `ERROR flash write failed`, `ERROR no buffer` | Flash problem on the receiver |
| `ERROR timeout`, `ERROR cancelled`, `ERROR expected GO` | Session ended without installing |

## Watchdog during install

Installing can take several seconds with no chance to feed the watchdog. Right before `flash_move()`, the receiver raises the watchdog timeout to its maximum (128 s) and stops Ethernet.

## Adding updates to your own firmware

`PixelLinkOta.h` is self-contained. Construct `pixellink::OtaUpdater ota{5151}`, call `ota.begin(feed, onStart, onFlash, onEnd)` after Ethernet is up, call `ota.poll()` every loop, and skip your normal work while `ota.active()`. The hooks are documented at the top of the header; `PixelLinkReceiver.ino` shows one way to fill them in. Keep a `FLASH_ID` reference in the image (see `FIRMWARE_ID` in the receiver) so the new firmware can itself be updated later. A future sACN receiver firmware can reuse it unchanged.

## Tests

`tests/host/ota_test.cpp` runs `OtaUpdater` against a fake flash and scripted TCP sessions: a good upload, each rejection, `CANCEL` and the timeout.
