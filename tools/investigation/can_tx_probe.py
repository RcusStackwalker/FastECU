#!/usr/bin/env python3
"""Throwaway diagnostic: transmit one CAN frame and watch what the adapter says.

Sends the ISO-TP single frame for UDS StartDiagnosticSession 0x81 on 0x7E0 --
byte for byte what fastecu-bench already sends, so nothing new is being asked
of the ECU and nothing destructive is involved.

The point is the ACK. A CAN transmitter needs at least one OTHER node to
acknowledge a frame at the bit level. So:

  TX confirmed (loopback / TX_DONE)  -> something on the bus ACKed us; the ECU
                                        is electrically present, and silence
                                        afterwards is an addressing/protocol
                                        problem
  TX never confirmed / error         -> nobody ACKed; the ECU is not on the bus,
                                        or CANH/CANL or termination is wrong

Usage: can_tx_probe.py
"""

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
REQUEST_ID = 0x7E0


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
    shown = reply.hex(" ")
    print(f"  {label:<16} -> {shown!r}")
    return reply


def main():
    fd = open_port()
    try:
        cmd(fd, b"ata\r\n", "open")
        cmd(fd, f"ato{CAN} 0 500000 {CAN}\r\n".encode(), "connect CAN")
        cmd(
            fd,
            f"atf{CAN} {PASS_FILTER} 0 4\r\n".encode(),
            "filter all",
            payload=b"\x00\x00\x00\x00" + b"\x00\x00\x00\x00",
        )

        # Raw CAN payload = 4-byte big-endian arbitration id, then 8 data bytes.
        # 02 10 81 -> ISO-TP single frame, length 2, UDS StartDiagnosticSession 0x81.
        frame = struct.pack(">I", REQUEST_ID) + bytes([0x02, 0x10, 0x81, 0, 0, 0, 0, 0])
        print(f"\n  transmitting id 0x{REQUEST_ID:03X} payload 02 10 81 (+pad)")
        cmd(
            fd,
            f"att{CAN} {len(frame)} 0\r\n".encode(),
            "transmit",
            wait=2.0,
            payload=frame,
        )

        print("\n  ... listening 3s for a reply ...")
        raw = drain(fd, 3.0)
        shown = raw.hex(" ")
        print(f"  post-tx traffic: {len(raw)} bytes {shown!r}")

        cmd(fd, f"atc{CAN}\r\n".encode(), "disconnect")
        cmd(fd, b"atz\r\n", "close")
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
