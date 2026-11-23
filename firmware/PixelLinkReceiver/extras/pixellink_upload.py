#!/usr/bin/env python3
"""Send a firmware .hex file to a Pixel Link receiver over Ethernet.

Usage:
    python pixellink_upload.py <receiver-ip> <firmware.hex> [--yes]

Get the .hex from the Arduino IDE with Sketch > Export Compiled Binary.
It lands in the sketch folder (or a build/ subfolder) as
PixelLinkReceiver.ino.hex.

The receiver checks the image before overwriting anything. If any check
fails, or the connection drops before you confirm, the running firmware
is left untouched. Needs only the Python standard library.
"""

import argparse
import socket
import struct
import sys
import time

UPDATE_PORT = 5151
DATA_PORT = 5150


def read_hex(path):
    with open(path, "r", encoding="ascii", errors="strict") as f:
        lines = [ln.strip() for ln in f if ln.strip()]
    if not lines or any(not ln.startswith(":") for ln in lines):
        sys.exit(f"{path} doesn't look like an Intel HEX file.")
    if lines[-1].upper() != ":00000001FF":
        sys.exit(f"{path} has no end-of-file record; it may be incomplete.")
    return lines


def read_reply(sock):
    """Reads one LF-terminated line from the receiver."""
    data = b""
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk
    return data.decode("ascii", "replace").strip()


def query_info(ip, timeout=1.0):
    """Sends DISCOVER and returns (name, version) from the INFO reply, or None."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(b"PX\x01\x03", (ip, DATA_PORT))
        data, _ = s.recvfrom(64)
    except (socket.timeout, OSError):
        return None
    finally:
        s.close()
    if len(data) < 34 or data[:4] != b"PX\x01\x04":
        return None
    major, minor, patch = struct.unpack_from("BBB", data, 4)
    name = data[18:34].split(b"\0", 1)[0].decode("ascii", "replace")
    return name, f"{major}.{minor}.{patch}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ip", help="receiver IP address, e.g. 192.168.177.111")
    ap.add_argument("hexfile", help="firmware .hex file")
    ap.add_argument("--yes", action="store_true", help="install without asking")
    ap.add_argument("--port", type=int, default=UPDATE_PORT)
    args = ap.parse_args()

    lines = read_hex(args.hexfile)
    payload = ("\n".join(lines) + "\n").encode("ascii")

    before = query_info(args.ip)
    if before:
        print(f"Receiver: \"{before[0]}\" running firmware {before[1]}")
    else:
        print("Receiver didn't answer DISCOVER (continuing anyway).")

    print(f"Connecting to {args.ip}:{args.port} ...")
    try:
        sock = socket.create_connection((args.ip, args.port), timeout=10)
    except OSError as e:
        sys.exit(f"Couldn't connect: {e}")
    sock.settimeout(30)

    # Send in chunks with a progress readout.
    total = len(payload)
    sent = 0
    chunk = 4096
    shown = -1
    try:
        while sent < total:
            sock.sendall(payload[sent:sent + chunk])
            sent = min(sent + chunk, total)
            pct = 100 * sent // total
            if pct != shown:
                shown = pct
                print(f"\rSending {len(lines)} lines: {pct:3d}%", end="", flush=True)
    except OSError:
        print()
        reply = ""
        try:
            reply = read_reply(sock)
        except OSError:
            pass
        sys.exit(f"Upload stopped: {reply or 'connection lost'}")
    print()

    print("Waiting for the receiver to check the image ...")
    reply = read_reply(sock)
    if not reply.startswith("READY"):
        sys.exit(f"Receiver refused the image: {reply or 'no reply'}")

    _, n_lines, n_bytes = reply.split()
    if int(n_lines) != len(lines):
        sock.sendall(b"CANCEL\n")
        sys.exit(f"Line count mismatch (sent {len(lines)}, receiver got {n_lines}). Cancelled.")
    print(f"Image OK: {n_lines} lines, {int(n_bytes):,} bytes.")

    if not args.yes:
        answer = input("Install it now? The receiver will reboot. [y/N] ").strip().lower()
        if answer != "y":
            sock.sendall(b"CANCEL\n")
            sys.exit("Cancelled. The running firmware is unchanged.")

    sock.sendall(b"GO\n")
    reply = read_reply(sock)
    sock.close()
    if reply != "FLASHING":
        sys.exit(f"Unexpected reply: {reply or 'none'}")

    print("Installing. Don't power off the receiver.")
    start = time.time()
    time.sleep(3)
    while time.time() - start < 60:
        after = query_info(args.ip)
        if after:
            print(f"Done in {time.time() - start:.0f} s: \"{after[0]}\" now running firmware {after[1]}")
            return
        time.sleep(1)
    print("No answer after 60 s. Check the receiver; it may still be finishing.")


if __name__ == "__main__":
    main()
