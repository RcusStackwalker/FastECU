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
equivalent to "ran chained" — section 3 requires both, in that order.

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

**Blocker — do not run these two commands under any circumstances yet:**

- [ ] **`upload-routine erase-redirect` — DO NOT RUN.**
- [ ] **`upload-routine write-redirect` — DO NOT RUN.**

  `//src/algorithms/protocol/colt:test_mitsu_colt_can_protocol` currently
  **fails** on this branch:
  `erase_and_write_redirect_routines_match_reflash_dir_checksums` expects
  `sizeof(kEraseRedirectRoutine) == 192` and
  `sizeof(kWriteRedirectRoutine) == 188` with checksums `0x5079` / `0x514e`,
  matching `mmc-patches/m32r/47110032/reflash/` build output. But
  `mitsu_colt_can_protocol.h:46-47,74-75` currently declares both redirect
  arrays at `kEraseRoutineSize = 160` / `kWriteRoutineSize = 176`. Either the
  baked-in arrays are truncated or the test's expectation is stale — nobody
  has established which. Running `upload-routine erase-redirect` or
  `upload-routine write-redirect` today pushes a 160/176-byte payload into
  ECU RAM and executes it as if it were the real 192/188-byte routine. Do not
  run either command until that test passes and the size/checksum
  discrepancy is resolved. `erase-page` and `write-page` are unaffected —
  both are sized 160/176 consistently with their own passing assertions.

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

**Qualification sequence.** Run `unlock`, then `erase`, then
`upload-routine erase-page`, each on its own invocation first — so a failure
in one is isolated — and only then chained in a single invocation, since
that is what the desktop app's reflash workflow actually does on the wire
(one bootloader session, not three). Record the reply bytes verbatim for
every run, not a summary of pass/fail.

- [ ] `./bazel-bin/apps/bench/fastecu-bench unlock --destructive`
- [ ] `./bazel-bin/apps/bench/fastecu-bench erase --destructive`
- [ ] `./bazel-bin/apps/bench/fastecu-bench upload-routine erase-page --destructive`
- [ ] Chained in one session:
      `./bazel-bin/apps/bench/fastecu-bench unlock --destructive : erase --destructive : upload-routine erase-page --destructive`

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
      code for this one command.
- [ ] **A step that fails after doing I/O does not print its `tx`/`rx`
      bytes.** `run_step` returns `Result<CommandOutcome>`; on failure the
      populated outcome (which may already carry real transmitted/received
      bytes and `vbatt`) is discarded, and `main.cpp` reconstructs a minimal
      outcome carrying only the step name, error kind, and error detail. In
      practice: if a destructive step in section 3 fails, the tool's own
      output will **not** show the reply bytes this checklist asks you to
      record verbatim. Capture the adapter-level trace by some other means
      (CAN sniffer log, adapter debug output) for any step that fails — do
      not rely on this tool's stdout for that step's bytes. This is a known
      open design gap, flagged for a later review; it is not being worked
      around here.

## 5. Sign-off

Complete only after every item in section 3 has passed, including the
chained run.

- [ ] Date: ________________
- [ ] ROM id: ________________
- [ ] Adapter: ________________
- [ ] Operator initials: ________________
