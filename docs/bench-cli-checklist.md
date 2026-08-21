# Bench CLI qualification checklist — `//apps/bench:fastecu-bench`

Gate before any use of `fastecu-bench` against a real ECU. This CLI drives
the Colt CZT (Z37A, 47110032) CAN reflash protocol one primitive UDS
operation at a time, including `unlock` and `erase`; it is **not
bench-qualified** until section 3 below has been completed successfully,
matching the convention in the [flash qualification matrix](flash-qualification-matrix.md)
and the [Colt CZT CAN bench checklist](colt_czt_47110032_can_bench_checklist.md)
that gates the desktop reflash workflow the same commands sit underneath.

Build with `bazel build --config=release //apps/bench:fastecu-bench`; the
binary lands at `bazel-bin/apps/bench/fastecu-bench`. Every command below
assumes that path. Steps chained in one invocation with a bare `:` token run
inside a single bootloader session (one `0x10`/`0x27` handshake); steps run
as separate invocations each reconnect from scratch, which is a materially
different sequence on the wire. Do not treat "ran individually" as
equivalent to "ran chained." In particular, a fresh process must never infer
that ECU RAM still contains the erase routine uploaded by an earlier process.

Each text or JSON command outcome reports an exchange count and elapsed time,
plus the first and last complete UDS request/response PDUs. For a single
exchange the first and last pairs are identical. An empty RX means no complete
response PDU arrived. `read`/`dump` additionally report the complete assembled
memory bytes as `DATA` / `data`, separate from traffic evidence. JSON mode is
newline-delimited: exactly one JSON object is written to stdout per outcome,
including `ports`, setup failures, and explicit `connect` failures; diagnostics
are written to stderr.

## 1. Preconditions

- [ ] The device under test is a bench or spare ECU. Never a car.
- [ ] `boot-talk` recovery is installed and reachable from this bench setup.
      (Section 3 has a stronger check on this before `unlock` specifically —
      installed is not the same as tested.)
- [ ] Supply voltage is stable and above 12 V for the whole session. A prior
      session logged 11.676 V during a failure; treat anything near or below
      12 V as disqualifying, not as a note to work around.
- [ ] `./bazel-bin/apps/bench/fastecu-bench ports` lists the OpenPort 2.0
      adapter before anything else in this checklist is attempted.

## 2. Read-only qualification, in order

None of these commands is destructive, but they must all pass, in order,
before any command in section 3 is attempted. Record the raw reply bytes for
each — not a paraphrase of whether it "looked right."

- [ ] `./bazel-bin/apps/bench/fastecu-bench connect`
- [ ] `./bazel-bin/apps/bench/fastecu-bench read 0x200 1`
- [ ] `./bazel-bin/apps/bench/fastecu-bench read 0x807fc6 2`
- [ ] `./bazel-bin/apps/bench/fastecu-bench read 0x807f88 2`
- [ ] `./bazel-bin/apps/bench/fastecu-bench read 0x8056a8 256`

## 3. Destructive qualification

**Redirect routines are bench-qualified with a carrier-verification caveat.**

- [x] `erase-redirect`: 160-byte upload and top-128-KiB erase verified on a
      spare 47110032 ECU on 2026-08-21.
- [x] `write-redirect`: 176-byte, checksum-`0x4eae` routine verified by writing
      and reading back all of `0x60000..0x7ffff`; the 128-KiB source and ECU
      dump both had SHA-256
      `09d69fe0357997a97a843d1d8e16926d23d5941223a92fd6172d698119711380`.

  The bootloader verifies each programmed page against the original logical
  RequestDownload address after the RAM helper returns. The redirect helper
  maps logical `0x8000` to physical `0x60000`, but the verifier still reads
  `0x8000`. Therefore an ordinary redirect download returns NRC `0x71` when
  carrier and target payloads differ, even when the physical write succeeds.
  Qualification used the same 128-KiB payload at carrier `0x8000..0x27fff`
  before invoking `write-redirect`; all 512 redirected page transfers were
  then accepted. The carrier was erased and restored from the real userspace
  image afterward. Do not interpret NRC `0x71` as proof that this helper failed,
  and do not use redirect download with arbitrary carrier contents if a positive
  TransferData acknowledgement is required.

**Before the first `unlock`, and not merely nominally:** `unlock` sends the
exact `0x3B` payload that `mitsu_colt_can_protocol.h` annotates as "KNOWN
RISK" — it "caused bootloader lockup" during the original author's own
testing, with the header's comment calling it out as the highest-risk step
in this protocol and saying never to send it without an explicit
confirmation gate on a bench/spare ECU. The CLI's only gate is the
`--destructive` flag on the `unlock` step itself, same as `erase`,
`download`, and `upload-routine`. Before running `unlock` for the first
time:

- [ ] Confirmed `boot-talk` ECU recovery is actually available **and
      tested** on this exact bench setup right now — not just installed
      somewhere, not tested on a different rig, not "should still work
      since last time." The bench-vs-car precondition above only holds up if
      this recovery path genuinely works.

**Qualification sequence.** The desktop executor uploads the intended erase
routine before it unlocks and triggers erase. Preserve that order. First test
the upload by itself; this loads and CRC-checks RAM but does not execute the
routine. Then start a new process and run the complete upload → unlock → erase
chain in one session. Record the CLI's first/last traffic and elapsed time for
each outcome, not only a pass/fail summary.

- [ ] Upload only (does not execute the routine):
      `./bazel-bin/apps/bench/fastecu-bench upload-routine erase-page --destructive`
- [ ] Desktop-order chain in one session:
      `./bazel-bin/apps/bench/fastecu-bench upload-routine erase-page --destructive : unlock --destructive : erase --destructive`

**Never run `erase` as a standalone invocation.** Its prerequisite is not
"an upload succeeded sometime earlier"; the intended erase routine must have
been uploaded earlier in the same process/session. After a restart or separate
invocation, treat RAM contents as unknown.

## 4. Known limitations

Acknowledge each of these before relying on this tool's output during a
session — they are documented behavior, not bugs to route around mid-run.

- [ ] `vbatt()` always returns `Unsupported`. `ICanFlashTransport` exposes no
      battery read, so `CommandOutcome::vbatt` is never populated over CAN;
      battery must be watched externally (meter, bench supply readout) for
      the whole session, not read from the tool's output.
- [ ] `send-raw` bypasses NRC handling entirely. A negative response (e.g. a
      UDS NRC byte) is reported as an ordinary reply, not flagged as an
      error — read the raw bytes yourself rather than trusting `ok`/exit
      code for this one command. Both `send` and `send-raw` reject known
      destructive PDUs at parse time: SID `0x3B`, SID `0x34`, SID `0x36`, and
      RoutineControl `0x31/0xE0` are reserved for the named, gated commands.
- [ ] A failure after I/O retains the traffic observed before the failure.
      Multi-exchange operations report first and last PDU pairs plus an
      exchange count and elapsed time; they do not print every intermediate
      transfer frame. Use an adapter-level trace if every intermediate frame
      is required.
- [ ] RoutineControl `0x31/0xE1` status `0x01` for a flash-address check is not
      exclusively a byte-sum mismatch. After the sums match, this bootloader
      also runs its code-integrity check and returns the same status when that
      check fails. The bench test image produced status `0x01` despite complete,
      byte-identical userspace and top-bank readback; use a full dump comparison
      when qualifying deliberately patched images.
- [ ] With `--script -`, global options (`--port`, `--json`, `--verbose`,
      `--timeout`, `--keep-going`, `--no-connect`, `--script`) belong on the
      outer invocation. They are rejected on individual script lines rather
      than silently discarded. Step-local `--destructive` and
      `upload-routine --from <file>` remain valid on script lines.

## 5. Sign-off

Complete only after every item in section 3 has passed, including the
chained run.

- [ ] Date: ________________
- [ ] ROM id: ________________
- [ ] Adapter: ________________
- [ ] Operator initials: ________________
