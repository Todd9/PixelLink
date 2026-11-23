# Pixel Link previz for TouchDesigner

Your computer stands in for a Pixel Link receiver. A real sender (a Teensy
running one of the library examples, or your own sketch) sends it real Pixel
Link frames, and TouchDesigner draws every LED at its position in 3D, live.
It answers DISCOVER and sends STATUS like the Teensy receiver, so a sender's
status output works too and reports the node as `previz`.

This is the receiving side. A TouchDesigner *sender* is planned separately.

| File | What it is |
| --- | --- |
| `pl_core.py` | Receiver logic: frames, PUSH/SYNC/implicit presentation, INFO/STATUS replies, layout. Plain Python + numpy, testable outside TouchDesigner (`tests/touchdesigner`). |
| `udpin_callbacks.py` | Callbacks for the UDP In DAT |
| `script_chop_callbacks.py` | Callbacks for the Script CHOP that feeds instancing |
| `layout.csv` | Example layout: `channel, pixel, feature, x, y, z` (inches, Z up). Four rows of 150 LEDs on channels 0–3, matching the `FastLEDSender` example. |

## 1. Network

- Set your computer's Ethernet adapter to the receiver's address the sender
  expects, e.g. IP 192.168.177.111, mask 255.255.255.0, gateway blank.
- Connect the sending Teensy directly to that adapter.
- Let TouchDesigner through the firewall (UDP port 5150) when asked.

## 2. TouchDesigner network

Put all operators side by side, e.g. in one Base COMP.

1. **Text DAT** named `pl_core`: paste in `pl_core.py`.
2. **File In DAT** named `layout`: File `layout.csv` (it loads as a table).
3. **Text DAT** named `udpin_callbacks`: paste in `udpin_callbacks.py`.
4. **UDP In DAT**: Port 5150, Row/Callback Format *One Per Message*, Maximum
   Lines 1 (the table isn't used), Local Address 192.168.177.111 (optional;
   helps with several adapters), Callbacks DAT `udpin_callbacks`.
5. **Text DAT** named `pixels_callbacks`: paste in `script_chop_callbacks.py`.
6. **Script CHOP** named `pixels`: Callbacks DAT `pixels_callbacks`. It shows
   one sample per layout row, with channels `tx ty tz r g b scale`.
7. **Geometry COMP**: inside, a low-resolution Sphere SOP as the rendered SOP.
   Instancing on, Instance OP `pixels`; Translate X/Y/Z `tx ty tz`, Scale
   X/Y/Z `scale`, Color R/G/B `r g b`. Material: a white Constant MAT (the
   instance color tints it).
8. **Camera COMP** (scene units are meters) pointed at the layout, and a
   **Render TOP** with the Geometry COMP and camera. A Bloom TOP after it
   makes the LEDs glow.

## 3. Run

Flash a sender and open its Serial Monitor. `FastLEDSender` prints

```
receiver: "previz" fw 1.0.0, 32 channels x 972 px max
```

and, every 2 s, a receiver status line with the previz frame rate.

## Notes

- **Your own layout:** one row per LED with its receiver channel and pixel
  index. Rows with channel `-1` (LEDs that aren't sent over the network, such
  as a sender's local pins) stay dark. The previz picks up a changed file
  automatically. Instance sizes per `feature` name are in `SIZES` in
  `script_chop_callbacks.py`.
- **Brightness:** the previz shows what's sent, including the sender's
  brightness. If it looks dim, raise `GAIN` in `script_chop_callbacks.py`.
- **Previz and real receiver at once:** put both on a switch, give the
  computer another address (e.g. .112), and add a second `Node` for it in the
  sender. Every node that gets data receives the SYNC.
