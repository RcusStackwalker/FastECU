#!/usr/bin/env python3
"""Throwaway probe: ask the OpenPort 2.0 for its pin-16 voltage.

Speaks the raw Tactrix serial protocol directly, the same three commands
FastECU's own J2534 layer uses:
    ata\r\n      open device        -> "ari ..." / "ar..."
    ati\r\n      read version       -> "ari 1.17.4877"
    atr 16\r\n   read pin 16 volts  -> "arr 16 <millivolts>"
    atz\r\n      close device

Read-only. Nothing here touches a channel, a filter, or the CAN bus.
"""

import fcntl
import os
import struct
import sys
import termios
import time

# macOS ioctl numbers; QSerialPort asserts both lines on open and the
# OpenPort firmware appears to need them.
TIOCMBIS = 0x8004746C
TIOCM_DTR = 0x002
TIOCM_RTS = 0x004

PORT = "/dev/cu.usbmodemTApU_RJO1"


def drain(fd, seconds=0.4):
    """Collect whatever arrives within `seconds`, without blocking forever."""
    out = b""
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        if chunk:
            out += chunk
            deadline = time.time() + seconds  # silence timeout, like the real reader
    return out


def send(fd, command, label):
    os.write(fd, command)
    reply = drain(fd)
    printable = reply.decode("ascii", "replace").replace("\r", "\\r").replace("\n", "\\n")
    print(f"  {label:<12} {command!r:<14} -> {printable!r}")
    return reply


def main():
    if not os.path.exists(PORT):
        print(f"FAIL: {PORT} not present")
        return 1

    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = attrs[1] = attrs[3] = 0  # iflag, oflag, lflag: raw
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[4] = attrs[5] = termios.B4800  # ispeed/ospeed; CDC ignores it but be explicit
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        fcntl.ioctl(fd, TIOCMBIS, struct.pack("I", TIOCM_DTR | TIOCM_RTS))
        termios.tcflush(fd, termios.TCIOFLUSH)
        time.sleep(0.5)  # let the CDC endpoint settle after open + DTR

        print(f"probing {PORT}")
        send(fd, b"\r\n", "wake")
        send(fd, b"ata\r\n", "open")
        send(fd, b"ati\r\n", "version")
        vbatt = send(fd, b"atr 16\r\n", "pin16 volts")
        send(fd, b"atz\r\n", "close")

        text = vbatt.decode("ascii", "replace")
        millivolts = None
        for token in text.replace("\r", " ").replace("\n", " ").split():
            if token.isdigit():
                millivolts = int(token)
        print()
        if millivolts is None or millivolts <= 1:
            print(f"RESULT: no usable voltage reading (raw {text!r})")
            print("        -> pin 16 is not powered, or the adapter did not answer")
            return 2
        print(f"RESULT: pin 16 = {millivolts} mV ({millivolts / 1000:.3f} V)")
        if millivolts < 12000:
            print("        -> below 12 V; the bench checklist treats that as disqualifying")
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
