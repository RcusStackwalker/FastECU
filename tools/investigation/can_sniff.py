#!/usr/bin/env python3
"""Throwaway diagnostic: listen on the CAN bus through the OpenPort 2.0.

Opens a RAW CAN channel (not ISO-TP) at 500 kbit/s with an accept-everything
filter and reports which arbitration ids are seen. Purely receive-only: no
frame is transmitted, so this cannot disturb an ECU.

Answers one question: is there ANY traffic on the bus, and on which ids?
  - traffic seen        -> bus and wiring are good; the problem is addressing
  - nothing seen at all -> ECU silent at rest (normal for a bootloader) OR
                           the bus is not actually connected

Usage: can_sniff.py [seconds]
"""

import collections
import fcntl
import os
import struct
import sys
import termios
import time

PORT = "/dev/cu.usbmodemTApU_RJO1"
TIOCMBIS = 0x8004746C
TIOCM_DTR = 0x002
TIOCM_RTS = 0x004

CAN = 5
PASS_FILTER = 0x01


def open_port():
    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[4] = attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    fcntl.ioctl(fd, TIOCMBIS, struct.pack("I", TIOCM_DTR | TIOCM_RTS))
    termios.tcflush(fd, termios.TCIOFLUSH)
    time.sleep(0.4)
    return fd


def drain(fd, seconds):
    out = b""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.005)
            continue
        if chunk:
            out += chunk
    return out


def cmd(fd, command, label, wait=0.4, payload=b""):
    os.write(fd, command + payload)
    reply = drain(fd, wait)
    shown = reply.decode("ascii", "replace").replace("\r", "\\r").replace("\n", "\\n")
    print(f"  {label:<14} {command!r:<28} -> {shown!r}")
    return reply


def main():
    listen_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    fd = open_port()
    try:
        print(f"listening on {PORT}, raw CAN 500 kbit/s, {listen_seconds:.0f}s")
        cmd(fd, b"ata\r\n", "open")
        cmd(fd, b"ati\r\n", "version")
        cmd(fd, b"atr 16\r\n", "pin16 volts")
        # ato<proto> <flags> <baud> <proto>
        cmd(fd, f"ato{CAN} 0 500000 {CAN}\r\n".encode(), "connect CAN")
        # atf<chan> <type> <txflags> <size> + mask bytes + pattern bytes.
        # All-zero mask with all-zero pattern accepts every arbitration id.
        cmd(
            fd,
            f"atf{CAN} {PASS_FILTER} 0 4\r\n".encode(),
            "filter all",
            payload=b"\x00\x00\x00\x00" + b"\x00\x00\x00\x00",
        )

        print(f"\n  ... collecting for {listen_seconds:.0f}s ...\n")
        raw = drain(fd, listen_seconds)

        cmd(fd, f"atc{CAN}\r\n".encode(), "disconnect")
        cmd(fd, b"atz\r\n", "close")

        print(f"\nRESULT: {len(raw)} bytes of channel traffic")
        if not raw:
            print("  Nothing on the bus.")
            print("  A bootloader-mode ECU is normally silent until addressed,")
            print("  so this alone does not prove the bus is disconnected.")
            return 2

        # Frames arrive as "ar<chan> <len> <type>\r\n" + payload. Just count
        # the record headers and show the raw head; exact framing is secondary
        # to the yes/no question.
        headers = collections.Counter()
        for index in range(len(raw) - 2):
            if raw[index] == 0x61 and raw[index + 1] == 0x72:  # "ar"
                headers[raw[index + 2:index + 3]] += 1
        print(f"  record kinds seen: {dict(headers)}")
        print(f"  first 200 bytes: {raw[:200]!r}")
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
