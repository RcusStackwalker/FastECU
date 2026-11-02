# CAN Cancellation Stack Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver PR #908 inside an atomically merged stacked-PR sequence that centralizes guarded CAN cancellation and leaves both production and test NLOC below the stack's `master` baseline.

**Architecture:** Update ADR 0025 separately, retain #908 as a conservative audited rollout, then replace per-executor opt-in with one production execution seam selected by executor/transport interface. Move generic refusal coverage to the guard owner, remove redundant family tests and pre-I/O polls, and finish with a compact durable evidence matrix.

**Tech Stack:** C++23, Qt 6, Bazel, GoogleTest/GoogleMock, `gh stack`, Prek, Lizard NLOC, Mull mutation testing.

**Spec:** `docs/superpowers/specs/2026-10-31-can-cancellation-stack-cleanup-design.md`

## Global Constraints

- Work in an isolated worktree created with `superpowers:using-git-worktrees`; preserve the current checkout's unrelated untracked files.
- The recorded `origin/master` trunk commit is the hard behavior and NLOC baseline. Rebase means remeasure and record a new trunk baseline; retain the `c078e458` comparison for #908 continuity.
- Preserve wire bytes, ordering, timeouts, delays, retry counts, returned errors, error precedence, logs, progress, events, confirmations and family-owned checkpoints.
- Guard only CAN and mixed-CAN I/O. K-Line, `UdsClient`, lifecycle, restart, mode changes, buffer control and `RequestUnblock` retain their current owners.
- Backend exceptions never cross ports; check `Result`/`Status` with `.has_value()`.
- Never widen Bazel visibility or add warning suppressions.
- Stage named paths only. Do not push, force-update #908, mark PRs ready or merge without explicit user authorization at that execution stage.
- The final stack must be below its trunk baseline in both production and test NLOC and must recover more than #908's measured +157 production and +1,640 test NLOC.
- If safe in-scope cleanup cannot satisfy both NLOC gates, stop and return to ADR review; do not broaden into unrelated deletion.

## Review Focus

- Cancellation raised during a delegated I/O must preserve the delegate's result and family error precedence; Tasks 3 and 5 retain owner and real-family precedence tests.
- Cancellation after a successful operation must still reach retained post-operation checkpoints before any family-owned continuation; Tasks 4 and 5 retain those named cases.
- K-Line must remain undecorated after interface-level dispatch; Task 3 adds a K-Line production-seam test.
- Mixed CAN must guard raw and ISO-15765 reads and writes while still forwarding lifecycle/mode operations; Tasks 2 and 3 retain both-mode owner/binding tests.
- Rebase onto a moving trunk must not invalidate the NLOC claim; Tasks 1 and 7 pin and remeasure the exact trunk commit.

---

### Task 1: Establish the isolated stack and standalone ADR branch

**Files:**
- Create: `docs/superpowers/specs/2026-10-31-can-cancellation-stack-cleanup-design.md`
- Create: `docs/superpowers/plans/2026-10-31-can-cancellation-stack-cleanup.md`
- Modify: `docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md`
- Modify: `docs/reference/flash-contracts.md`
- Modify: `docs/README.md`

**Interfaces:**
- Consumes: `origin/master` and the approved design in this checkout.
- Produces: branch `docs/can-cancellation-guard-decision`, the recorded trunk SHA/NLOC baseline, and ADR 0025's durable completion contract.

- [ ] **Step 1: Create an isolated worktree from the current trunk**

Invoke `superpowers:using-git-worktrees`. Fetch `origin`, record `git rev-parse origin/master`, create the worktree and branch `docs/can-cancellation-guard-decision`, and confirm the new worktree is clean. Do not reuse the current checkout or its local `docs/can-cancellation-ownership` branch.

- [ ] **Step 2: Record the exact trunk baseline**

Run:

```sh
python3 scripts/nloc_loop.py measure --json
git rev-parse HEAD
```

Expected: JSON containing production/test/total NLOC and a SHA equal to the selected `origin/master`. Save both in the ADR PR description draft; do not add a status ledger to the repository.

- [ ] **Step 3: Add the approved design and this plan to the ADR branch**

Recreate the two approved files at the exact paths above in the isolated worktree. Preserve their contents verbatim from the source checkout.

- [ ] **Step 4: Update ADR 0025 to define the durable end state**

Keep ADR 0025 as the owner. Replace language that makes explicit per-executor aliases permanent with:

- per-family audits are migration evidence;
- `BoundAttempt`-owned execution selects the guard by executor/transport interface after all CAN implementations are audited;
- generic next-I/O refusal is tested at the guarded transport;
- family suites retain precedence, continuation, post-operation and wire behavior;
- migration completes only after redundant polls/tests are removed and production/test NLOC are both below the recorded trunk baseline.

Do not claim the rollout is already implemented in this PR.

- [ ] **Step 5: Align the flash contract and documentation index**

Update `docs/reference/flash-contracts.md` and `docs/README.md` to link to ADR 0025's proposed durable contract. Do not add executor inventories or implementation status.

- [ ] **Step 6: Verify the documentation-only branch**

Run:

```sh
prek run lychee --all-files
git diff --check
git diff --stat origin/master...HEAD
```

Expected: link checking and whitespace checks pass; the scoped review contains only the five named documentation artifacts.

- [ ] **Step 7: Commit the ADR layer**

```sh
git add docs/README.md docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md docs/reference/flash-contracts.md docs/superpowers/specs/2026-10-31-can-cancellation-stack-cleanup-design.md docs/superpowers/plans/2026-10-31-can-cancellation-stack-cleanup.md
git commit -m "docs: decide durable CAN cancellation ownership"
```

Do not push yet.

### Task 2: Reconstruct PR #908 as the conservative rollout layer

**Files:**
- Modify: the 52 paths currently changed by PR #908, excluding ADR/strategy claims assigned to Task 1.
- Preserve: `docs/superpowers/plans/2026-10-25-can-cancellation-ownership.md`
- Preserve: `docs/reference/can-cancellation-migration-results.md`

**Interfaces:**
- Consumes: Task 1's ADR branch and source PR head `9d0295d03a453b07a8f6ecc64f67a02fe9e1d112`.
- Produces: rewritten branch `refactor/can-cancellation-ownership`, still associated with PR #908, containing a behavior-preserving safety rollout.

- [ ] **Step 1: Preserve a local recovery reference**

Confirm the remote #908 head is still the expected SHA before rewriting. Create a local backup reference named `backup/pr-908-before-stack-9d0295d0`. If the remote head changed, stop and re-review the new commits instead of overwriting them.

- [ ] **Step 2: Create the rewritten #908 branch on the ADR branch**

Create a temporary reconstruction branch from `docs/can-cancellation-guard-decision`. Transplant the implementation/test commits from #908 in their original order, excluding:

- `ec114418` and `abdc6dc1` decision-only setup;
- merge commit `0f44b89f`;
- implementation-status wording in `3bce81a9` and `9d0295d0` that conflicts with Task 1's proposed ADR state.

Retain and reconcile the code/test commits from `205e5185` through `d3b93fac`, the mixed-CAN implementation `8aab26ab`, and convention fixes `631cb067`/`f2aa35b9`. Recreate durable migration evidence from `b1adfbde`, `eb18acec`, `3bce81a9` and `9d0295d0` without reintroducing self-authorization.

- [ ] **Step 3: Make #908's documentation state its actual scope**

The PR layer must say:

- it is the conservative audited rollout;
- it intentionally retains polls and predecessor tests;
- its measured growth is not savings or completed ownership transfer;
- the stacked cleanup layers own alias removal, test compression and poll deletion;
- no hardware qualification is claimed.

Do not edit ADR 0025 in this layer except conflict resolution needed to retain Task 1 verbatim.

- [ ] **Step 4: Verify the reconstructed behavior baseline**

Run:

```sh
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py
bazel test --config=release //src/backend/flash:guarded_can_flash_transport_test //src/backend/flash:guarded_mixed_can_flash_transport_test //src/backend/flash:bound_flash_attempt_test //src/backend/flash/testing:guarded_attempt_expectations_test //src/backend/flash/ecu/... //src/backend/flash/eeprom/...
python3 scripts/nloc_loop.py measure --json
git diff --check
```

Expected: Gazelle's second run produces no diff, focused tests pass, and the measurement reproduces #908's directionally positive production/test result. Record exact reconstructed deltas against Task 1's trunk baseline.

- [ ] **Step 5: Compare reconstructed #908 with the original PR**

Use `git range-diff` and a scoped tree diff against `9d0295d0`. Confirm that differences are limited to the extracted decision layer, removal of the merge commit, documentation reconciliation and conflict adaptation. Any production or test behavioral delta requires explicit review before continuing.

- [ ] **Step 6: Replace the local #908 branch without pushing**

Move local `refactor/can-cancellation-ownership` to the verified reconstruction while retaining the backup ref. Do not force-update the remote until the complete local stack is green and the user authorizes submission.

### Task 3: Centralize guard selection at the production execution seam

**Files:**
- Modify: `src/backend/flash/flash_executor.h`
- Create: `src/backend/flash/guarded_flash_execution.cpp`
- Modify: `src/backend/flash/BUILD.bazel` (Gazelle-managed output)
- Modify: `src/backend/flash/bound_flash_attempt_test.cpp`
- Delete: `src/backend/flash/testing/execute_with_cancellation_guard.h`
- Delete: `src/backend/flash/testing/guarded_attempt_expectations.h`
- Delete: `src/backend/flash/testing/guarded_attempt_expectations_test.cpp`
- Modify: `src/backend/flash/testing/BUILD.bazel` (Gazelle-managed output)
- Modify: all executor headers currently declaring `CancellationGuard`
- Modify: all executor tests currently calling `ExecuteWithCancellationGuard` or defining `SelectedGuardIsUsedByBoundAttempt`

**Interfaces:**
- Consumes: `ICanFlashExecutor`, `IMixedCanFlashExecutor`, `IKlineFlashExecutor` and their transport interfaces from `flash_executor.h`.
- Produces: three overloads named `ExecuteWithTransportGuard`, each returning `Result<FlashExecutionResult>` and taking its executor interface, `const FlashPlan&`, matching transport interface, `IClock&`, `const ICancellationToken&`, and `IEventSink&`.

- [ ] **Step 1: Write failing production-path binding tests**

In `bound_flash_attempt_test.cpp`, add:

- `CanAttemptUsesTransportGuardWithoutExecutorAlias`: a cancellation-blind CAN transport cancels after an accepted write; the following read is not delegated.
- `MixedCanAttemptUsesTransportGuardForRawAndIsoWithoutExecutorAlias`: raw and ISO subcases each cancel after an accepted write and refuse the following matching read.
- `KlineAttemptExecutesWithoutCanGuard`: a K-Line probe still receives its original transport and completes its existing script.

The probe executors must not declare `CancellationGuard`.

- [ ] **Step 2: Run the tests to prove the old alias mechanism fails**

Run:

```sh
bazel test --config=release //src/backend/flash:bound_flash_attempt_test --test_output=errors
```

Expected: the alias-free CAN and mixed-CAN cases delegate the forbidden read and fail; the K-Line case passes.

- [ ] **Step 3: Declare the three production overloads**

In `flash_executor.h`, after the executor interface declarations, declare:

```cpp
Result<FlashExecutionResult> ExecuteWithTransportGuard(
    ICanFlashExecutor& executor, const FlashPlan& plan,
    ICanFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events);
Result<FlashExecutionResult> ExecuteWithTransportGuard(
    IMixedCanFlashExecutor& executor, const FlashPlan& plan,
    IMixedCanFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events);
Result<FlashExecutionResult> ExecuteWithTransportGuard(
    IKlineFlashExecutor& executor, const FlashPlan& plan,
    IKlineFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events);
```

Change `BoundAttempt::Run` to call this overload set exactly once after Open and before Close.

- [ ] **Step 4: Implement the overloads without a header cycle**

In `guarded_flash_execution.cpp`, include `flash_executor.h`, `guarded_can_flash_transport.h`, and `guarded_mixed_can_flash_transport.h`. CAN constructs `GuardedCanFlashTransport`, mixed CAN constructs `GuardedMixedCanFlashTransport`, and K-Line calls the executor directly.

- [ ] **Step 5: Make the binding tests pass**

Run the Task 3 focused test again. Expected: all three cases pass, including raw and ISO mixed-CAN subcases.

- [ ] **Step 6: Remove transitional production opt-in**

Delete every executor `CancellationGuard` alias/include and delete `DensoIso15765CancellationPolicy` plus its four identical specializations. `git grep -n 'CancellationGuard' -- src/backend/flash` must find only historical comments if any; no production selection marker remains.

- [ ] **Step 7: Remove duplicated test dispatch and synthetic selection proofs**

Delete the three test-support files listed above. Remove every `SelectedGuardIsUsedByBoundAttempt` test and its includes/BUILD dependencies. Revert ordinary family tests and shared conformance helpers to direct undecorated `executor.Execute(...)`; retained composition cases invoke `BindFlashAttempt`.

- [ ] **Step 8: Regenerate and verify the binding layer**

Run:

```sh
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py
bazel test --config=release //src/backend/flash:bound_flash_attempt_test //src/backend/flash:guarded_can_flash_transport_test //src/backend/flash:guarded_mixed_can_flash_transport_test //src/backend/flash/ecu/... //src/backend/flash/eeprom/...
python3 scripts/nloc_loop.py measure --json
git diff --check
```

Expected: all tests pass; both production and test NLOC decrease from rewritten #908. Record the deletion ledger and deltas in the PR description draft.

- [ ] **Step 9: Commit the binding layer**

Stage the named production, test and generated BUILD paths only, then commit:

```sh
git commit -m "refactor: select CAN cancellation guard at execution boundary"
```

### Task 4: Compress generic family cancellation coverage

**Files:**
- Modify: the CAN/mixed-CAN executor test files changed by #908.
- Modify only if a real shared contract remains: `src/backend/flash/ecu/testing/can_executor_conformance.h`
- Modify: generated package `BUILD.bazel` files.

**Interfaces:**
- Consumes: Task 3's guard-owner and `BoundAttempt` binding proofs.
- Produces: family suites containing only family-owned cancellation semantics; no synthetic transport-selection framework.

- [ ] **Step 1: Pin the retained family-specific cases before deletion**

Run the named cases that protect behavior not owned by entry refusal:

- `CrcReplyCancellationStopsBeforeRawStaleDrain`
- `CancellationDuringFinalPagePacingRetainsClockFailure`
- `ReadFailureWinsOverConcurrentCancellation`
- `SuccessfulReadCancellationKeepsPostReadCheckpoint`
- `CancellationStillWinsOverConcurrentReadFailure`
- `AcceptedWriteCancellationKeepsPostWriteCheckpoint`
- `AcceptedEraseCancellationStopsBeforeData`
- `AcceptedB6WriteCancellationKeepsAfterChunkCheckpoint`
- `AcceptedWriteCancellationKeepsAfterWriteDetail`
- `ConcurrentIoFailureKeepsTransportErrorPrecedence`

Expected: each passes on Task 3. If a name changed during reconstruction, map it by exact assertion behavior rather than silently dropping it.

- [ ] **Step 2: Delete cases owned entirely by guarded entry refusal**

Delete new #908 cases whose sole distinguishing assertion is that a cancellation-blind inner transport did not receive the next read/write, including:

- the N83M 1.5M/4M OBK/setup refusal cases;
- petrol/diesel/TCU `AcceptedUploadWriteRefusesRawDiscardRead` cases;
- pending-identity and kernel-probe next-read cases;
- the three CVT `AcceptedB6WriteCancellationDoesNotDelegateChunkReply` copies;
- other new cases that assert no delegation without also pinning precedence, a post-operation checkpoint, tolerated continuation, or later family work.

For every deletion, name the surviving guard-owner or binding test in the PR deletion ledger.

- [ ] **Step 3: Consolidate only genuine real-family contracts**

If two or more retained cases have identical behavior but different executors, add one typed conformance test using each real executor and valid plan factory. Do not introduce synthetic executors or rebuild a selection framework. If parameter scaffolding is not smaller than the cases, keep the cases.

- [ ] **Step 4: Verify lossless family behavior and measure**

Run:

```sh
python3 scripts/gazelle_check.py --fix
bazel test --config=release //src/backend/flash:guarded_can_flash_transport_test //src/backend/flash:guarded_mixed_can_flash_transport_test //src/backend/flash:bound_flash_attempt_test //src/backend/flash/ecu/... //src/backend/flash/eeprom/...
python3 scripts/nloc_loop.py measure --json
git diff --check
```

Expected: all retained named cases and family targets pass; test NLOC falls materially; production NLOC is unchanged.

- [ ] **Step 5: Commit the test-ownership layer**

```sh
git commit -m "test: keep CAN cancellation coverage at its owner"
```

### Task 5: Remove redundant pre-I/O polls with family behavior frozen

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.cpp`
- Modify: applicable Denso ISO-15765/BEEF executor or shared-session sources present after reconstruction.
- Modify: `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.cpp`
- Modify: `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp`
- Modify: matching executor/shared-session tests and generated BUILD files.

**Interfaces:**
- Consumes: Task 3's guaranteed entry refusal and Task 4's reduced test ownership.
- Produces: no explicit cancellation check whose only effect is refusing the immediately following guarded I/O; all post-call and non-I/O semantics remain.

- [ ] **Step 1: Classify every candidate poll before editing**

For each `.Cancelled()` in CAN/mixed-CAN production sources, record one of:

- `remove`: immediately precedes guarded I/O and has no distinct log/error/continuation effect;
- `retain-post-operation`: observes cancellation after a delegated operation;
- `retain-precedence`: orders cancellation against an existing error;
- `retain-non-io`: guards a wait, confirmation, validation, progress event, loop computation or mode/lifecycle action;
- `retain-lower-layer`: belongs to restart/client/driver behavior outside this ADR.

Put only removed and retained boundary summaries in the PR deletion ledger; do not create a repository task diary.

- [ ] **Step 2: Remove shared Denso pre-I/O checks first**

Delete only `remove`-classified checks in shared Denso ISO-15765/BEEF helpers. Run their existing common/session tests plus all concrete family targets after each shared helper edit. Expected: transcripts and retained precedence/checkpoint cases are unchanged.

- [ ] **Step 3: Commit the shared-seam deletion**

After focused tests and NLOC measurement pass, commit:

```sh
git commit -m "refactor: let CAN guard own shared Denso I/O refusal"
```

- [ ] **Step 4: Remove family-local pre-I/O checks**

Apply the same classification to Hitachi, CVT, TCU, EEPROM and mixed-CAN family-local sources. Never remove a check merely because it is near I/O; its returned detail and ordering must be equivalent to the guard at that point.

- [ ] **Step 5: Delete tests whose last production checkpoint was removed**

For every removed poll, delete or rewrite only the test that solely asserted that checkpoint. Keep the Task 4 retained cases and all address-window, confirmation, lifecycle, retry, transcript and precedence tests.

- [ ] **Step 6: Run focused and mutation verification**

Run:

```sh
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py
bazel test --config=release //src/backend/flash/...
python3 scripts/mutation_test.py --diff-ref HEAD~2
python3 scripts/nloc_loop.py measure --json
git diff --check
```

Expected: all flash tests pass; changed cancellation lines have no newly surviving relevant mutants; production and test NLOC both decrease. If local Mull is unavailable, require the PR mutation job before marking this layer ready.

- [ ] **Step 7: Commit the family-local deletion**

```sh
git commit -m "refactor: let CAN guard own family I/O refusal"
```

### Task 6: Close the migration and preserve only durable evidence

**Files:**
- Modify: `docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md`
- Modify: `docs/reference/flash-contracts.md`
- Rewrite: `docs/reference/can-cancellation-migration-results.md`
- Modify: `docs/superpowers/specs/2026-10-18-size-reduction-strategy.md`
- Modify: `docs/README.md`
- Delete: `docs/superpowers/plans/2026-10-25-can-cancellation-ownership.md`
- Delete: `docs/superpowers/specs/2026-10-31-can-cancellation-stack-cleanup-design.md`
- Delete: `docs/superpowers/plans/2026-10-31-can-cancellation-stack-cleanup.md`

**Interfaces:**
- Consumes: final code/test ownership and measurements from Tasks 3–5.
- Produces: compact durable contract/evidence with completed temporary artifacts removed in a later PR layer.

- [ ] **Step 1: Replace the migration diary with a compact matrix**

Keep one row per executor or shared dialect containing: guard interface, removed refusal boundary, retained family checkpoints, surviving test owner, final NLOC contribution and qualification gap. Remove per-task command transcripts, repeated baseline prose and intermediate status snapshots.

- [ ] **Step 2: Reconcile ADR, contract, strategy and index**

Mark interface-level ownership implemented only if Tasks 3–5 passed. Record exact trunk and `c078e458` comparisons. Keep hardware status unchanged and link each durable fact to its single owning document.

- [ ] **Step 3: Delete completed design/plan artifacts**

Delete the three named temporary artifacts in this later branch. Confirm they exist in lower stack history before deletion, so atomic squash merge retains their original contents in earlier PR commits.

- [ ] **Step 4: Verify documentation and final NLOC gates**

Run:

```sh
prek run lychee --all-files
python3 scripts/nloc_loop.py measure --json
git diff --check
```

Expected: links pass; production and test NLOC are each below the exact Task 1 trunk baseline; cleanup recovered more than 157 production and 1,640 test NLOC from rewritten #908. If either condition fails, stop without submitting the stack as ready.

- [ ] **Step 5: Commit the closure layer**

```sh
git commit -m "docs: close CAN cancellation ownership migration"
```

### Task 7: Verify, submit and atomically merge the stack

**Files:**
- No intended source changes; verification fixes return to the owning lower branch and are rebased upward.

**Interfaces:**
- Consumes: all six local stack branches and explicit user authorization for remote mutation.
- Produces: linked draft PR stack, green ready stack, then one atomic squash merge when separately authorized.

- [ ] **Step 1: Initialize the local stack bottom-to-top**

Use `gh stack init` with these branches in order:

1. `docs/can-cancellation-guard-decision`
2. `refactor/can-cancellation-ownership` (existing PR #908)
3. `refactor/can-cancellation-guard-binding`
4. `test/can-cancellation-owner-compression`
5. `refactor/can-cancellation-poll-ownership`
6. `docs/can-cancellation-migration-closeout`

Run `gh stack view --short` and confirm every base relationship matches this order.

- [ ] **Step 2: Run the top-of-stack repository gates**

Run:

```sh
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py
bazel build --config=release //:fastecu
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy
bazel coverage --config=coverage //...
python3 scripts/mutation_test.py --diff-ref <recorded-trunk-sha>
python3 scripts/nloc_loop.py measure --json
git diff --check <recorded-trunk-sha>...HEAD
```

Expected: every gate passes, Gazelle converges, mutation output has no newly surviving relevant changed-line mutant, and both NLOC categories pass Task 6's gates.

- [ ] **Step 3: Run whole-stack review**

Review each PR diff against its parent and the complete stack against the recorded trunk. Confirm decision, rollout, binding, compression, poll deletion and closure do not leak across their assigned boundaries. Fix findings on the owning branch, then run `gh stack rebase` and repeat affected verification upward.

- [ ] **Step 4: Submit drafts only after explicit authorization**

After the user authorizes remote updates, run `gh stack submit` and preserve #908's existing PR association. Confirm the ADR PR is the bottom member and #908 targets it. Keep all new PRs draft until CI and bottom-up review complete.

- [ ] **Step 5: Make the full stack ready**

Use `gh stack submit --open` only after every member's CI passes and review feedback is propagated. Confirm `gh stack view --short` shows every PR open, non-draft, correctly based and mergeable.

- [ ] **Step 6: Atomically squash-merge only after final authorization**

After a separate user confirmation, run:

```sh
gh stack merge --yes --squash
```

Expected: GitHub merges the entire stack all-or-nothing, with one durable squash commit per PR. If any member fails branch protection or mergeability, none merge; repair the owning layer and resubmit rather than merging PRs individually.

## Self-review

- Spec coverage: every approved decision maps to Tasks 1–7; no behavior-normalization or hardware-qualification work entered scope.
- Step scan: branch construction, red/green binding, deletion classification, verification and remote authorization are separate checkable actions.
- Type consistency: all later tasks use the three `ExecuteWithTransportGuard` overloads declared in Task 3; ordinary family tests intentionally bypass that composition seam.
- Review Focus: concurrent delegated cancellation, post-operation checks, K-Line exclusion, mixed raw/ISO coverage and moving-trunk measurement each have an owning test/gate.
- Proportion: exact bodies are specified only for the small overload interface; family poll deletion remains rule-driven because preserving each existing precedence/checkpoint is the contract.
