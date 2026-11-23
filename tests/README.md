# Tests

Desktop tests, no boards needed.

## C++ (`tests/host`)

Needs g++ with C++17.

```
cd tests/host
make test
```

| Test | What it covers |
| --- | --- |
| `protocol_test.cpp` | Every builder and parser in `PixelLink.h` against the byte layout in `docs/spec.md`; frame-number wraparound |
| `receiver_test.cpp` | The real `PixelLinkReceiver.ino` on a fake clock: boot, test pattern, PUSH/SYNC/implicit frames, grow and shrink, the ObjectFLED limit, late packets, sender restart, bad packets, DISCOVER/INFO, hold, fade and dark |
| `ota_test.cpp` | `PixelLinkOta.h` against a fake flash: good upload, bad checksum, missing `FLASH_ID`, wrong base address, `CANCEL`, timeout |

`stubs/` holds minimal stand-ins for the Teensy headers the firmware includes (`Arduino.h`, `QNEthernet.h`, `ObjectFLED.h`, `Watchdog_t4.h`, `FlashTxx.h`).

`PixelLinkFastLED.h` isn't covered here yet: it needs FastLED built for the host. Check it with the `FastLEDSender` example on a Teensy.

## Python (`tests/touchdesigner`)

Needs numpy.

```
python3 -m unittest discover tests/touchdesigner
```

Covers `touchdesigner/PixelLinkPreviz/pl_core.py`, the Python receiver used by the TouchDesigner previz: PUSH, SYNC, implicit frames, offsets, late packets, wraparound, bad packets, INFO and STATUS layout.
