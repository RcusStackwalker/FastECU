#!/usr/bin/env python3
"""Control experiment: what do the adapter's timestamps actually measure?

The ECU-pacing conclusion rests on inter-frame gaps taken from OpenPort
timestamps. That is only valid if the adapter stamps a frame when it hits the
wire. If instead it stamps at dequeue and drains its RX queue on a periodic
tick, evenly-spaced timestamps would be an artifact of the tick, not evidence
about the sender.

This test removes the ECU from the question. The adapter transmits N frames
back to back and we read the TX LOOPBACK records it generates for its own
traffic. The host writes them as fast as the USB link allows and no ECU paces
them, so:

  loopback gaps ~= CAN wire time (~270 us for an 8-byte frame at 500 kbit/s)
      -> timestamps are wire-time; 5 ms gaps from the ECU are REAL ECU pacing

  loopback gaps ~= 5 ms, or quantised to a multiple of it
      -> the adapter's timestamp/report path is quantised; the ECU-pacing
         conclusion is UNPROVEN and must be withdrawn

Frames are sent on an unused id with a benign payload, so nothing addresses
the ECU and nothing can be interpreted as a diagnostic request.

Usage: ts_control.py [count]
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
# Deliberately not 0x7E0/0x7E8: nothing should treat this as diagnostics.
SCRATCH_ID = 0x123


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


def parse_records(raw):
    """(timestamp_us, type_byte, can_id, payload) for every 'ar5' record."""
    out = []
    i = 0
    while i < len(raw) - 4:
        if raw[i:i + 3] == b"ar5":
            length = raw[i + 3]
            body = raw[i + 4:i + 4 + length]
            if len(body) >= 9:
                out.append((struct.unpack(">I", body[1:5])[0], body[0],
                            struct.unpack(">I", body[5:9])[0], body[9:]))
            i += 4 + length
        else:
            i += 1
    return out


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    fd = open_port()
    try:
        os.write(fd, b"ata\r\n"); drain(fd, 0.3)
        os.write(fd, f"ato{CAN} 0 500000 {CAN}\r\n".encode()); drain(fd, 0.3)
        os.write(fd, f"atf{CAN} {PASS_FILTER} 0 4\r\n".encode() +
                 b"\x00\x00\x00\x00" * 2); drain(fd, 0.3)

        print(f"transmitting {count} frames back-to-back on id 0x{SCRATCH_ID:03X}")
        print("(no ECU involvement: nothing paces these but the bus itself)\n")

        # Queue every frame into the USB pipe before reading anything back, so
        # host-side write latency cannot masquerade as inter-frame spacing.
        for n in range(count):
            frame = struct.pack(">I", SCRATCH_ID) + bytes([n, 0xAA, 0xBB, 0xCC, 0, 0, 0, 0])
            os.write(fd, f"att{CAN} {len(frame)} 0\r\n".encode() + frame)

        raw = drain(fd, 2.0)
        os.write(fd, f"atc{CAN}\r\n".encode()); drain(fd, 0.2)
        os.write(fd, b"atz\r\n"); drain(fd, 0.2)

        records = [r for r in parse_records(raw) if r[2] == SCRATCH_ID]
        if len(records) < 2:
            print(f"Only {len(records)} loopback records; raw={raw[:160].hex(' ')}")
            return 2

        print("   #  type  timestamp_us     gap_us    gap_ms")
        gaps = []
        for index, (ts, kind, _, _) in enumerate(records):
            if index == 0:
                print(f"  {index:3d}   {kind:02x}   {ts:>10}          -         -")
                continue
            gap = ts - records[index - 1][0]
            gaps.append(gap)
            print(f"  {index:3d}   {kind:02x}   {ts:>10}   {gap:>8}   {gap/1000:7.3f}")

        mean = sum(gaps) / len(gaps)
        print(f"\n  {len(records)} loopback frames, mean gap {mean:.0f} us "
              f"(min {min(gaps)}, max {max(gaps)})")
        print("\n  VERDICT:")
        if mean < 1500:
            print("   Sub-millisecond gaps -> timestamps track the wire, not a")
            print("   report tick. The 5 ms ECU gaps are REAL ECU pacing.")
        elif 4000 < mean < 6000:
            print("   ~5 ms gaps with no ECU involved -> the adapter quantises")
            print("   its timestamp/report path. The ECU-pacing conclusion is")
            print("   UNPROVEN and must be withdrawn.")
        else:
            print(f"   Inconclusive: mean {mean:.0f} us matches neither wire time")
            print("   (~270 us) nor a 5 ms tick. Needs a different instrument.")
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
