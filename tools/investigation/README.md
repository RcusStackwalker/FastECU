# Colt CAN read investigation — throwaway diagnostic probes

**These are not part of the build and are not maintained.** They exist as the
record of how the "slow Colt CAN read" findings were established, so the
measurements can be reproduced or challenged rather than taken on trust.

They speak the Tactrix OpenPort serial protocol directly over the tty,
deliberately bypassing FastECU, so a result from a probe is independent of the
code under investigation. Plain Python 3, no dependencies (`pyserial` is not
required and is not installed on the bench host).

Every probe hardcodes `/dev/cu.usbmodemTApU_RJO1`. Edit `PORT` for a different
machine.

## Safety

All six are read-only with respect to ECU state. `can_tx_probe.py` and
`isotp_timing.py` transmit, but only UDS `0x10` (StartDiagnosticSession) and
`0x23` (ReadMemoryByAddress), which is what `fastecu-bench read` already sends.
`fc_probe.py` transmits a single ISO-TP FirstFrame and deliberately never
follows it with a ConsecutiveFrame, so no request ever completes — the ECU's
N_Cr timer expires and discards the partial message. None of them writes ECU
memory, erases, or unlocks. **Do not extend them to send SID `0x3B`, `0x34`,
`0x36`, or RoutineControl `0x31/0xE0` without the gating that
`docs/bench-cli-checklist.md` requires.**

## The probes

| Script | Question it answers |
| --- | --- |
| `vbatt_probe.py` | Is the adapter alive, and does OBD pin 16 have voltage? Sends `ata` / `ati` / `atr 16`. `ati` is answered by the adapter's own MCU off USB power, so total silence means a wedged or absent adapter rather than an unpowered ECU. |
| `can_sniff.py` | Is there any traffic on the bus? Raw CAN, accept-everything filter, receive-only. |
| `can_tx_probe.py` | Is the ECU electrically present? Transmits one frame and looks for the ACK plus a reply. A CAN frame needs another node to acknowledge it, so this distinguishes "ECU not answering" from "ECU not on the bus". |
| `isotp_timing.py` | Where does the per-frame dead time come from? Hand-rolls ISO-TP over raw CAN so the host chooses STMIN, then reports adapter microsecond timestamps per frame. Takes STMIN as an argument. |
| `ts_control.py` | **Control for `isotp_timing.py`.** Do the adapter's timestamps measure wire arrival, or a dequeue tick? Transmits back-to-back frames with no ECU involved and reads its own loopback timestamps. Without this, evenly-spaced timestamps prove nothing. |
| `fc_probe.py` | What spacing does the ECU demand of **us** — the write direction? Sends one ISO-TP FirstFrame and stops, reading BS/STMIN off the FlowControl the ECU's transport layer must send before the application layer sees the service id. No ConsecutiveFrame follows, so no request completes and no write traffic occurs. |

## Findings these produced

Recorded in full in [bench measurements](../../docs/j2534-throughput-bench-notes.md).

1. **Port selection, the actual blocker.** Every exchange timed out because the
   bench CLI's implicit default takes the first port `QSerialPortInfo` reports,
   which on macOS is `cu.Bluetooth-Incoming-Port`. The probes reached the ECU
   immediately because they open the adapter by path.
2. **A 5 ms ECU-side frame pacing floor.** `isotp_timing.py 00` gives 5.00 ms
   gaps; `isotp_timing.py 14` gives 20.00 ms. The ECU honours STMIN and clamps
   it at 5 ms, capping throughput at ~1400 B/s regardless of host behaviour.
3. **That floor is not an instrument artifact.** `ts_control.py` shows the
   adapter's own loopback frames 471 µs apart, so its timestamp path resolves
   far finer than 5 ms.
4. **The write direction is not throttled at all.** `fc_probe.py` shows the ECU
   answering `30 00 00` — ContinueToSend, BS=0, STMIN=0. Reads are ECU-bound;
   writes are host-bound.

## Reproducing

```sh
python3 tools/investigation/vbatt_probe.py       # adapter alive? pin 16 volts?
python3 tools/investigation/can_tx_probe.py      # does the ECU answer at all?
python3 tools/investigation/ts_control.py 12     # validate the instrument FIRST
python3 tools/investigation/isotp_timing.py 00   # then measure frame pacing
python3 tools/investigation/isotp_timing.py 14   # and confirm STMIN is honoured
python3 tools/investigation/fc_probe.py          # what the ECU demands of us
```

Run `ts_control.py` before believing any `isotp_timing.py` result. The original
5 ms conclusion was asserted without it and was not sound until it existed.

If every command returns empty, including `ati`, the adapter has wedged — unplug
and replug its USB. That happened once during this investigation and produced
symptoms indistinguishable from a dead ECU.
