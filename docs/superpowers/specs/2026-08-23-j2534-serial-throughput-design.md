# J2534 serial throughput on Unix — design

Status: phases 0–2 implemented and measured; phases 3–4 superseded by the
measurements. See [bench measurements](../../j2534-throughput-bench-notes.md)
and the Outcome section at the end of this document.
Applies to: macOS and Linux only (`src/platform/desktop/unix/j2534/`). Windows
reaches the adapter through the vendor DLL and is untouched by every phase here.

## Problem

Bulk CAN operations against a Mitsubishi Colt CZT ECU are far slower than the
bus. [PR #176](https://github.com/RcusStackwalker/FastECU/pull/176) attributed
this to ISO-15765 interframe spacing and tuned `ISO15765_STMIN` / `ISO15765_BS`.
It made no measurable difference, and the change could not have been validated
either way — see "Why #176 could not work" below.

The dominant cost is not on the CAN bus. It is in the host's serial I/O with
the Openport 2.0, which moves **one byte per call in each direction**.

### Receive path

`J2534::read_serial_data` (`J2534_unix.cpp:81`):

```cpp
QTime dieTime = QTime::currentTime().addMSecs(timeout);
while ((uint32_t)ReceivedData.length() < datalen && (QTime::currentTime() < dieTime))
{
    if (serial->bytesAvailable())
    {
        dieTime = QTime::currentTime().addMSecs(timeout);
        ReceivedData.append(serial->read(1));
    }
    serial->waitForReadyRead(1);
}
```

Two defects compound:

1. `serial->read(1)` takes a single byte per iteration.
2. `waitForReadyRead(1)` runs on **every** iteration, including when
   `bytesAvailable()` is already non-zero. `QIODevice::waitForReadyRead` blocks
   until *new* data arrives. `QSerialPort` has already drained the kernel
   buffer into its own, so when a burst is sitting in Qt's buffer there is no
   new data to wait for and the call sleeps its full 1 ms.

The result is roughly **1 ms of wall clock per received byte**, independent of
line rate.

### Transmit path

`J2534::write_serial_data` (`J2534_unix.cpp:106`) issues one
`serial->write(msg, 1)` per byte and never flushes. `QSerialPort::write` only
appends to Qt's write buffer; the actual write happens when the event loop
runs, which here means "whenever some later `waitForReadyRead` happens to pump
it". Transmission is therefore both fragmented and timed by an unrelated call.

### Magnitude

One 192-byte flash read chunk (`MitsuColtCan::kFlashReadBlockSize`) at
500 kbit/s:

| Layer | Cost per chunk |
| --- | --- |
| CAN wire (1 FF + 27 CF, ~135 bits/frame) | ~7.6 ms |
| Host serial layer (~200 bytes at ~1 ms/byte) | ~200 ms |

The host is roughly **25× slower than the bus it is driving**. Interframe
spacing cannot be the limiting factor while that ratio holds, which is why
#176 produced no observable change.

### Why #176 could not work

`J2534::PassThruIoctl` under `SET_CONFIG` (`J2534_unix.cpp:1029-1061`) writes
`ats<chan> <param> <value>`, reads the adapter's reply into a local, logs it,
and discards it. The function returns `STATUS_NOERROR` on every path.

Consequently #176's fallback — retry with `STMIN=1` / `BS=16` when the first
`SET_CONFIG` is rejected — was unreachable code, and "STMIN=0 was accepted" was
never actually observed. The tuning may or may not have reached the adapter;
nothing in the system could distinguish the two.

## Non-goals

- No change to the Windows J2534 path.
- No change to any flash write path, address-window guard, or qualification
  status. Phases 0–2 are read-only against the ECU.
- No change to the Tactrix wire protocol or to how call sites parse it. The
  byte-at-a-time parsing in `PassThruReadMsgs` stays exactly as written; only
  the cost of a one-byte read changes.

## Phases

Phases are ordered so that each is measurable before the next begins.
Phase 2 deliberately precedes phase 3: tuning interframe spacing while the host
is 25× slower than the bus is what produced #176's unfalsifiable result.

### Phase 0 — Vendor-extension connect in the bench CLI

Prerequisite. The bench ECU carries the third-party vendor diagnostic
extension, so it does not answer a bare bootload-session request and no
measurement is possible without this.

`BenchSession::connect()` (`apps/bench/bench_session.cpp:101`) sends
`buildDiagnosticSession(kSessionBootload)` as its first PDU. The desktop
executor's `connect_bootloader`
(`src/backend/flash/ecu/mitsu_colt_m32r_can_executor.cpp:100-152`) instead
opens a **basic** session first and completes the vendor challenge when its
plan sets `use_vendor_challenge`.

Change:

- Add `bool vendor_ext = false;` to `GlobalOptions` and a `--vendor-ext` global
  flag in `parse_command_line`.
- When set, `BenchSession::connect()` runs, before the existing bootload
  sequence:
  1. `buildDiagnosticSession(kSessionBasic)`, verifying the echoed session byte.
  2. `MitsuColtCanVendorExt::buildChallengeSeedRequest()`, requiring at least
     the two selector bytes plus a 4-byte seed, then
     `uds::payload(reply).subspan(2, 4)` as the seed.
  3. `challengeInverseTransform(bytesToSeed(seed))` → `buildChallengeKey(key)`,
     requiring the reply to carry `kVendorChallengeAccepted` (0x34). Echoing
     the selector alone is **not** acceptance — this mirrors the executor's
     explicit content check.
- The existing bootload session and factory SecurityAccess then run unchanged.

Default off, so every step in the [bench CLI checklist](../../bench-cli-checklist.md)
keeps its current on-the-wire behavior. Reuses `MitsuColtCanVendorExt`
wholesale; no new protocol code.

### Phase 1 — Baseline measurement

`CommandOutcome` already carries `exchange_count` and `elapsed_ms`, and `--json`
already emits them. Add a `--stats` global flag that derives, per outcome,
`bytes_per_s` (`data.size()` over `elapsed_ms`) and `ms_per_exchange`
(`elapsed_ms` over `exchange_count`). In text mode these print as an extra
indented line; in JSON mode they become two additional keys on the existing
object. Outcomes with no `data` or no exchanges omit the respective figure
rather than reporting zero.

Take the baseline on a bulk read spanning many chunks — 8 KiB is 43 chunks at
`kFlashReadBlockSize` — and record it in `docs/j2534-throughput-bench-notes.md`,
created by this phase. Every later phase is judged against this number.

### Phase 2 — Buffered serial I/O

The substantive change.

Introduce `SerialByteBuffer` in `src/platform/desktop/unix/j2534/`:

- Owns a `QByteArray` staging buffer and a refill callable injected at
  construction. The callable performs one bulk read and returns what it got;
  the production one is `serial->read(serial->bytesAvailable())` preceded by a
  `waitForReadyRead` only when nothing is buffered.
- Exposes `take(std::size_t n, int timeout_ms)`: serves from the staging buffer
  first, refills in bulk when short, and returns as soon as `n` bytes are
  available or the timeout expires.
- Preserves the existing silence-timeout semantics exactly: the deadline
  refreshes whenever data arrives, so `take` means "n bytes, or `timeout_ms` of
  silence", identical to today's `read_serial_data`.

`J2534::read_serial_data` becomes a thin wrapper over `take`. Every call site is
unchanged — `while ((uint8_t)msg[msg.length() - 1] != 0x0a) msg.append(read_serial_data(1, Timeout));`
still reads one byte at a time and still stops on the terminator. It simply
stops costing a millisecond each.

`J2534::write_serial_data` becomes a single `serial->write(output)` followed by
`waitForBytesWritten(timeout)`, replacing the per-byte loop and the implicit
"flushed by whoever pumps the event loop next" behavior.

Testing: `SerialByteBuffer` is pure over its refill callable, so it gets a
package-owned gtest with a fake source (per
[ADR 0008](../../adr/0008-use-package-owned-mocks.md)) covering exact-fit,
short-refill, over-refill with leftovers retained, timeout with partial data,
and the deadline-refresh-on-data rule. The existing pty-based `MockOpenPort`
harness (`unix/j2534/testing/mock_openport.h`) covers end-to-end byte-sequence
equivalence through `PassThruReadMsgs`.

### Phase 3 — Verifiable ISO-TP configuration, then re-land #176

`PassThruIoctl` under `SET_CONFIG` must parse the adapter's reply to the `ats`
command and return a failure status when it is not an acknowledgement, instead
of unconditionally returning `STATUS_NOERROR`.

Before landing this: audit every `SET_CONFIG` caller — including the K-Line
paths in `serial_port_actions_direct.cpp` — for code that currently depends on
a rejected parameter passing silently. Any such caller is either fixed or
explicitly exempted with a comment saying why.

With that in place, #176's `ISO15765_STMIN = 0` / `ISO15765_BS = 0` change and
its `STMIN=1` / `BS=16` fallback become real, exercised code. Re-measure to
determine whether interframe spacing contributes anything once the host is no
longer the bottleneck. It is an acceptable outcome for this phase to conclude
that it does not and to keep only the error propagation.

### Phase 4 — Re-measure and record

Target: bulk read throughput within roughly 2× of CAN wire time — about
7.6 ms per 192-byte chunk, a ceiling near 25 KB/s at 500 kbit/s.

Record baseline and post-change figures in `docs/j2534-throughput-bench-notes.md`
alongside the existing checklists. State plainly what was measured, on which
ECU, and at what supply voltage.

## Risks

- **`read_j2534_data` extra reads.** For non-CAN connections
  (`serial_port_actions_direct.cpp:1194-1240`), a `TX_DONE` or
  `START_OF_MESSAGE` status triggers an additional blocking `PassThruReadMsgs`
  at the full timeout. ISO-15765 takes this branch. These become cheap once
  phase 2 lands, but the sequence must be checked for consuming a frame the
  caller needed.
- **Propagating `SET_CONFIG` failures.** Turning a silent success into a real
  error can surface latent breakage in K-Line paths. This is why phase 3
  requires a caller audit rather than a one-line change.
- **Timeout semantics.** `read_serial_data`'s deadline currently refreshes on
  every received byte. Getting this subtly wrong in `SerialByteBuffer` changes
  how long the app waits on a slow or silent adapter. The unit tests name this
  rule explicitly.

## Success criteria

1. `bazel test --config=release //...` green, `prek run --all-files` clean.
2. `--vendor-ext` establishes a session against the bench ECU and a bulk read
   returns correct data.
3. Post-change bulk read throughput within ~2× of CAN wire time, with baseline
   and result both recorded.
4. `SET_CONFIG` returns a failure status when the adapter rejects a parameter,
   demonstrated by a test.

## Outcome

Phases 0–2 landed and were measured on a spare Colt ECU. Phases 3–4 were
overtaken by what the measurements showed; the full data is in
[bench measurements](../../j2534-throughput-bench-notes.md).

**Delivered.** Buffered serial I/O took a 192-byte flash read from 343.0 ms to
144.0 ms per exchange — 555 to 1323 bytes/s, a measured **2.38×**, taken against
the actual pre-change commit rather than projected.

**The stated target was unreachable, and the diagnosis in this document was
wrong about why.** This spec assumed the host was ~25× slower than the bus and
that removing that gap would approach CAN wire time. The first half was right.
The second was not: the ECU paces its own consecutive frames at a 5 ms floor. It
honours a requested STMIN of 20 ms exactly and clamps a requested 0 to 5 ms, so
its ceiling is 7 payload bytes per 5 ms = 1400 bytes/s. The measured 1323 bytes/s
is 94.5% of that. "Within ~2× of CAN wire time" was never achievable on this ECU
by any host-side change. Success criterion 3 is therefore retired, and the
honest replacement is: **within ~6% of the ECU's own ceiling.**

**Phase 3 splits.** Making `SET_CONFIG` failures observable is still worth doing
and is more valuable than this document realised — the same swallowed-reply
pattern also hid a port-selection bug that made every exchange time out against
`cu.Bluetooth-Incoming-Port`. Re-landing PR #176's STMIN/BS tuning is **not**
worth doing: the ECU clamps STMIN regardless, so the change is a measured no-op.
Both are tracked in the "slow Colt CAN read" issue rather than here.

**A methodological note worth keeping.** The 5 ms figure was first asserted from
adapter timestamps without establishing what those timestamps measure — a
dequeue-time report tick would have produced identical evidence. A control
experiment with the ECU removed (adapter transmitting to itself, loopback
timestamps 471 µs apart) was needed to make the claim stand. Instrument
validation belongs before the conclusion, not after it.
