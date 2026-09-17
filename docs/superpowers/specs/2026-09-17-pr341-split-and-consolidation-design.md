# PR #341 Split and Consolidation — Design

**Status:** Approved for implementation planning on 2026-09-17 against `master`
at `e0e264a2`. Subject is pull request #341,
`codex/flash-wave5-implementation`: 30 commits, 94 files, +21,248 / −8,399.

## Goal

Reduce the review and merge risk of #341 without changing what it delivers, and
raise the consistency of the code it adds.

Two things happen:

1. Two self-contained slices are peeled off #341 into their own pull requests
   and merged first. #341 rebases onto them.
2. The remaining #341 adopts shared facilities it currently bypasses, and drops
   duplication it currently introduces, before it merges.

Non-goals: no change to any wire sequence, no change to qualification status,
and no restructuring of the four family ports themselves. Every family keeps
its own executor, plan, types and characterization tests.

This follows the family-per-PR and port-then-factor rules of the
[step-5 tail flash drain design](2026-08-08-step5-tail-flash-drain-design.md)
and closes out work specified in the Wave 5 Denso SH705x CAN design. That
document, `docs/superpowers/specs/2026-09-02-step5-tail-wave5-denso-sh705x-can-design.md`,
is added by #341 itself and so is cited by path rather than linked until it
reaches `master`.

## Findings That Shape the Design

### The wave-5 plan already anticipated the split

The Wave 5 design names five review units — DensoCAN, TCU, petrol, diesel,
closeout — and states that each family change independently shrinks the drain,
so partial progress stays mergeable. The split below is therefore a return to
the intended shape, not a new decomposition.

### The transport contract changes are independent of every family

Outside #341, `ICanFlashTransport` has exactly two implementors:
`DesktopCanFlashTransport` and `ScriptedCanFlashTransport`. Both are already
updated inside the PR. Adding the pure-virtual `reset_connection()` therefore
ripples nowhere else, and the transport work can merge ahead of the families
that consume it.

### The K-Line header bug is not a wave-5 bug

`DesktopCanFlashTransport::configure()` did not clear ISO-14230 auto-header
state, so a CAN flash following a K-Line session on the shared
`SerialPortActions` facade inherited it. This affects the eight CAN families
already on `master`, not only the four #341 adds. It should not wait behind
21,000 lines of unrelated change.

### Executor bodies are genuinely divergent; only small helpers are not

Twenty-two function signatures are common to the three ISO-15765 wave-5
executors (`subaru_denso_sh7058_can_executor.cpp`,
`subaru_denso_sh7058_can_diesel_executor.cpp`,
`subaru_tcu_denso_sh705x_can_executor.cpp`). Comparing bodies:

| Verdict | Functions |
| --- | --- |
| Byte-identical in all three | `info`, `debug`, `error`, `cancelled_if_requested`, `elapsed_milliseconds`, `beef_request`, `encrypt_payload`, `channel_request_optional` |
| Identical in two only | `parse_beef` (petrol = diesel) |
| All three differ | `connect_bootloader` (201 lines), `flash_block` (113), `upload_kernel` (100), `write_memory` (72), `compare_blocks` (58), `read_memory` (49), `beef_exchange`, `upload_b6_discard`, `nonfatal_query`, `strict_payload`, `discard_stale_frame`, `query_crc`, `reflash_block` |

Every substantial protocol routine differs. The Wave 5 design forbids merging
divergent sequences, and that judgment holds. The extractable surface is eight
small helpers, about 56 unique lines.

### The larger win is reuse, not extraction

Facilities the wave-5 code needs already exist and were not used:

- `uds_client_exchange_common.h` defines `info()` and `error()` as templates
  constrained on a `WithEventSink` concept, with a header comment stating the
  template form exists so families with their own context shape can use them
  unchanged. All three ISO executors redeclare both privately; none includes
  the header. The header has no `debug()`, which wave-5 introduces.
- `src/backend/ports/testing/fake_cancellation_token.h` provides
  `FakeCancellationToken`. `NeverCancelled` is that type default-constructed;
  `ToggleCancellation` is that type plus `set_cancelled(true)`. Four new test
  files declare both locally.
- Commit `a90b648e` in this PR added `lifecycle_calls_` and per-call counters
  to `ScriptedCanFlashTransport`. Three new test files nevertheless define a
  `RecordingCanTransport` decorator that records lifecycle a second time,
  leaving two competing answers to "how do I assert transport lifecycle."

  These three decorators are **not** identical — 71, 85 and 123 lines, and
  they split two ways.

  The petrol and diesel copies share a core (the wrapped `scripted` transport,
  `reset_result`, `writes`, `read_timeouts`, `cancellation_to_trigger`,
  `cancel_prefix`, `cancellation_on_open`, all six overrides) and diverge only
  in additions that are **inert unless set**: petrol adds
  `cancellation_on_reset` and `cancellation_on_configure`, diesel adds an
  optional `timeline` pointer recorded on all four lifecycle methods plus
  `cancel_after_read_count` with its `read_count`. Their union is therefore
  behaviour-preserving for both.

  The TCU copy is **not** unionable and stays where it is. Its
  `restart_in_progress` flag is set unconditionally inside `reset_connection()`
  and then changes what `configure()` and `open()` do — `configure()` records
  into `restart_configs` and can return `restart_configure_result` without
  reaching the scripted transport. Its `write()` also classifies timeline
  entries by inspecting payload bytes (`kernel_start_write`, `kernel_id_write`)
  and its `read()` cancels after a kernel-start reply. None of that is inert by
  default, so folding it in would change petrol and diesel behaviour.

- `PhaseCancellingEventSink` is byte-identical across all three suites
  (22 lines each). `RecordingClock` is not — 11, 21 and 28 lines — so it stays
  family-local.

### Two blemishes worth fixing while the files are open

- `DesktopCanFlashTransport::restart_iso15765` is an override whose entire body
  calls the base implementation.
- `mainwindow.cpp` gained `goto ecu_operation_cleanup;` to skip flash routing
  after a handled TCU service action. The `denso_tcu_read_preflight` seam
  already returns a bool, so a guarded branch expresses this without a jump
  into a labelled tail.

## Design

### Part 1 — The two extracted pull requests

**PR A, `fix(flash): clear stale K-Line header state`.** Adds
`set_add_iso14230_header(false)` to `DesktopCanFlashTransport::configure()`
with its test. Taken from the `desktop_can_flash_transport.cpp` and
`desktop_can_flash_transport_test.cpp` hunks of commit `8f97613c`; that
commit's mixed-transport hunks stay with #341. Roughly 35 lines. Merges first
and independently, because it fixes a defect that predates the wave.

Two details the extraction must respect. Commit `8f97613c` also edits two
backend-operation-trace assertions that do not exist on `master` — they arrive
with PR B — so those hunks stay behind and PR A is a cherry-pick with a
resolved conflict, not a clean one. And the branch's
`configureSucceedsWhenEverySetterSucceeds` ends with `QVERIFY(result.has_value())`
written twice; PR A drops the duplicate while it is in that function.

**PR B, `feat(flash): CAN transport lifecycle and mixed raw/ISO contracts`.**
Contains commits `822ef869`, `b84bf2c5`, `395c1e75`, and the interface hunks of
`a90b648e`:

- `ICanFlashTransport::reset_connection()` (pure virtual) and
  `restart_iso15765()` (defaulted, cancellation-checked at each step)
- `before_transport_configure()` on the K-Line, CAN and mixed executor
  interfaces, invoked from `BoundAttempt`
- `RawCanConfig`, `MixedCanConfig`, `IMixedCanFlashTransport`,
  `IMixedCanFlashExecutor`, `TransportKind::CanRawIso15765`
- `ScriptedMixedCanFlashTransport`; lifecycle recording on
  `ScriptedCanFlashTransport`
- `DesktopCanFlashTransport::reset_connection()`,
  `DesktopMixedCanFlashTransport` and its tests

Roughly 1,900 lines, almost entirely new files or additive hunks.

**Accepted cost.** PR B lands `IMixedCanFlashTransport` and
`DesktopMixedCanFlashTransport` with no production consumer until DensoCAN
merges with #341. This dead-code interval is accepted deliberately. The
alternative is that the only novel architecture in the wave is reviewed as an
appendix to four protocol ports. The `//:portable_closure` guard and the
transport's own tests constrain it meanwhile.

**What stays in #341.** The four families and their plans, executors, types and
tests; the `FlashWorkflowFactory` routes; the four legacy family removals;
`denso_tcu_read_preflight`; the `mainwindow` rewiring; the drain-script,
portable-target-registry, qualification-matrix and plan or design doc updates.
#341 rebases onto A and B and loses roughly 2,000 lines across 12 files.

### Part 2 — Consolidation inside #341, before merge

Scope rule: this work touches only files #341 already changes. Any cleanup
reaching files outside that set is deferred, so the diff stays about one
subject.

**Tier 1 — adopt what already exists.**

1. Delete the six private `info()` / `error()` definitions across the three ISO
   executors; include `uds_client_exchange_common.h` and use the shared
   templates. Add `debug()` to that header in the same `WithEventSink` template
   shape, beside its siblings.
2. Replace `NeverCancelled` and `ToggleCancellation` in the four new test files
   with `FakeCancellationToken`. `NeverCancelled` becomes a default-constructed
   token; `ToggleCancellation`'s `cancel()` becomes `set_cancelled(true)`.
   Roughly 96 lines removed.
3. Add `RecordingCanFlashTransport` to `src/backend/flash/ecu/testing/`,
   carrying the core the petrol and diesel decorators share plus the union of
   their inert-by-default additions (`cancellation_on_reset`,
   `cancellation_on_configure`, `cancellation_on_open`,
   `cancellation_to_trigger` with `cancel_prefix`, `cancel_after_read_count`
   with `read_count`, and the optional `timeline` pointer). Its `lifecycle`
   vector is dropped in favour of the `ScriptedCanFlashTransport` recording
   added by `a90b648e`, so one answer replaces two.

   Both those decorators are then deleted — roughly 71 of 156 lines. The TCU
   decorator is left untouched for the reason recorded above; a comment on it
   records why it does not use the shared type, so the next reader does not
   re-derive the analysis.

4. Move `PhaseCancellingEventSink` into the same shared testing header and
   delete the three identical copies. It is constructor-injected
   (`PhaseCancellingEventSink(FakeCancellationToken&, std::string phase, int done)`
   after item 2) and cancels only when `event.phase_name` and `event.done` both
   match, so it moves verbatim. Roughly 44 lines removed.
5. Delete the `DesktopCanFlashTransport::restart_iso15765` override. Replace
   the `mainwindow.cpp` `goto` with a guarded branch over the existing bool
   return.

**Tier 2 — extract the proven helpers.** Add
`src/backend/flash/ecu/denso_beef_can_common.h`, beside the existing data-only
cluster header `src/backend/flash/ecu/denso_iso15765_can_common.h`, holding the
helpers verified byte-identical across all three ISO executors:
`info`/`debug`/`error` land in `uds_client_exchange_common.h` per Tier 1, and
`cancelled_if_requested`, `elapsed_milliseconds`, `beef_request`,
`encrypt_payload` and `channel_request_optional` land here.

`parse_beef` stays local to each family. It matches petrol and diesel only, and
a two-way match is where the Wave 5 design says to stop.

The new header follows the convention of its neighbour: it carries a comment
naming its consumers, stating what was compared, and recording that the
divergent routines in the table above were examined and deliberately left
family-local. Executor test suites keep their independently transcribed wire
expectations and do not read the shared helpers back.

**Tier 3 — explicitly deferred.**

- Unifying `KernelBackedCanFlashWorkflow`, `SimpleCanFlashWorkflow` and
  `ColtWorkflow`. The new template is the old one plus lazy kernel resolution,
  a confirmation loop and a transport parameter, and `ColtWorkflow` hand-rolls
  the same confirmation loop; `flash_workflow.cpp` is now 1,014 lines with
  seven sibling classes. Unifying them touches routing for every already-merged
  CAN family. The Wave 5 design considered widening `single_window_plan` and
  declined. This needs its own change and its own risk budget.
- Expressing each family's `nonfatal_query` through the shared
  `non_fatal_query`. All three differ from each other and from the shared
  helper; converging them is behaviour-changing surgery on characterization-
  tested sequences. Investigate before committing to it.
- Sweeping `NeverCancelled` from the two pre-existing wave-4 test files and
  `RecordingClock` from the six that predate this PR. Outside #341's file set.

## Sequencing

1. PR A merges.
2. PR B merges.
3. #341 rebases onto `master`; its extracted commits drop out as already
   applied.
4. Tier 1 and Tier 2 land as commits on #341.
5. #341 merges, reducing `//:legacy_flash_drain` from 14 families to 10.

PR A and PR B are independent of each other and may merge in either order;
listing A first only reflects that it is the smaller and more urgent.

## Testing

No new test strategy. The wave's characterization suites are the safety net for
every consolidation step, and none of that work may change a wire expectation:

- Tier 1 items 1 and 5 are pure substitutions; the suites must pass unchanged.
- Tier 1 items 2, 3 and 4 rewrite test scaffolding. Each file's assertions stay
  byte-identical; only the double behind them changes. A step that requires
  editing an expectation is a signal the substitution is not equivalent, and
  stops.
- Tier 2 runs the three executor suites plus
  `denso_iso15765_can_common_test` green before and after, as the
  behaviour-preserving baseline the Wave 5 plan's factoring task established.

Gate for each pull request, per the repository's standard:

```sh
bazel test  --config=release --nocache_test_results //...
bazel build --config=release //:fastecu //:portable_closure
bazel test  --config=release //:serial_compat_allowlist //:legacy_flash_drain //:windows_preprocessor_guards
bazel run   //:clang_tidy_report_changed
prek run --all-files
```

## Risks

| Risk | Mitigation |
| --- | --- |
| PR B's unconsumed mixed-CAN types linger if #341 stalls | Accepted knowingly; the interval is bounded by #341, which is already written and passing |
| A "pure substitution" silently changes behaviour | Any consolidation step that requires editing a test expectation stops and is reported, not adjusted |
| Tier 2 extraction hides a future divergence between families | Header names its consumers and records what was compared; suites keep independently transcribed expectations and never read the helpers back |
| Rebasing #341 onto A and B resolves conflicts wrongly | The extracted hunks are additive and file-local; after rebase, the diff against pre-rebase #341 must be empty outside the extracted files |
| Splitting is read as a qualification claim | No qualification row changes; all four families stay automated-only and experimental |
