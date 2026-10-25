# CAN Cancellation Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend guarded cancellation refusal to all CAN flash executors through individually reviewed opt-in, including CAN EEPROM and mixed raw-CAN/ISO-15765.

**Architecture:** Keep `BoundAttempt`'s executor-selected decorator and the existing `GuardedCanFlashTransport` contract. Adopt it explicitly for each concrete executor; use an empty default policy with explicit plan-type specializations for the four shared Denso bootloader instantiations. Add one mixed-CAN decorator and preserve existing executor/helper checkpoints during this first rollout; poll deletion and test compression are subsequent independently assessed changes.

**Tech Stack:** Qt 6 desktop composition; portable C++23 backend; GoogleTest/Google Mock; Bazel pinned by `.bazelversion`; Gazelle; prek.

**Execution:** Subagent-driven development and PR delivery, explicitly authorized by the user.

**Spec:** [ADR 0025](../../adr/0025-enforce-cancellation-in-a-guarded-transport.md), with the implemented [Colt contract](../../reference/flash-contracts.md#colt-cancellation-ownership), [pilot lessons](../../reference/guarded-transport-pilot-lessons.md), and [Colt results](../../reference/colt-cancellation-centralization-results.md).

## Global Constraints

- “Migration is explicit per executor through the existing `CancellationGuard` opt-in at BoundAttempt.”
- “The guard returns delegated results unchanged.” No post-call check, error normalization, persistent stopped state, or final cancellation sweep.
- “Only explicit token polls shown redundant by the migrated executor's boundary audit may be removed.” This plan removes none; retain diagnostic text, error precedence, post-operation checkpoints and checks before non-I/O work.
- “Protocol validation, abort/continue decisions, and plan confirmations stay with their current owners.” Preserve tolerated errors, best-effort operations, wire bytes, timing and address guards.
- “Lifecycle, in-session restart, mode changes, buffer control and RequestUnblock retain their existing owners and cancellation behavior.” Keep setup/close/unblock on the owned transport.
- “K-Line and cancellation deferral remain outside this decision's scope.” Keep UdsClient and clocks unchanged; add no expected-silence or critical-section API.
- “Test compression remains separately assessed” and “centralization alone establishes neither net savings nor redundant tests.” Retain predecessor tests and report production/test support cost separately.
- Backend remains portable: no Qt, owned threads or direct filesystem access. Use `Result`/`Status.has_value()`, existing error kinds, byte types and warning-as-error rules.
- Regenerate managed BUILD files using `python3 scripts/gazelle_check.py --fix`; use existing visibility groups and package-owned tests.
- Consult the [qualification matrix](../../flash-qualification-matrix.md) and applicable family checklist; record outstanding bench evidence without claiming hardware qualification.

## Review Focus

1. Cancellation between responsePending reads must refuse another inner Read without resending the request: Task 2 `PendingReadCancellationDoesNotDelegateAnotherRead`.
2. Cancellation coincident with a failed read must preserve differing family precedence: Task 14 `ReadFailureWinsOverConcurrentCancellation` and Task 15 `CancellationStillWinsOverConcurrentReadFailure`.
3. Late cancellation during tolerated stop/close work must preserve the family's result while refusing further I/O: Task 7 `CancellationAtBestEffortStopRetainsReadResult`.
4. A cancelled restart or cleanup must still reach its original owner and close once: existing `GuardedCanFlashTransport.LifecycleRestartAndUnblockForwardAfterCancellation` owner coverage and BoundAttempt close/unblock tests remain mandatory; Task 13 retains the real TCU restart integration.
5. Mixed-CAN raw traffic must be covered independently of ISO-15765, including cancellation concurrent with an accepted result or transport error: Task 17's guard outcome matrix and two-mode runner proof.

---

## File structure and evidence ownership

- `src/backend/flash/guarded_can_flash_transport.{h,cpp}`: existing CAN refusal policy; unchanged.
- Create `src/backend/flash/testing/execute_with_cancellation_guard.h`: test-only direct-execution adapter, selecting the executor's guard when declared and otherwise retaining the unguarded path.
- Create `src/backend/flash/testing/guarded_attempt_expectations.h`: two bounded runner expectations using the actual selected guard type; no transcript parser, scenario generator or new production framework.
- Create `src/backend/flash/testing/guarded_attempt_expectations_test.cpp`: adapter and probe tests; generated test target `//src/backend/flash/testing:guarded_attempt_expectations_test`.
- `src/backend/flash/ecu/testing/can_executor_run.h` and `can_executor_conformance.h`: route existing direct Execute calls through the adapter, without changing their assertions.
- Each migration changes its executor header, its existing executor test, generated BUILD dependencies and one evidence section. Implementation `.cpp` files are audit inputs and stay unchanged.
- `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`: empty default `DensoIso15765CancellationPolicy<Plan>` plus explicit migrated specializations; no blanket alias on the shared executor.
- Create `src/backend/flash/guarded_mixed_can_flash_transport.{h,cpp}` and `_test.cpp`: mixed-CAN decorator and owner tests, integrated with its sole executor in Task 17.
- Create `docs/reference/can-cancellation-migration-results.md`: durable boundary audit, preservation evidence, measurements and outstanding qualification evidence, organized by delivered change rather than a copied capability inventory.
- Update `docs/reference/flash-contracts.md`, ADR 0025 and documentation navigation to describe implemented adoption and link to its evidence. Keep `#colt-cancellation-ownership` and the historical Colt results intact.

Baseline for planning: `23b16c128ef068c888a50014a66bd6533fc870b4`, plus this session's uncommitted ADR revision. Recheck drift at execution; do not modify or stage unrelated untracked plans/specs. Deliver the ADR, this plan's original content and its execution through retained history. No completed plan deletion in the same squash that first introduces it.

## Shared migration procedure (Tasks 2–17)

Each task is its own reviewable commit. Tasks 2–16 depend on Task 1; Tasks 8–10 also depend on Task 7. Execute sequentially, with focused tests after each; full gates precede PR delivery.

For each migration's **Audit** step, read the listed executor `.cpp` and shared helpers, find all Execute callers with `rg`, and append a boundary table to the results document. Record admission, preparation, each I/O path, waits, events, retries, after-call checks, final results and cleanup; mark every existing poll retained. Record new refusal sites introduced by guard entry, including a raw/pending Read previously protected only by the inner transport. Test-only cancellation-blind ports expose this change without claiming a hardware defect.

For each **Tests** step, retain all predecessor cases and add `SelectedGuardIsUsedByBoundAttempt` calling the Task 1 expectation with a valid existing family plan. That expectation uses a tiny probe executor that takes the actual family's `CancellationGuard` type but performs only Write then Read, without clocks, UdsClient or executor polls. Its blind inner port cancels after accepted Write. This proves the chosen type and generic runner wiring even when a real family's retained checkpoints mask bypass. Add a real-family BoundAttempt test at an unmasked boundary where available; distinguish it from the synthetic selection proof in the evidence. A real-family cancellation test alone is not a bypass proof.

Preserved-behavior characterizations must first pass on the unguarded baseline. Tests that expect a newly refused blind-inner Read are guard regressions: capture their extra-Read failure on the baseline, then require them to pass after opt-in. Do not mislabel the intended guard boundary change as preserved behavior. The new selection proof must fail to compile because `Executor::CancellationGuard` is absent (or fail its behavior assertion when guard selection is intentionally bypassed). This is the RED gate; do not claim already-passing characterizations are RED evidence.

For each **Opt in** step, add `public: using CancellationGuard = GuardedCanFlashTransport;` and its include in the concrete header, or the explicit Denso specialization stated in that task. Replace direct executor test calls with `testing::ExecuteWithCancellationGuard(...)` and use the Task 1 adapter in existing shared runners. Do not change production checkpoints or protocol helpers. Regenerate BUILD files and review only relevant dependency changes.

For each **Verify** step, run the exact family target stated in that task plus `//src/backend/flash/testing:guarded_attempt_expectations_test`, `//src/backend/flash:bound_flash_attempt_test` and `//src/backend/flash:guarded_can_flash_transport_test`. Expected: all tests pass, predecessor cases remain, no warnings. Confirm invalid-plan-before-precancel and family abort tests still run. For each new guard declaration, remove only that declaration temporarily: the selection proof must fail. Also temporarily bypass guard construction in BoundAttempt: the shared probe must fail on an extra inner Read. Restore both changes and rerun focused tests before committing.

For each **Record/commit** step, append commands, boundary observations, predecessor/candidate XML case counts, declaration/bypass mutation outcomes, source revision and `python3 scripts/nloc_loop.py measure` production/test deltas to the results document. Record that polls and hardware status are retained, update current contract wording to link to the evidence, run `git diff --check`, and stage only that task's named paths, generated dependencies and documentation. Commit as `refactor: guard <family> CAN cancellation`. Do not stage directories containing unrelated work.

### Task 1: Make direct test execution and runner proofs honor opt-in

**Files:** Create the three test support files above; modify `src/backend/flash/ecu/testing/can_executor_run.h`, `src/backend/flash/ecu/testing/can_executor_conformance.h`, and their generated BUILD files. Create the results document and link it from the current contract.

**Interfaces:** In `fastecu::flash::testing`, produce:

```cpp
template<class Executor>
Result<FlashExecutionResult> ExecuteWithCancellationGuard(
    Executor& executor, const FlashPlan& plan, typename Executor::TransportType& transport,
    IClock& clock, const ICancellationToken& cancellation, IEventSink& events);
template<class Executor> void ExpectCanGuardedAttempt(const FlashPlan& plan);
template<class Executor> void ExpectMixedCanGuardedAttempt(const FlashPlan& plan);
```

The adapter borrows the transport and constructs `Executor::CancellationGuard` around Execute only if present. Both expectations select that actual alias and invoke a local probe via `BindFlashAttempt`; the probe's TransportSetup delegates to the named executor. CAN expects one accepted Write, zero delegated Reads, a queued reply, exactly one Configure/Open/Close and `kCancelled`. Mixed expects the same independently for raw and ISO-15765, with the normal mode transition forwarded. Blind inner ports must pass a separate never-cancelled token to any scripted delegate; counters count attempts before delegation. Do not alter existing scripted transports' cancellation behavior.

- [ ] **Tests:** Add `DirectOptInRefusesReadAfterAcceptedWrite`, `DirectNonOptInKeepsItsPath` and `BoundAttemptUsesSelectedGuard`. Use a local CAN executor/probe and a real ManualCancellationToken, not poll counts. Assertions: guarded `EXPECT_EQ(readAttempts, 0)` and `IsErr(kCancelled)`; unguarded `EXPECT_EQ(readAttempts, 1)` and `IsOk()`; one accepted Write and one Close for the bound probe.
- [ ] **RED:** Run `bazel test --config=release //src/backend/flash/testing:guarded_attempt_expectations_test`; expect missing adapter/expectation definitions. Generate the target first if needed to get a source/compiler failure rather than an unknown-label failure.
- [ ] **Implement:** Write the adapter and bounded probes; replace raw Execute calls inside existing shared test runners with the adapter. Do not migrate a production executor in this task. Mixed templates need not be instantiated until Task 17.
- [ ] **GREEN:** Run `bazel test --config=release //src/backend/flash/testing:guarded_attempt_expectations_test //src/backend/flash/ecu/... //src/backend/flash:bound_flash_attempt_test`. Existing non-opted-in families must retain their results. Bypass runner guarding and confirm the probe fails; restore and rerun.
- [ ] **Record/commit:** Start the evidence record with baseline measurements and source revision; commit only the support, regenerated BUILD files and linked evidence as `test: exercise executor-selected cancellation guards`.

### Task 2: Opt in Hitachi M32R ECU

**Files:** Modify `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.h`, `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruHitachiM32rCanExecutor>(const FlashPlan&)`; produces `SubaruHitachiM32rCanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit `FatalRequest`, OBK probe, read/write loops and best-effort close in the executor. Keep both loop polls.
- [ ] **Characterize:** Add `PendingReadCancellationDoesNotDelegateAnotherRead`: queue a responsePending reply, cancel after that accepted Read using a blind inner port, and assert `EXPECT_EQ(readAttempts, 1)` and `IsErr(ErrorKind::kCancelled)` with no resend. Use the existing UDS policy and framed reply bytes; script one queued positive reply to prove it remains unread. Run `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_m32r_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruHitachiM32rCanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_m32r_can_executor_test` plus the shared verification targets. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.h`, named generated BUILD files and documentation only.

### Task 3: Opt in Hitachi M32R TCU

**Files:** Modify `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.h`, `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruTcuHitachiM32rCanExecutor>(const FlashPlan&)`; produces `SubaruTcuHitachiM32rCanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit the raw erase exchange as well as UDS traffic, 1 ms read pacing and the read/write loop polls. Keep all three polls and the nonstandard erase reply policy.
- [ ] **Characterize:** Retain rejected-erase/no-data tests. Add `AcceptedEraseCancellationStopsBeforeData`: inject cancellation on the accepted erase reply, assert the existing accepted-erase event sequence and `EXPECT_EQ(dataWriteAttempts, 0)`; derive exact logs from this executor, not Colt. This is a characterization, not the bypass proof. Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruTcuHitachiM32rCanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_hitachi_m32r_can_executor_test` plus the shared verification targets. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.h`, named generated BUILD files and documentation only.

### Task 4: Opt in CVT Hitachi M32R

**Files:** Modify `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.h`, `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruTcuCvtHitachiM32rCanExecutor>(const FlashPlan&)`; produces `SubaruTcuCvtHitachiM32rCanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit raw alive/erase exchanges and `ExchangeCvtB6Chunk`; keep read/write polls, 0x7e1/0x7e9 envelopes and content-blind B6 acknowledgment handling.
- [ ] **Characterize:** At a B6 write, cancel from the accepted Write in a blind inner transport and assert `EXPECT_EQ(chunkReplyReadAttempts, 0)` and `IsErr(ErrorKind::kCancelled)`; preserve all existing silent/unchecked-reply cases. Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruTcuCvtHitachiM32rCanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_executor_test` plus the shared verification targets. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.h`, named generated BUILD files and documentation only.

### Task 5: Opt in CVT Mitsubishi MH8111

**Files:** Modify `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.h`, `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruTcuCvtMitsuMh8111CanExecutor>(const FlashPlan&)`; produces `SubaruTcuCvtMitsuMh8111CanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit the shared B6 exchange, raw stop path and tolerated close replies; keep both loop polls, its geometry and timeout values.
- [ ] **Characterize:** At a B6 write, cancel from accepted Write and assert zero chunk-reply Read attempts plus `IsErr(ErrorKind::kCancelled)`; retain final checksum and unchecked close-reply cases. Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8111_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruTcuCvtMitsuMh8111CanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8111_can_executor_test` plus the shared verification targets. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.h`, named generated BUILD files and documentation only.

### Task 6: Opt in CVT Mitsubishi MH8104

**Files:** Modify `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.h`, `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruTcuCvtMitsuMh8104CanExecutor>(const FlashPlan&)`; produces `SubaruTcuCvtMitsuMh8104CanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit local raw exchanges, content-blind B7 reads and B6 writes; keep both loop polls and this family's separate 500 ms B6 receive timeout.
- [ ] **Characterize:** Cancel after the first accepted B6 Write with a blind inner transport: `EXPECT_EQ(chunkReplyReadAttempts, 0)` and `IsErr(ErrorKind::kCancelled)`. Keep malformed-but-tolerated reply and addressed write tests. Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8104_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruTcuCvtMitsuMh8104CanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8104_can_executor_test` plus the shared verification targets. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.h`, named generated BUILD files and documentation only.

### Task 7: Opt in Denso N83M 1.5M

**Files:** Modify `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`, `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/denso_iso15765_bootloader_executor.cpp` and `src/backend/flash/ecu/denso_iso15765_can_common.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruDenso1n83m_1_5mCanExecutor>(const FlashPlan&)`; produces `SubaruDenso1n83m_1_5mCanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit `denso_iso15765_bootloader_executor.cpp` and `denso_iso15765_can_common.cpp`, including direct channel calls and six best-effort stop/close attempts. Keep the shared read/write polls and unpaced-read behavior.
- [ ] **Characterize:** Add `CancellationAtBestEffortStopRetainsReadResult`: cancel when the sink receives `Sending stop command`, after the full dump; assert `IsOk()`, unchanged padded/decrypted ROM, `EXPECT_EQ(stopWriteAttempts, 0)` and no extra inner Read. This pins tolerated cancellation without adding a final sweep. Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruDenso1n83m_1_5mCanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** First define empty `template<class Plan> struct DensoIso15765CancellationPolicy {};` and make `DensoIso15765BootloaderExecutor<Plan>` publicly inherit it in addition to its existing FamilyExecutor base. Add only `DensoIso15765CancellationPolicy<SubaruDenso1n83m_1_5mCanPlan>` specialization exposing `using CancellationGuard = GuardedCanFlashTransport;` before the shared executor definition. Do not add a guard alias to the shared executor itself. The definitions must be visible to both explicit-instantiation .cpp and all alias-header users; do not specialize in alias headers. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test` plus the shared verification targets. Also run `//src/backend/flash/ecu:denso_iso15765_bootloader_executor_test` and `//src/backend/flash/ecu:denso_iso15765_can_common_test`. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp`, `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`, named generated BUILD files and documentation only.

### Task 8: Opt in Denso N83M 4M

**Files:** Modify `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`, `src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/denso_iso15765_bootloader_executor.cpp` and `src/backend/flash/ecu/denso_iso15765_can_common.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruDenso1n83m_4mCanExecutor>(const FlashPlan&)`; produces `SubaruDenso1n83m_4mCanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit the same shared bootloader/read helpers, including this profile's tolerant dump setup and bench discarded reply. Keep the shared polls and all family profile values.
- [ ] **Characterize:** Retain tolerated wrong/absent dump-setup replies. Add cancellation after a setup Write with a blind inner port: `EXPECT_EQ(setupReplyReadAttempts, 0)` and `IsErr(ErrorKind::kCancelled)`; do not convert cancellation into tolerated absence. Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruDenso1n83m_4mCanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add only `DensoIso15765CancellationPolicy<SubaruDenso1n83m_4mCanPlan>` specialization exposing `using CancellationGuard = GuardedCanFlashTransport;` before the shared executor definition. Do not add a guard alias to the shared executor itself. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test` plus the shared verification targets. Also run `//src/backend/flash/ecu:denso_iso15765_bootloader_executor_test` and `//src/backend/flash/ecu:denso_iso15765_can_common_test`. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp`, `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`, named generated BUILD files and documentation only.

### Task 9: Opt in Denso SH72531

**Files:** Modify `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`, `src/backend/flash/ecu/subaru_denso_sh72531_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/denso_iso15765_bootloader_executor.cpp` and `src/backend/flash/ecu/denso_iso15765_can_common.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruDensoSh72531CanExecutor>(const FlashPlan&)`; produces `SubaruDensoSh72531CanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit the shared helpers and SH-specific page pacing; keep the shared polls, 1 ms Sleep and bench discard.
- [ ] **Characterize:** Add cancellation during the final page pacing wait: `IsErr(ErrorKind::kCancelled)` and `EXPECT_EQ(stopWriteAttempts, 0)`. Preserve the existing clock outcome even though the last page was accepted; do not copy N83M's late-success rule. Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh72531_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruDensoSh72531CanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add only `DensoIso15765CancellationPolicy<SubaruDensoSh72531CanPlan>` specialization exposing `using CancellationGuard = GuardedCanFlashTransport;` before the shared executor definition. Do not add a guard alias to the shared executor itself. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh72531_can_executor_test` plus the shared verification targets. Also run `//src/backend/flash/ecu:denso_iso15765_bootloader_executor_test` and `//src/backend/flash/ecu:denso_iso15765_can_common_test`. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_denso_sh72531_can_executor_test.cpp`, `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`, named generated BUILD files and documentation only.

### Task 10: Opt in Denso SH72543 diesel

**Files:** Modify `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`, `src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/denso_iso15765_bootloader_executor.cpp` and `src/backend/flash/ecu/denso_iso15765_can_common.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruDensoSh72543CanDieselExecutor>(const FlashPlan&)`; produces `SubaruDensoSh72543CanDieselExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan()` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit DID F182 identity handling, tolerant bench session, shared page pacing and the 2000 ms profile timeouts. Keep shared polls and nonfatal identity policy.
- [ ] **Characterize:** Retain tolerated bench/identity failures. Cancel after an accepted pending response on a direct receive path; assert no additional delegated Read and `IsErr(ErrorKind::kCancelled)` at the next fatal exchange, preserving any preceding nonfatal log events. Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh72543_can_diesel_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruDensoSh72543CanDieselExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add only `DensoIso15765CancellationPolicy<SubaruDensoSh72543CanDieselPlan>` specialization exposing `using CancellationGuard = GuardedCanFlashTransport;` before the shared executor definition. Do not add a guard alias to the shared executor itself. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh72543_can_diesel_executor_test` plus the shared verification targets. Also run `//src/backend/flash/ecu:denso_iso15765_bootloader_executor_test` and `//src/backend/flash/ecu:denso_iso15765_can_common_test`. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor_test.cpp`, `src/backend/flash/ecu/denso_iso15765_bootloader_executor.h`, named generated BUILD files and documentation only.

### Task 11: Opt in Denso SH7058 BEEF petrol

**Files:** Modify `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.h`, `src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruDensoSh7058CanExecutor>(const FlashPlan&)`; produces `SubaruDensoSh7058CanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `PlanFor(kVariants.front(), FlashOperation::kRead)` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit `BeefCanSession`, `denso_beef_can_common.h`, raw B6 reply discard, stale CRC drain and preparation hooks. Keep all shared/helper and three startup polls.
- [ ] **Characterize:** Add cancellation after a successful B6 upload Write: zero raw discard Read attempts and `IsErr(ErrorKind::kCancelled)`. Preserve tolerance of malformed discard content and ordinary stale-read failures; cancellation/disconnect stay terminal. Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruDensoSh7058CanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test` plus the shared verification targets. Also run `//src/backend/flash/ecu:beef_can_session_test` and `//src/backend/flash/ecu:denso_beef_can_common_test`. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.h`, named generated BUILD files and documentation only.

### Task 12: Opt in Denso SH7058 BEEF diesel

**Files:** Modify `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.h`, `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruDensoSh7058CanDieselExecutor>(const FlashPlan&)`; produces `SubaruDensoSh7058CanDieselExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `PlanFor(kVariants.front(), FlashOperation::kRead)` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit the same BEEF helpers plus the diesel preparation hook; keep shared polls and the executor's startup poll.
- [ ] **Characterize:** Pin raw stale-frame draining cancellation independently of framed exchanges: cancel after the CRC reply and assert `EXPECT_EQ(staleReadAttempts, 0)` and `IsErr(ErrorKind::kCancelled)`. Keep existing CRC and tolerated-drain tests. Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruDensoSh7058CanDieselExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test` plus the shared verification targets. Also run `//src/backend/flash/ecu:beef_can_session_test` and `//src/backend/flash/ecu:denso_beef_can_common_test`. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp`, `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.h`, named generated BUILD files and documentation only.

### Task 13: Opt in Denso SH705x BEEF TCU

**Files:** Modify `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h`, `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruTcuDensoSh705xCanExecutor>(const FlashPlan&)`; produces `SubaruTcuDensoSh705xCanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `ReadPlan(kCases.front())` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit BEEF helpers, custom kernel-ID continuation/drains, read-only preflight and confirmation boundary. Keep shared polls and existing drain bounds.
- [ ] **Characterize:** Retain `KernelIdTrailingDrainPropagatesCancellationAndDisconnect`, exact session validation and no-upload-after-failure tests. At accepted B6 Write, cancel and assert zero discard Read attempts and no following B6/37 request. Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruTcuDensoSh705xCanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test` plus the shared verification targets. Also run `//src/backend/flash/ecu:beef_can_session_test` and `//src/backend/flash/ecu:denso_beef_can_common_test`. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h`, named generated BUILD files and documentation only.

### Task 14: Opt in Hitachi SH7058

**Files:** Modify `src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor.h`, `src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruHitachiSh7058CanExecutor>(const FlashPlan&)`; produces `SubaruHitachiSh7058CanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `write plan from existing ActiveKernelWritesAll4096Frames test` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit `Session::Checkpoint`, `Exchange`, erase polling and programming. Retain all pre/post checks, including the after-read check after transport error handling; retain write-only admission.
- [ ] **Characterize:** Add `ReadFailureWinsOverConcurrentCancellation`: in a blind read implementation cancel the token and return `Fail(ErrorKind::kDisconnected, "read failed")`; assert `IsErrWith(ErrorKind::kDisconnected, "read failed")` and no erase/data commands. Add successful-read cancellation characterization with the retained `"SH7058 write cancelled"` detail. Run `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_sh7058_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruHitachiSh7058CanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_sh7058_can_executor_test` plus the shared verification targets. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor.h`, named generated BUILD files and documentation only.

### Task 15: Opt in Hitachi SH72543R

**Files:** Modify `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.h`, `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<SubaruHitachiSh72543rCanExecutor>(const FlashPlan&)`; produces `SubaruHitachiSh72543rCanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `Plan() from existing Sh72543rExecutor fixture` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit `Session::Send`, `Receive`, `Sleep`, pre-encryption, erase/program/finalize checkpoints. Retain all checks; its Receive checks cancellation before interpreting a delegated error.
- [ ] **Characterize:** Add `CancellationStillWinsOverConcurrentReadFailure`: the inner Read cancels then returns `Fail(ErrorKind::kDisconnected, "read failed")`; assert `IsErrWith(ErrorKind::kCancelled, "flash cancelled")` and no following request. Retain finalization retry and complete-write tests. Run `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_sh72543r_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<SubaruHitachiSh72543rCanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/ecu:subaru_hitachi_sh72543r_can_executor_test` plus the shared verification targets. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor_test.cpp`, `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.h`, named generated BUILD files and documentation only.

### Task 16: Opt in Denso SH705x CAN EEPROM

**Files:** Modify `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.h`, `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor_test.cpp`, generated BUILD dependencies, and the shared results/contract documents. Audit `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp`.

**Interfaces:** Consumes Task 1 adapter and `ExpectCanGuardedAttempt<DensoSh705xEepromCanExecutor>(const FlashPlan&)`; produces `DensoSh705xEepromCanExecutor::CancellationGuard` as `GuardedCanFlashTransport`. Valid plan: `valid read plan from existing KernelAlreadyRunningSkipsBootloaderMatchesLegacyTrace test` from this suite; do not reuse another family's plan.

- [ ] **Audit:** Audit `CanRawExchange`, `SendKernelBlocks`, `ReadPageData`, `ReadEepromPage`, connection/upload/read loops and the port's restart forwarding contract. Keep every pre/post poll, header-only final B6 frame, short-page tolerance, inter-page delay and workflow retry staging.
- [ ] **Characterize:** Replace the existing `CancelOnCheck(75)` injection with a ManualCancellationToken cancelled after the first accepted B6 chunk Write; pin the same first-chunk cancellation outcome and zero chunk-ack Read attempts. Restart forwarding is already covered at its guard owner; retain that test and close-once runner tests rather than duplicating them in EEPROM, which never restarts. Run `bazel test --config=release //src/backend/flash/eeprom:denso_sh705x_eeprom_can_executor_test` before opting in; preserved-behavior characterizations pass, while newly guarded blind-inner reads fail on an extra delegated Read as specified in the shared procedure.
- [ ] **RED:** Add `SelectedGuardIsUsedByBoundAttempt` with `ExpectCanGuardedAttempt<DensoSh705xEepromCanExecutor>(*plan)`. Run the same target; expect a missing CancellationGuard diagnostic.
- [ ] **Opt in:** Add the public guard alias and include to the named executor header, following the shared procedure. Route all direct suite calls through the adapter; regenerate and review BUILD dependencies.
- [ ] **Verify:** Run `bazel test --config=release //src/backend/flash/eeprom:denso_sh705x_eeprom_can_executor_test` plus the shared verification targets. Run and restore the declaration/bypass mutations from the shared procedure.
- [ ] **Record/commit:** Follow the shared evidence procedure; commit `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor_test.cpp`, `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.h`, named generated BUILD files and documentation only.

### Task 17: Implement mixed-CAN guarding and opt in DensoCAN

**Files:** Create `src/backend/flash/guarded_mixed_can_flash_transport.h`, `.cpp`, `_test.cpp`; modify `src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.h` and `_test.cpp`, generated BUILD dependencies, the evidence record and current contract/navigation. Audit the existing DensoCAN `.cpp`; preserve all its checks and mode sequencing.

**Interfaces:** Produce `GuardedMixedCanFlashTransport final : public IMixedCanFlashTransport`, constructed as `explicit GuardedMixedCanFlashTransport(IMixedCanFlashTransport& inner) noexcept`. Implement every existing port method with its exact signature from `flash_executor.h`: ResetConnection, Configure, Open, Close, EnterRawBootloaderMode, ClearReceiveBuffer, EnterIso15765KernelMode, RequestUnblock, WriteIso15765, ReadIso15765, WriteRaw and ReadRaw. The four I/O methods alone check their supplied token before delegation. Add `SubaruDensoSh705xDensoCanExecutor::CancellationGuard = GuardedMixedCanFlashTransport`.

- [ ] **Audit:** Record raw wake/upload/ack paths, ISO probe/read/write/CRC paths, nonterminal initial probe outcomes, after-call checks, post-erase recovery warning and startup hooks. Read the family's matrix entry and checklist before implementation.
- [ ] **Owner tests:** Write `PrecancelledIoDoesNotDelegate` for all four I/O methods: `IsErr(kCancelled)` and zero inner attempts. Write `DelegatedOutcomeAndArgumentsAreUnchanged` for success, absent reply, present-empty bytes, actual raw frame and every existing error kind, including cancellation raised inside the delegate: preserve bytes/frame id/extended flag, timeout, original token reference, kind and detail. Write `ControlOperationsStillDelegateAfterCancellation`, checking every non-I/O method and exact returned outcomes. Write `FailureDoesNotPoisonNextCall`. Use blind local ports and deterministic tokens; no threads.
- [ ] **RED:** Generate the managed targets and run `bazel test --config=release //src/backend/flash:guarded_mixed_can_flash_transport_test`; expect missing guard definitions.
- [ ] **Implement guard:** Borrow the inner transport and forward every method. Entry refusal details are `mixed CAN ISO-15765 write cancelled before delegation`, `mixed CAN ISO-15765 read cancelled before delegation`, `mixed CAN raw write cancelled before delegation`, and `mixed CAN raw read cancelled before delegation`. Preserve delegated outcomes without post-checks.
- [ ] **GREEN guard:** Rerun the owner test; all cases pass. Extend the Task 1 mixed expectation instantiation to prove raw and ISO-15765 refusal independently, with one close per attempt.
- [ ] **Characterize and RED opt-in:** Add `SelectedGuardIsUsedByBoundAttempt` invoking `ExpectMixedCanGuardedAttempt<SubaruDensoSh705xDensoCanExecutor>(*plan)` using an existing valid family plan. First run the existing real-family startup, transition, cancellation and post-erase recovery tests unchanged. Then run `bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh705x_densocan_executor_test`; the selection proof fails on the absent alias.
- [ ] **Opt in:** Add the concrete alias/include and route direct suite calls through the adapter. Keep pre/post checks in IsoWrite/IsoRead/RawWrite/RawRead and all loop/setup checks. Regenerate BUILD files and review their dependencies.
- [ ] **Verify:** Run the mixed owner and family tests, shared runner tests, and existing scripted-transport tests. Removing the executor alias must fail the selection proof; bypassing BoundAttempt must fail on an extra raw or ISO Read. Restore both and rerun. Cancellation after an accepted write must still produce the family's existing after-write detail; concurrent write/read errors retain precedence.
- [ ] **Record/commit:** Record preservation, both-mode bypass evidence, measured support cost and outstanding qualification evidence, update implemented contract scope while retaining the Colt anchor/history, and commit named paths as `refactor: guard mixed CAN flash cancellation`.

## Delivery gates and PR

- [ ] Run `python3 scripts/gazelle_check.py --fix`; review generated changes and confirm a second invocation produces no new diff.
- [ ] Run `bazel build --config=release //:fastecu` and `bazel test --config=release //...`; expected: success with no failed tests or warning opt-outs.
- [ ] Run `prek run --all-files` and `bazel run //:clang_tidy`; expected: all applicable checks pass. Run `prek run lychee --files docs/superpowers/plans/2026-10-25-can-cancellation-ownership.md` while the new plan is untracked as well as the all-files gate.
- [ ] Run `bazel coverage --config=coverage //...` using the host setup documented in [tooling](../../tooling.md), record coverage and retained predecessor cases, and run `python3 scripts/nloc_loop.py measure` for final production/test/cumulative support cost. No claim of savings without measurements.
- [ ] Review the complete diff against ADR 0025 and the evidence record. Verify all 16 additional concrete executors opt in explicitly, including four Denso specializations, EEPROM and mixed CAN; Colt stays guarded; K-Line and UdsClient remain untouched. Review preparation, cancellation/result precedence, confirmation and address guards.
- [ ] Update the ADR's current implementation wording and documentation index to link to the implemented CAN contract/evidence. Keep outstanding Colt compression and bench work visible and hardware qualification status unchanged. Preserve this completed plan in delivered history before a later deletion.
- [ ] Stage named task paths only, commit any final evidence updates, push the feature branch and create a PR. The PR describes explicit CAN adoption, retained polls, synthetic selection proofs versus real-family evidence, measured support cost, validation and outstanding hardware evidence. Record CI results and address failures before reporting delivery.

## Self-review

All ADR requirements map to the shared procedure, task-level opt-ins, mixed owner tests and delivery gates. The five review-focus conditions each have a named owning task. The two test helpers separate guard wiring from family checkpoints, avoiding false guard-bypass evidence. The Denso policy is empty by default and specialized in one shared definition site to preserve per-instantiation adoption without ODR differences. No task removes polls, compresses suites or changes hardware status. Execution method: subagent-driven development, authorized by the user with PR delivery after verification.
