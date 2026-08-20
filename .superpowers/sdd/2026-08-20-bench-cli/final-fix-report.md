# Final bench CLI fix report

## Status

DONE_WITH_CONCERNS. The consolidated bench CLI correction wave is complete on
top of fix base `993c4114`. The only concern is an unchanged, pre-existing Colt
routine-redirect test failure described under Verification.

No hardware-facing binary was run and no transport was opened during
verification. The unrelated untracked `fasecu.zip` was preserved.

## Implemented corrections

1. Corrected every destructive checklist and design example to load the erase
   routine before unlock and erase, stated that upload does not execute the
   routine, and prohibited treating standalone erase as safe when RAM contents
   are unknown after a process restart.
2. Closed the raw-command destructive bypass at parse time and again at command
   execution. `send` and `send-raw` reject the known destructive services:
   WriteMemoryByAddress (`0x3b`), RequestDownload (`0x34`), TransferData
   (`0x36`), and the erase RoutineControl request (`0x31 0xe0`). Safe diagnostic
   experimentation remains available.
3. Added pre-I/O validation for every three-byte address, range, and length,
   including arithmetic overflow, zero-byte download/upload files, and payloads
   larger than `0xffffff`.
4. Added consistent command evidence: exchange count, first TX/RX, last TX/RX,
   elapsed time, and complete data where applicable. Evidence survives failures
   after I/O, including connect/setup failures, and is emitted in both text and
   newline-delimited JSON output.
5. Added positive-response echo validation for diagnostic session and seed/key
   levels, plus routine ID and status validation for erase and CRC replies.
6. Extracted a pure CLI driver/environment seam. Explicit `connect` obtains an
   unconnected session and performs exactly one handshake; other commands retain
   lazy implicit connection unless `--no-connect` is selected.
7. Aligned RequestDownload and TransferData timing with the desktop executor at
   500 ms, reserving the 3000 ms policy for the final CRC request.
8. Made every JSON result, including `ports` and setup/connect failures, one
   newline-delimited object on stdout while diagnostics remain on stderr.
   Per-line script global options are now rejected explicitly instead of being
   accepted and discarded. Bare `ports` does not request a session.
9. Added parser, command, formatter, session, and driver coverage using focused
   test seams. Production `fastecu-bench` links the production runtime, while
   fakes remain in test-only Bazel targets.

The intentional battery-voltage limitation remains: the current transport
interface cannot provide real vbatt, and the checklist continues to say so.

## Verification

- `bazel test --config=release //apps/bench/...` — passed all 7 test targets.
- `bazel test --config=release //apps/bench/... //src/platform/desktop/common/transport:all //:portable_closure //:serial_compat_allowlist` — passed all 12 targets after formatting.
- `bazel build --config=release //apps/bench:fastecu-bench` — passed; the binary
  was not run.
- `prek run --all-files` — passed every hook.
- `git diff --check` — passed.
- `bazel test --config=release //src/algorithms/protocol/colt:test_mitsu_colt_can_protocol`
  — one known pre-existing failure remains in
  `erase_and_write_redirect_routines_match_reflash_dir_checksums`: the erase
  redirect array is 160 bytes while the test expects 192. This blocker was
  already documented in the bench checklist before this correction wave and is
  unchanged by these edits.

## Safety and scope notes

- `fastecu-bench` was never executed because a physical J2534 adapter may be
  attached.
- All test paths use fakes or the existing scripted transport; CI verification
  cannot touch hardware.
- The production binary was successfully analyzed and linked by Bazel, which
  also enforces that it cannot depend on Bazel `testonly` fake targets.
- No unrelated refactor or transport-interface expansion was included.
