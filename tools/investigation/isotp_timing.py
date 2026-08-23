#!/usr/bin/env python3
"""Throwaway diagnostic: where does the per-frame dead time come from?

Drives a 192-byte UDS ReadMemoryByAddress over RAW CAN, hand-rolling the
ISO-TP flow control so WE choose STMIN. The adapter stamps every frame with a
microsecond timestamp, so the gaps between consecutive frames are measured on
the wire, not inferred from host wall-clock.

That separates the two candidate explanations:
  - gaps ~= our requested STMIN   -> the ECU is honouring flow control; the
                                     host's FC frame is what throttles us
  - gaps >> requested STMIN       -> the ECU (or the adapter) imposes its own
                                     spacing and STMIN tuning cannot help

Read-only: SID 0x23 ReadMemoryByAddress only. Nothing is written.

Usage: isotp_timing.py [stmin_hex]     default 00 (as fast as possible)
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
RESPONSE_ID = 0x7E8

READ_ADDR = 0x8056A8
READ_LEN = 0xC0  # 192 bytes -> 1 FirstFrame + 27 ConsecutiveFrames


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
            time.sleep(0.002)
            continue
        if chunk:
            out += chunk
    return out


def send(fd, command, payload=b""):
    os.write(fd, command + payload)


def tx_frame(fd, can_id, data8):
    frame = struct.pack(">I", can_id) + bytes(data8)
    send(fd, f"att{CAN} {len(frame)} 0\r\n".encode(), frame)


def parse_records(raw):
    """Yield (timestamp_us, can_id, payload) for every 'ar5' data record.

    Record layout, confirmed against live captures:
      'a' 'r' '5' <len> <type> <ts:4 BE> <canid:4 BE> <data...>
    where <len> counts everything after itself.
    """
    out = []
    i = 0
    while i < len(raw) - 4:
        if raw[i:i + 3] == b"ar5":
            length = raw[i + 3]
            body = raw[i + 4:i + 4 + length]
            if len(body) >= 9:
                ts = struct.unpack(">I", body[1:5])[0]
                can_id = struct.unpack(">I", body[5:9])[0]
                out.append((ts, can_id, body[9:]))
            i += 4 + length
        else:
            i += 1
    return out


def main():
    stmin = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x00
    fd = open_port()
    try:
        send(fd, b"ata\r\n"); drain(fd, 0.3)
        send(fd, f"ato{CAN} 0 500000 {CAN}\r\n".encode()); drain(fd, 0.3)
        send(fd, f"atf{CAN} {PASS_FILTER} 0 4\r\n".encode(),
             b"\x00\x00\x00\x00" * 2); drain(fd, 0.3)

        print(f"ISO-TP 192-byte read, our FlowControl says BS=0 STMIN=0x{stmin:02x}")

        # SingleFrame: len 5, then 23 <addr 3> <len>
        request = [0x05, 0x23, (READ_ADDR >> 16) & 0xFF, (READ_ADDR >> 8) & 0xFF,
                   READ_ADDR & 0xFF, READ_LEN, 0x00, 0x00]
        tx_frame(fd, REQUEST_ID, request)
        time.sleep(0.02)

        # FlowControl: 30 <BS=0> <STMIN>. Sent unsolicited right after the
        # request; the ECU's FirstFrame is already on its way and it will
        # consume this before streaming ConsecutiveFrames.
        tx_frame(fd, REQUEST_ID, [0x30, 0x00, stmin, 0, 0, 0, 0, 0])

        raw = drain(fd, 1.5)
        send(fd, f"atc{CAN}\r\n".encode()); drain(fd, 0.2)
        send(fd, b"atz\r\n"); drain(fd, 0.2)

        records = [r for r in parse_records(raw) if r[1] == RESPONSE_ID]
        if not records:
            print(f"\nNo frames from 0x{RESPONSE_ID:03X}. raw={raw[:120].hex(' ')}")
            return 2

        print(f"\n{len(records)} frames from the ECU:\n")
        print("   #   pci  timestamp_us     gap_us   gap_ms")
        gaps = []
        for index, (ts, _, data) in enumerate(records):
            if index == 0:
                print(f"  {index:3d}   {data[0]:02x}   {ts:>10}          -        -")
                continue
            gap = ts - records[index - 1][0]
            gaps.append(gap)
            print(f"  {index:3d}   {data[0]:02x}   {ts:>10}   {gap:>8}   {gap/1000:6.2f}")

        if gaps:
            total = records[-1][0] - records[0][0]
            print(f"\n  frames {len(records)}, span {total} us = {total/1000:.2f} ms")
            print(f"  mean gap {sum(gaps)/len(gaps):.0f} us, "
                  f"min {min(gaps)} us, max {max(gaps)} us")
            print(f"\n  CAN wire time for one 8-byte frame at 500 kbit/s is ~270 us.")
            print(f"  Requested STMIN was 0x{stmin:02x} "
                  f"({'0 ms, i.e. as fast as possible' if stmin == 0 else f'{stmin} ms'}).")
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
