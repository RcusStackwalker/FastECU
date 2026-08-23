#!/usr/bin/env python3
"""What spacing does the ECU demand of US? (the write direction)

The measured 5 ms floor governs ECU->tester only: it is the ECU pacing its own
ConsecutiveFrames. For tester->ECU traffic -- UDS TransferData during a flash --
the roles swap, and OUR spacing is set by the STMIN byte in the FlowControl
frame the ECU sends us. That is a different parameter and has to be measured
separately.

SAFETY: this sends NO write traffic and completes NO request.

ISO-TP is a transport layer beneath the application layer. On receiving a
FirstFrame the ECU's transport layer must answer FlowControl before the
application layer has seen the service id at all. So we send one FirstFrame,
read the FlowControl, and then STOP -- no ConsecutiveFrame is ever sent, the
ECU never receives a complete PDU, and its N_Cr timer simply expires and
discards the partial message.

The declared service is 0x23 (ReadMemoryByAddress), a read, so even the
abandoned request would have been harmless had it completed.

Frame layout:
  FirstFrame     1L LL  d d d d d d      L = 12-bit total length
  FlowControl    30 BS STMIN
                    ^  ^-- STMIN: 0x00-0x7F = milliseconds
                    |                0xF1-0xF9 = 100-900 microseconds
                    +----- BS: frames per FlowControl, 0 = send them all

Usage: fc_probe.py
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
    out = []
    i = 0
    while i < len(raw) - 4:
        if raw[i:i + 3] == b"ar5":
            length = raw[i + 3]
            body = raw[i + 4:i + 4 + length]
            if len(body) >= 9:
                out.append((struct.unpack(">I", body[1:5])[0],
                            struct.unpack(">I", body[5:9])[0], body[9:]))
            i += 4 + length
        else:
            i += 1
    return out


def describe_stmin(value):
    if value <= 0x7F:
        return f"{value} ms" + (" (as fast as the sender likes)" if value == 0 else "")
    if 0xF1 <= value <= 0xF9:
        return f"{(value & 0x0F) * 100} microseconds"
    return "reserved/invalid -- receiver should treat as 0x7F (127 ms)"


def main():
    fd = open_port()
    try:
        os.write(fd, b"ata\r\n"); drain(fd, 0.3)
        os.write(fd, f"ato{CAN} 0 500000 {CAN}\r\n".encode()); drain(fd, 0.3)
        os.write(fd, f"atf{CAN} {PASS_FILTER} 0 4\r\n".encode() +
                 b"\x00\x00\x00\x00" * 2); drain(fd, 0.3)

        # FirstFrame declaring a 10-byte message: 0x10 | 0x0, 0x0A, then six
        # payload bytes starting with SID 0x23. Nothing follows it.
        ff = [0x10, 0x0A, 0x23, 0x80, 0x56, 0xA8, 0x10, 0x00]
        print("sending ONE FirstFrame (declared length 10) and stopping.")
        print("no ConsecutiveFrame follows, so no request ever completes.\n")
        frame = struct.pack(">I", REQUEST_ID) + bytes(ff)
        os.write(fd, f"att{CAN} {len(frame)} 0\r\n".encode() + frame)

        raw = drain(fd, 1.0)
        os.write(fd, f"atc{CAN}\r\n".encode()); drain(fd, 0.2)
        os.write(fd, b"atz\r\n"); drain(fd, 0.2)

        replies = [r for r in parse_records(raw) if r[1] == RESPONSE_ID]
        if not replies:
            print(f"No reply from 0x{RESPONSE_ID:03X}. raw={raw[:160].hex(' ')}")
            return 2

        for ts, _, data in replies:
            print(f"  0x{RESPONSE_ID:03X}  {data.hex(' ')}")

        fc = next((d for _, _, d in replies if d and (d[0] & 0xF0) == 0x30), None)
        if fc is None:
            print("\nNo FlowControl frame (PCI 0x3x) in the reply.")
            return 2

        flow_status = fc[0] & 0x0F
        block_size = fc[1]
        stmin = fc[2]
        status_name = {0: "ContinueToSend", 1: "Wait", 2: "Overflow/abort"}.get(
            flow_status, f"reserved({flow_status})")

        print(f"\nFlowControl from the ECU:")
        print(f"  FlowStatus  0x{flow_status:x}  {status_name}")
        print(f"  BlockSize   0x{block_size:02x}  "
              f"{'unlimited -- send every frame without waiting for another FC' if block_size == 0 else str(block_size) + ' frames per FC'}")
        print(f"  STMIN       0x{stmin:02x}  {describe_stmin(stmin)}")

        print("\nWhat this means for TransferData (256-byte chunks = 1 FF + 36 CF):")
        if stmin <= 0x7F:
            per_frame_us = stmin * 1000
        elif 0xF1 <= stmin <= 0xF9:
            per_frame_us = (stmin & 0x0F) * 100
        else:
            per_frame_us = 127000
        wire_us = 270
        gap_us = max(per_frame_us, wire_us)
        chunk_ms = 37 * gap_us / 1000.0
        print(f"  effective spacing  {gap_us} us per frame "
              f"(STMIN {per_frame_us} us vs ~{wire_us} us wire time)")
        print(f"  per 256-byte chunk ~{chunk_ms:.1f} ms")
        print(f"  512 KiB = 2048 chunks -> ~{2048 * chunk_ms / 1000:.0f} s of frame time")
        print("  (frame time only: excludes ECU programming time per block,")
        print("   erase, and per-exchange overhead)")
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
