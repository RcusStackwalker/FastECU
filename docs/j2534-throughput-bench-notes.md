# J2534 serial throughput — bench measurements

Measurements backing the [J2534 serial throughput design](superpowers/specs/2026-08-23-j2534-serial-throughput-design.md).

Every figure here is a real measurement against real hardware. Do not add a
projected or calculated number to this file without labelling it as such.

## Setup

- Adapter: Tactrix OpenPort 2.0, `/dev/cu.usbmodemTApU_RJO1`, firmware 1.17.4869
- ECU: Mitsubishi Colt, spare/bench unit. **No vendor diagnostic extension** —
  it answers `23 27 41` with `7f 23 12` (NRC 0x12, subFunctionNotSupported),
  and reads work without `--vendor-ext`.
- Bus: CAN 500 kbit/s, 11-bit, request `0x7E0` / response `0x7E8`
- Supply: pin 16 = **12.395 V**, read via the adapter's own `atr 16` command
- Host: macOS, Apple Silicon
- Command: `fastecu-bench --port "cu.usbmodemTApU_RJO1 - OpenPort 2.0" --stats --json read 0x8056a8 8192`
  (8192 bytes = 43 chunks at `kFlashReadBlockSize` = 192)

**The `--port` argument is required.** Without it the CLI takes the first port
`QSerialPortInfo` reports, which on macOS is `cu.Bluetooth-Incoming-Port`. Every
exchange then times out against a port with no ECU behind it. See "Root cause of
the initial no-response failures" below.

## Headline result

| | ms/exchange | bytes/s | 8 KiB read |
| --- | --- | --- | --- |
| Baseline, commit `4616356d` (per-byte serial I/O) | 343.0 | 555.4 | 14 749 ms |
| After buffered serial I/O, commit `c5859dc9` | 144.0 | 1 323.4 | 6 190 ms |
| **Speedup** | **2.38×** | **2.38×** | **2.38×** |

Three post-fix runs returned 6190 ms each, identical to the millisecond — the
remaining cost is a deterministic pacing floor, not I/O variance.

## The ceiling is the ECU, not the bus and not the host

The spec's original target — within ~2× of CAN wire time, about 15 ms per
192-byte chunk — **is not reachable, and never was.** The reason is measured,
not inferred.

Driving a 192-byte ISO-TP read over raw CAN with hand-rolled flow control, so
the host chooses STMIN, and reading the adapter's own microsecond timestamps:

| FlowControl STMIN we requested | Measured gap between consecutive frames |
| --- | --- |
| `0x00` (as fast as possible) | **5.00 ms**, min 4991 µs, max 5006 µs across 27 frames |
| `0x14` (20 ms) | **20.00 ms**, min 19990 µs, max 20000 µs |

The ECU honours STMIN precisely when asked for 20 ms, and clamps to 5 ms when
asked for 0. **5 ms per consecutive frame is an ECU-side floor.** CAN wire time
for one 8-byte frame at 500 kbit/s is ~270 µs, so 94% of each frame slot is the
ECU's own pacing.

### Ruling out the adapter as the source of the 5 ms

Those gaps come from OpenPort timestamps, which only support the conclusion if
the adapter stamps a frame at wire arrival. If it stamped at dequeue and drained
its RX queue on a periodic tick, evenly-spaced timestamps would be an artifact
of the tick and would say nothing about the ECU — and a 5 ms tick fits both rows
of the table above, since 20 ms is a multiple of 5 ms.

Control experiment, ECU removed from the question entirely: the adapter
transmits 12 frames back-to-back on an unused id (`0x123`) and we read the
timestamps of its own TX loopback records. Nothing paces these but the bus.

```
mean gap 471 us  (min 464, max 480, n=11)
```

Sub-millisecond and tightly clustered, against a 5 ms hypothesis that would have
produced either 5 ms spacing or batched identical timestamps. The adapter's
timestamp path resolves events far finer than 5 ms, so the 5.00 ms gaps from the
ECU are real bus-level pacing. (The 471 µs sits above the ~270 µs theoretical
wire time because each frame required its own `att` command over USB; that makes
471 µs an upper bound on the adapter's reporting granularity, not a floor.)

Two further observations point the same way and do not depend on the control:

- In the first raw-CAN exchange, the ECU's reply was timestamped **5822 µs**
  after our transmitted frame's loopback. A 5 ms-quantised report path could not
  produce 5822 µs.
- In the STMIN=0 run the FirstFrame→first-ConsecutiveFrame gap was 25 ms,
  tracking when the host sent its FlowControl frame rather than any fixed
  cadence — the ECU was reacting to flow control in real time.

This does not rule out the adapter's **ISO15765** engine having its own pacing,
but that engine is not in the path here: these measurements ran on raw CAN
(protocol 5) with host-side ISO-TP. Independently, `fastecu-bench` measured
144 ms/exchange *through* the adapter's ISO15765 engine for the same 192-byte
read, against ~155 ms measured on raw CAN — the two paths agree, so neither is
adding pacing the other lacks.

That gives a hard ceiling of 7 payload bytes per 5 ms:

```
7 bytes / 0.005 s = 1400 bytes/s
```

Measured throughput is **1323 bytes/s, which is 94.5% of that ceiling.**

Fitting the two measured points (16 bytes → 20 ms, 192 bytes → 144 ms) gives

```
exchange_ms = 8.7 + 0.714 x payload_bytes
```

where 0.714 ms/byte is exactly 5 ms ÷ 7 bytes. The 8.7 ms is fixed per-exchange
overhead. Larger chunks amortise only that fixed part, so raising
`kFlashReadBlockSize` from 192 toward infinity would gain at most ~6%. 192 is
already close to optimal.

## What this means for PR #176

[PR #176](https://github.com/RcusStackwalker/FastECU/pull/176) attributed the
slowness to ISO-15765 interframe spacing and tuned `ISO15765_STMIN` /
`ISO15765_BS`. The measurements above show it was half right and half wrong:

- **Right about the symptom.** Interframe spacing genuinely does dominate now —
  5 ms of every 5.27 ms is dead air.
- **Wrong about the lever.** That spacing is imposed by the ECU, which clamps
  our requested STMIN of 0 up to 5 ms. No host-side STMIN or BS value can move
  it. Setting `STMIN=0` was already the effective behaviour.
- **Unverifiable either way at the time**, because `PassThruIoctl` under
  `SET_CONFIG` discards the adapter's reply and returns `STATUS_NOERROR`
  unconditionally, so a rejected parameter was indistinguishable from an
  accepted one.

The real win was host-side: per-byte serial I/O, worth the 2.38× above.

## Root cause of the initial no-response failures

Before any of the above could be measured, every exchange timed out with an
empty RX. The cause was not the ECU, the bus, or the protocol:
`list_desktop_serial_ports` returns ports in `QSerialPortInfo` order, and the
CLI's implicit default takes the first one. On macOS that is
`cu.Bluetooth-Incoming-Port`, not the adapter. The session was talking to a
Bluetooth serial port.

Two things masked it for a long time:

1. `PassThruOpen`, `PassThruConnect`, `PassThruStartMsgFilter` and
   `PassThruIoctl` all discard the adapter's reply, so opening a channel on a
   port with no adapter behind it looked exactly like success.
2. A wedged adapter earlier in the same session produced identical symptoms and
   was cleared by a USB replug, which briefly made the port question look
   settled.

## Raw evidence

Adapter liveness and supply, via the Tactrix protocol directly:

```
ata     -> aro
ati     -> ari main code version : 1.17.4869
atr 16  -> arr 16 12395          (12.395 V)
```

First successful ECU exchange, raw CAN, showing the bus was always fine:

```
TX  00 00 07 e0 | 02 10 81 00 00 00 00 00
RX  00 00 07 e8 | 02 50 81 ff ff ff ff ff     (UDS positive response)
```

First successful read through `fastecu-bench` once the port was named:

```
read 0x8056a8 16
  1 exchange (20 ms)
  TX first 23 80 56 a8 10
  RX first 63 00 00 00 00 00 00 3f fe 00 00 40 00 00 00 5f fe
  ok (20 ms)
```

## Open items

- The 8.7 ms fixed per-exchange overhead has not been broken down. At ~6% of a
  192-byte exchange it is not currently worth chasing, but it is the only
  host-side headroom left.
- Whether the 5 ms floor is a firmware task-tick or a deliberate diagnostic
  rate limit is unknown, and would need ROM analysis rather than bench
  measurement to settle.
