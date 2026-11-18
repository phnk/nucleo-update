#!/usr/bin/env python3
"""Send the app (build/update.bin) to the loader (main.cpp) over the ST-LINK USB
serial port. The loader swaps it in while the board keeps running.

usage: python3 upload.py FILE.bin [/dev/ttyACM0]
needs: sudo apt install python3-serial
"""
import struct
import sys
import time
import zlib

import serial

SYNC, ACK, NACK = b"U", 0x06, 0x15
CHUNK = 256


def wait_reply(port, timeout):
    """Wait for ACK/NACK, skipping the board's normal text output."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        b = port.read(1)
        if b and b[0] == ACK:
            return True
        if b and b[0] == NACK:
            return False
    return None


def fail(msg):
    sys.exit(f"\nerror: {msg}")


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    path = sys.argv[1]
    device = sys.argv[2] if len(sys.argv) == 3 else "/dev/ttyACM0"
    data = open(path, "rb").read()

    try:
        port = serial.Serial(device, 115200, timeout=0.02, exclusive=True)
    except serial.SerialException as e:
        fail(f"can't open {device}: {e}\n(is picocom still running? close it first)")

    with port:
        port.reset_input_buffer()
        print(f"{path}: {len(data)} bytes")

        # 1. get the board's attention
        start = time.monotonic()
        while True:
            if time.monotonic() - start > 5:
                fail("no answer from the board (did you `make flash`? is picocom closed?)")
            port.write(SYNC)
            if wait_reply(port, 0.1):
                break

        # 2. length + checksum
        time.sleep(0.05)
        port.write(struct.pack("<II", len(data), zlib.crc32(data)))
        reply = wait_reply(port, 10)
        if reply is None:
            fail("board stopped answering")
        if not reply:
            fail("board refused the upload (file too big?)")

        # 3. the data
        for offset in range(0, len(data), CHUNK):
            port.write(data[offset:offset + CHUNK])
            reply = wait_reply(port, 5)
            if not reply:
                fail(f"board {'rejected' if reply is False else 'stopped answering at'} byte {offset}")
            print(f"\r  sent {min(offset + CHUNK, len(data))}/{len(data)}", end="", flush=True)
        print()

        # 4. the board checks the checksum and answers once more
        reply = wait_reply(port, 5)
        if not reply:
            fail("checksum mismatch or invalid file, board kept the old app")
        print("upload OK. board says:")

        # show what the board prints next
        end = time.monotonic() + 1.5
        while time.monotonic() < end:
            text = port.read(256)
            if text:
                sys.stdout.write(text.decode(errors="replace").replace("\r", ""))
                sys.stdout.flush()


if __name__ == "__main__":
    main()
