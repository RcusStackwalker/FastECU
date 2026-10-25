# CAN cancellation migration results

[ADR 0025](../adr/0025-enforce-cancellation-in-a-guarded-transport.md) sets the
migration requirements. The [flash contract](flash-contracts.md#colt-cancellation-ownership)
owns implemented boundaries; the [Colt results](colt-cancellation-centralization-results.md)
retain the original pilot evidence.

## Shared test support

Task 1, 2026-10-25: direct execution tests and shared CAN runners now honor an
executor's existing `CancellationGuard` alias. The bounded selection proof takes
the actual named executor's alias and delegates its `TransportSetup`, then uses
`BindFlashAttempt` for one Write followed by one Read. A separate never-cancelled
token isolates the scripted delegate from the real token cancelled after an
accepted Write. Attempts are counted before delegation. The proof requires one
accepted Write, zero inner Reads, a remaining queued reply, `kCancelled`, and
exactly one Configure/Open/Close. The mixed-CAN template repeats that contract
independently in raw and ISO-15765 modes and checks forwarded mode transitions;
its instantiation remains for the mixed-CAN migration.

This is synthetic selection/wiring evidence, not a family's protocol execution.
No production executor, checkpoint, transport, clock, UdsClient, wire byte,
address guard, or lifecycle policy changes here. Non-opted-in direct execution
still accepts the blind inner Read and succeeds. The synthetic guarded probe
newly refuses that Read after its accepted Write; it is not a claim that a real
family's existing post-operation checkpoints are redundant.

### Verification and preservation

Source baseline: `24b0db79b3e0dd0ee86425343186a38dd0561fa2`; implementation review
base: `c28be51f615f7cfc409d46dbf13dae5f29f21f37` (intervening documentation only).
The controller's baseline flash gate passed 87/87 targets; full baseline passed
303 targets with 20 skipped. Task 1 retained all predecessor test cases and
assertions. Its focused candidate XML has 1,186 cases in 70 predecessor targets,
plus three new cases (1,189 total); no separate pre-edit case XML snapshot was
captured for this task.

- RED: `bazel test --config=release //src/backend/flash/testing:guarded_attempt_expectations_test`
  failed compilation on missing adapter/expectation definitions after target generation.
- GREEN, repeated after restoring both mutations:
  `bazel test --config=release //src/backend/flash/testing:guarded_attempt_expectations_test //src/backend/flash/ecu/... //src/backend/flash:bound_flash_attempt_test`
  passed all 71 targets, with three new adapter/runner cases passing.
- Runner bypass: delegating directly from BoundAttempt caused
  `BoundAttemptUsesSelectedGuard` to fail: one inner Read, consumed queued reply,
  and success instead of `kCancelled`.
- Adapter bypass independently caused `DirectOptInRefusesReadAfterAcceptedWrite`
  to fail on the same extra-Read/result/queued-reply assertions. The unguarded
  direct path and bound probe still passed. Both mutations were restored.
- `python3 scripts/gazelle_check.py --fix` regenerated the two managed BUILD files;
  new support libraries are explicitly test-only. Final regeneration, buildifier,
  buildifier-lint, scoped formatting, and `git diff --check` passed.

### Measured cost and qualification

`python3 scripts/nloc_loop.py measure --json` before any source edit reported
152,811 total / 68,434 production / 84,377 test NLOC. After fixture review:
153,180 total / 68,434 production / 84,746 test. Delta from the source baseline:
production **0**, test **+369**. Of the added test cost, 284 NLOC is support
(19 adapter, 257 bounded expectations, 8 shared-runner growth) and 85 is the new
regression suite. The initial implementation at `0331d650` had added 405 test
NLOC; consolidating its duplicated CAN exchange and blind transport removed
36 NLOC. The direct and bound proofs now share those fixtures while retaining
all three cases and assertions. Overall support growth remains; no family test
compression or production savings are claimed.

Review round 1 repeated the focused shared target before and after consolidation,
then bypassed runner guarding and adapter guarding independently. Only
`BoundAttemptUsesSelectedGuard` failed for the runner bypass; only
`DirectOptInRefusesReadAfterAcceptedWrite` failed for the adapter bypass. Each
observed an extra inner Read, a consumed queued reply, and success instead of
`kCancelled`. After restoring both, all three cases passed again. Gazelle removed
the regression target's now-unused direct scripted-transport dependency.

The [qualification matrix](../flash-qualification-matrix.md) and its family
checklists remain authoritative. No hardware was used, no family is newly opted
in, and hardware status and outstanding bench evidence remain unchanged. Full
repository candidate gates run once before PR delivery, separately from these
per-task focused checks.
