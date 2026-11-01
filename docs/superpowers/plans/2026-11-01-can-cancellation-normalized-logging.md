# CAN Cancellation Normalized Logging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver PR #908 in an atomically merged stack that centralizes CAN cancellation, normalizes pre-delegation logging, removes 33 redundant polls and stays within the approved bounded-growth budget.

**Architecture:** Amend ADR 0025 separately, preserve the already reviewed conservative rollout/binding/test-ownership layers, then add typed cancellation I/O exposure before deleting the polls it makes redundant. Guards and `UdsClient` mark known refusal; successful Send boundaries promote later cancellation to may-have-started; shared reporters use that typed evidence instead of error text.

**Tech Stack:** C++23, Qt 6, Bazel, GoogleTest/GoogleMock, `gh stack`, Prek, `dedup_loop.py`, Mull mutation testing.

**Spec:** `docs/superpowers/specs/2026-11-01-can-cancellation-normalized-logging-design.md`

## Global Constraints

- The exact trunk baseline is `e62684af59f1bd3c98b20596ee0503327eea8a94`: 66,405 production and 88,783 test NLOC.
- The final stack must not exceed 66,409 production NLOC (`+4`) or 89,884 test NLOC (`+1,101`) against that trunk.
- A rebase onto newer trunk requires a new baseline while preserving maximum deltas of `+4` production and `+1,101` test NLOC unless another approved ADR changes them.
- Preserve wire bytes, accepted-operation ordering, timeouts, delays, retries, confirmations, progress, address windows, lifecycle behavior and family continuation policy.
- A typed pre-delegation refusal is `kCancelled`, delegates no I/O and emits no warning.
- Cancellation after a successful Send, or with unknown exposure, is may-have-started and retains the ECU safety warning.
- Do not infer exposure from error-detail strings and do not add an `ErrorKind`.
- Guard only CAN and mixed-CAN execution. K-Line, lifecycle, restart, mode changes, buffer control, `RequestUnblock`, waits and platform checks keep their owners.
- Generic refusal/exposure/binding tests belong to shared owners; family suites retain unique continuation, precedence, post-operation, transcript and lifecycle behavior.
- Backend exceptions never cross ports; inspect `Result`/`Status` with `.has_value()`.
- Never widen Bazel visibility or add warning suppressions.
- Stage named paths only. Do not push, force-update #908, mark PRs ready or merge without explicit authorization at that execution stage.
- The spike commits `a436a86c81094269a0760a96b8ca496bd7cb6d9b` and `88418ba48930f22b0eddf9d4230a039ddec9f19a` are implementation evidence, not delivery history; reproduce/review their diffs on the correct stack layers.

## Existing Reviewed Inputs

- ADR layer: `docs/can-cancellation-guard-decision` at `4b84179673aa9f3de9421273116488c942aad019`.
- Reconstructed #908 layer: `refactor/can-cancellation-ownership-stacked` at `d55cc27ddc8f46ea5e77a2ddba21f386604c6391`.
- Binding layer: `refactor/can-cancellation-guard-binding` at `d21b0007a0f7abc68b93562e8a361e7fdfcdd44f`.
- Test-ownership layer: `test/can-cancellation-owner-compression` at `e5465afd30e6308cf3d5231b2f2ef1d9da2143b5`.
- The narrow SH72543R deletion `fd963473370a400fdcda6034a42c19253dc511c0` belongs in the later poll-deletion layer.
- Tasks that produced those commits were independently reviewed; do not reimplement them. Rebase them after the ADR amendment and verify their parent-relative diffs remain equivalent.

## Review Focus

- Cancellation already present before Send/Write must be quiet and must not reach the delegate; Task 3 owns guard, `UdsClient` and reporter tests.
- Cancellation after a successful Send but before/during delay, Receive or pending reread must retain the may-have-started warning; Task 3 owns the promotion tests.
- A guarded Read refusal after a successful Write must not erase the earlier request exposure; Task 3 owns raw CVT Hitachi and MH8104 tests.
- Direct executor tests after poll deletion must use a cancellation-aware transport when entry refusal is part of the case; Task 4 owns the affected family sweeps/transcripts.
- K-Line and lifecycle methods must remain undecorated and behaviorally unchanged; Task 2 preserves the binding tests and Task 6 reviews the complete diff.

---

### Task 1: Amend ADR 0025 and preserve the revised decision in the bottom PR

**Files:**
- Create: `docs/superpowers/specs/2026-11-01-can-cancellation-normalized-logging-design.md`
- Create: `docs/superpowers/plans/2026-11-01-can-cancellation-normalized-logging.md`
- Modify: `docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md`
- Modify: `docs/reference/flash-contracts.md`
- Modify: `docs/README.md`
- Modify: `docs/superpowers/specs/2026-10-31-can-cancellation-stack-cleanup-design.md`

**Interfaces:**
- Consumes: approved revised design and pinned trunk measurements.
- Produces: ADR contract for `CancellationIoExposure`, the `+4`/`+1,101` hard budget, ownership-based test simplicity and a seven-layer atomic stack.

- [ ] **Step 1: Restore the bottom ADR branch in an isolated worktree**

Check out `docs/can-cancellation-guard-decision` without modifying any other managed worktree. Confirm its parent is the pinned trunk and its existing diff is the reviewed Task 1 diff.

- [ ] **Step 2: Update the decision documents**

Amend ADR 0025 in place. Replace the net-negative completion condition with the approved bounded-growth contract; define pre-delegation refusal and may-have-started cancellation; require typed evidence, quiet refusal, retained accepted-operation warning and all 33 poll removals. Mark the 2026-10-31 design as superseded by the new design without rewriting its historical findings.

- [ ] **Step 3: Update the current contract and index**

In `flash-contracts.md`, describe the proposed exposure field and reporter behavior without claiming implementation. In `docs/README.md`, point active work to the new design/plan and state that the older cleanup plan stopped at its size gate.

- [ ] **Step 4: Validate documentation**

Run:

```sh
prek run lychee --all-files
git diff --check
```

Expected: links pass; the ADR, design, plan, contract and index agree on the exact baseline, caps and behavior terminology.

- [ ] **Step 5: Commit the amended ADR layer**

```sh
git add docs/README.md docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md \
  docs/reference/flash-contracts.md \
  docs/superpowers/specs/2026-10-31-can-cancellation-stack-cleanup-design.md \
  docs/superpowers/specs/2026-11-01-can-cancellation-normalized-logging-design.md \
  docs/superpowers/plans/2026-11-01-can-cancellation-normalized-logging.md
git commit -m "docs: revise guarded CAN cancellation completion contract"
```

### Task 2: Restack the four reviewed implementation layers without semantic drift

**Files:**
- No intended source edits; resolve only documentation conflicts caused by Task 1.

**Interfaces:**
- Consumes: Task 1 ADR head and the four reviewed input commits.
- Produces: linear heads for reconstructed #908, binding cleanup and test ownership, with Task 4 head still measuring 66,526 production and 89,699 test NLOC.

- [ ] **Step 1: Rebase the reconstructed #908 layer**

Rebase `refactor/can-cancellation-ownership-stacked` onto the amended ADR head. Resolve documentation conflicts in favor of the revised ADR while retaining #908's conservative rollout evidence and its measured transitional growth. Compare the parent-relative source/test diff with `4b84179..d55cc27`; no new production or test behavior belongs here.

- [ ] **Step 2: Rebase the binding layer**

Rebase `refactor/can-cancellation-guard-binding` onto the new #908 head. Preserve the three `ExecuteWithTransportGuard` overloads, interface-selected CAN/mixed guards, undecorated K-Line and the reviewed Colt test fix.

- [ ] **Step 3: Rebase the test-ownership layer**

Rebase `test/can-cancellation-owner-compression` onto the new binding head. Preserve its deletion-only parent-relative source/test diff and all named family-specific continuation/precedence cases.

- [ ] **Step 4: Verify the restacked head**

Run:

```sh
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py
bazel test --config=release //src/backend/flash/...
python3 scripts/dedup_loop.py measure --json
git diff --check
```

Expected: 88 flash targets pass; Gazelle converges; production NLOC is 66,526 and test NLOC is 89,699. If rebasing changes those numbers, explain the exact upstream/source difference before continuing.

### Task 3: Add typed cancellation exposure and normalize CAN logging

**Files:**
- Modify: `src/backend/ports/error.h`
- Modify: `src/backend/ports/result_test.cpp`
- Modify: `src/backend/protocol/uds/uds_client.cpp`
- Modify: `src/backend/protocol/uds/uds_client_test.cpp`
- Modify: `src/backend/flash/guarded_can_flash_transport.cpp`
- Modify: `src/backend/flash/guarded_can_flash_transport_test.cpp`
- Modify: `src/backend/flash/guarded_mixed_can_flash_transport.cpp`
- Modify: `src/backend/flash/guarded_mixed_can_flash_transport_test.cpp`
- Modify: `src/backend/flash/ecu/uds_client_exchange_common.h`
- Modify: `src/backend/flash/ecu/uds_client_exchange_common.cpp`
- Modify: `src/backend/flash/ecu/uds_client_exchange_common_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`
- Modify: `docs/reference/flash-contracts.md`

**Interfaces:**
- Consumes: Task 2 guards, `UdsClient`, shared CAN reporter and interface-owned binding.
- Produces: `enum class CancellationIoExposure { kMayHaveStarted, kRefusedBeforeDelegation }`, defaulted `Error::cancellation_io_exposure`, and `Error AfterIoMayHaveStarted(Error)`.

- [ ] **Step 1: Create the exposure branch**

Create `refactor/can-cancellation-exposure` from the restacked Task 2 head and confirm the worktree is clean.

- [ ] **Step 2: Add failing typed-exposure tests**

Add/extend these owner tests using the exact names and semantics:

- `Error.CancellationExposureIsTypedAndConservative`: two-field `Error` defaults to `kMayHaveStarted`; refused exposure differs; `AfterIoMayHaveStarted` promotes only `kCancelled`.
- `UdsClientTest.PreservesSendRefusalButPromotesCancellationAfterSuccessfulSend`: a refused Send keeps pre-delegation exposure; cancellation from Receive after successful Send is promoted.
- `UdsClientTest.PromotesPreReadDelayCancellationAfterSuccessfulSend`: a cancelled delay after Send is may-have-started.
- Existing guard refusal tests assert `kRefusedBeforeDelegation` and no delegation for all two CAN and four mixed-CAN operations.
- `ReportExchangeFailureTest.KnownPredelegationCancellationReturnsWithoutLogging`: typed refusal returns unchanged with no event; default/may-have-started cancellation retains the warning.
- `RawExchangeCancellationWarningDependsOnRequestExposure` in both CVT Hitachi and MH8104: failed pre-delegation Send is silent; successful Send followed by cancellation warns.

- [ ] **Step 3: Run the red tests**

Run the eight focused targets listed in the spike report (result, `UdsClient`, both guards, shared reporter, CVT Hitachi, MH8104 and Colt).

Expected: compilation/assertion failures because exposure and promotion do not exist and old warning expectations remain.

- [ ] **Step 4: Implement the typed interface**

In `error.h`, add the exact enum, defaulted member and helper from the Interfaces block. Keep `ErrorKind::kCancelled` unchanged and preserve existing aggregate initializers.

- [ ] **Step 5: Stamp and promote exposure at its owners**

Mark `UdsClient` pre-request refusal and all guard entry refusals as `kRefusedBeforeDelegation`. Preserve failed-Send provenance. After a successful Send, pass delay/Receive/pending-reread cancellation through `AfterIoMayHaveStarted` before returning.

- [ ] **Step 6: Normalize shared and raw CAN reporting**

Make the shared reporter return pre-delegation cancellation without logging and retain the existing warning otherwise. Promote cancellation after successful Send in CVT Hitachi raw erase and MH8104 raw exchange. Route MH8104's private reporter through the shared reporter without changing non-cancellation text, fatality or retries. Update only the two Colt assertions whose next unsent request is now quiet.

- [ ] **Step 7: Run focused verification and measure the layer**

Run:

```sh
bazel test --config=release \
  //src/backend/ports:result_test \
  //src/backend/protocol/uds:uds_client_test \
  //src/backend/flash:guarded_can_flash_transport_test \
  //src/backend/flash:guarded_mixed_can_flash_transport_test \
  //src/backend/flash/ecu:uds_client_exchange_common_test \
  //src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_executor_test \
  //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8104_can_executor_test \
  //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py
python3 scripts/dedup_loop.py measure --json
git diff --check
```

Expected: all eight targets pass; production NLOC is 66,542 and test NLOC is 89,891 (`+16`/`+192` from Task 2).

- [ ] **Step 8: Commit the exposure layer**

Stage only the named paths and commit:

```sh
git commit -m "refactor: distinguish refused CAN cancellation"
```

### Task 4: Remove the 33 redundant polls and align their tests

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.cpp`
- Modify: `src/backend/flash/ecu/uds_client_exchange_common.cpp`
- Modify: `src/backend/flash/ecu/uds_client_exchange_common.h`
- Modify: `src/backend/flash/ecu/beef_can_kernel.cpp`
- Modify: `src/backend/flash/ecu/beef_can_kernel_test.cpp`
- Modify: `src/backend/flash/ecu/denso_beef_can_ecu_execution.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_hitachi_sh7058_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh705x_densocan_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp`
- Modify: `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp`
- Modify: `docs/reference/flash-contracts.md`

**Interfaces:**
- Consumes: Task 3 typed quiet refusal and may-have-started promotion.
- Produces: no explicit check whose sole effect is refusing the immediately following guarded/`UdsClient` I/O; all retained checkpoints keep their owners.

- [ ] **Step 1: Rebase the poll-deletion branch onto Task 3**

Rebase `refactor/can-cancellation-poll-ownership` so its existing commit `fd963473370a400fdcda6034a42c19253dc511c0` follows the exposure head. Preserve its reviewed SH72543R Receive behavior and confirm it is the only parent-relative change before continuing.

- [ ] **Step 2: Apply the spike's 33-site removal ledger**

Use the approved design and payoff report as the exact inventory. Remove 33 pre-I/O call sites across 14 production files plus the unused Denso TCU helper. Do not remove any item in the report's retention ledger. In particular, retain all post-Write/Read checks, waits, SH72543R Receive precedence, BEEF stale-drain/post-discard checks, EEPROM acknowledgements/page checks, restart/lifecycle checks and mixed-DensoCAN post-operation checks.

- [ ] **Step 3: Align only tests whose owned checkpoint changed**

Delete `BeefCanKernel.CancelledIfRequestedReportsTheCallerDetail`. Convert removed-entry detail assertions to kind/generic exposure assertions. Run diesel and SH72543R cancellation composition cases through `ExecuteWithTransportGuard` where production entry refusal is the subject. Preserve exact accepted I/O counts, script consumption, continuation, remaining details and post-operation warnings. Set the SH72543R cancellation sweep to its observed 58 guarded checks.

- [ ] **Step 4: Run focused and package verification**

Run the BEEF kernel, diesel executor, SH72543R and Hitachi TCU focused targets, followed by:

```sh
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py
bazel test --config=release //src/backend/flash/...
prek run --all-files
python3 scripts/mutation_test.py --diff-ref <task-3-head>
python3 scripts/dedup_loop.py measure --json
git diff --check
```

Expected: 88 flash targets pass; no relevant changed-line mutant survives; production NLOC is 66,409 and test NLOC is 89,884. Both values equal or beat the approved hard caps.

- [ ] **Step 5: Commit the deletion layer**

On `refactor/can-cancellation-poll-ownership`, stage only the named production, test and contract paths and commit:

```sh
git commit -m "refactor: let CAN I/O owners refuse cancellation"
```

### Task 5: Close the migration with durable evidence

**Files:**
- Modify: `docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md`
- Modify: `docs/reference/flash-contracts.md`
- Rewrite: `docs/reference/can-cancellation-migration-results.md`
- Modify: `docs/superpowers/specs/2026-10-18-size-reduction-strategy.md`
- Modify: `docs/README.md`
- Delete: `docs/superpowers/plans/2026-10-25-can-cancellation-ownership.md`
- Delete: `docs/superpowers/specs/2026-10-31-can-cancellation-stack-cleanup-design.md`
- Delete: `docs/superpowers/plans/2026-10-31-can-cancellation-stack-cleanup.md`
- Delete: `docs/superpowers/specs/2026-11-01-can-cancellation-normalized-logging-design.md`
- Delete: `docs/superpowers/plans/2026-11-01-can-cancellation-normalized-logging.md`

**Interfaces:**
- Consumes: implemented ownership, exposure, exact poll ledger, final NLOC and prior reviewed evidence.
- Produces: compact durable contract/evidence; temporary designs/plans remain recoverable from lower stack history.

- [ ] **Step 1: Create the closeout branch**

Create `docs/can-cancellation-migration-closeout` from the Task 4 head and confirm the worktree is clean.

- [ ] **Step 2: Replace the migration diary with a compact matrix**

Keep one row per executor/shared dialect with guard interface, removed refusal boundary, retained family checkpoints, surviving test owner, final NLOC contribution and qualification gap. Record pre-delegation versus may-have-started logging once at the shared owner; do not copy the spike transcript.

- [ ] **Step 3: Reconcile ADR, contracts, strategy and index**

Mark interface ownership and normalized logging implemented. Record exact baseline/final values and accepted deltas. State that this work did not achieve net-negative test NLOC and that the approved budget replaced that goal. Keep hardware status unchanged.

- [ ] **Step 4: Delete completed temporary artifacts**

Confirm every named design/plan exists in a lower stack commit, then delete it in this later branch. Do not delete durable ADRs, contracts or evidence.

- [ ] **Step 5: Validate closure**

Run:

```sh
prek run lychee --all-files
python3 scripts/dedup_loop.py measure --json
git diff --check
```

Expected: links pass; NLOC remains at or below 66,409 production and 89,884 test; all durable documents agree.

- [ ] **Step 6: Commit the closure layer**

Stage only the named documentation paths and commit:

```sh
git commit -m "docs: close CAN cancellation ownership migration"
```

### Task 6: Verify, review, submit and atomically merge the seven-PR stack

**Files:**
- No intended source changes; fixes return to their owning lower branch and are propagated upward.

**Interfaces:**
- Consumes: all seven local branches and explicit authorization for each remote mutation stage.
- Produces: green linked PRs and, only after separate authorization, an atomic squash merge.

- [ ] **Step 1: Confirm #908 and initialize the local stack**

Before any remote update, confirm PR #908's current head SHA and compare it with the reviewed historical head. Create a local backup ref named `backup/pr-908-before-stack-<short-sha>`. If the remote head changed, stop and review that new diff before force-updating it.

Initialize/verify this bottom-to-top order with `gh stack`:

1. `docs/can-cancellation-guard-decision`
2. the local reconstructed #908 branch, associated at submission with PR #908's existing remote branch
3. `refactor/can-cancellation-guard-binding`
4. `test/can-cancellation-owner-compression`
5. `refactor/can-cancellation-exposure`
6. `refactor/can-cancellation-poll-ownership`
7. `docs/can-cancellation-migration-closeout`

Run `gh stack view --short`; every base relationship must match this order.

- [ ] **Step 2: Run top-of-stack repository gates**

Run:

```sh
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py
bazel build --config=release //:fastecu
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy
bazel coverage --config=coverage //...
python3 scripts/mutation_test.py --diff-ref e62684af59f1bd3c98b20596ee0503327eea8a94
python3 scripts/dedup_loop.py measure --json
git diff --check e62684af59f1bd3c98b20596ee0503327eea8a94...HEAD
```

Expected: every gate passes; Gazelle converges; mutation has no relevant surviving changed-line mutant; production/test NLOC do not exceed 66,409/89,884.

- [ ] **Step 3: Run task and whole-stack reviews**

Review every PR against its parent, then the complete stack against pinned trunk. Pay special attention to conservative default exposure, Send-success promotion, raw exchange boundaries, direct-test transport assumptions, K-Line exclusion, the 33/retained checkpoint ledger and exact size caps. Fix findings on the owning branch and rebase/verify upward.

- [ ] **Step 4: Submit drafts only after explicit authorization**

After authorization, use `gh stack submit` to create/update drafts and preserve #908's PR identity. Confirm the standalone ADR PR is the bottom member and #908 targets it. Do not mark ready in this step.

- [ ] **Step 5: Open the stack only after CI and bottom-up review pass**

After every member's CI passes and feedback is propagated, request authorization, then use `gh stack submit --open`. Confirm all seven PRs are non-draft, correctly based and mergeable.

- [ ] **Step 6: Atomically merge only after separate final authorization**

After a separate user confirmation, run:

```sh
gh stack merge --yes --squash
```

If any member fails branch protection or mergeability, merge none; repair the owning layer and resubmit.

## Self-review

- Spec coverage: ADR amendment, typed exposure, normalized logging, 33-poll deletion, ownership-based tests, exact budget, atomic delivery and exclusions each map to a task.
- Step scan: documentation, restacking, red/green exposure, deletion alignment, closure and remote actions are separate checkable units.
- Type consistency: Task 3 defines `CancellationIoExposure`, `Error::cancellation_io_exposure` and `AfterIoMayHaveStarted`; Tasks 4–6 use those exact names and semantics.
- Review Focus: pre-Send quiet refusal, post-Send warning promotion, raw Send/Receive history, cancellation-aware direct tests and K-Line exclusion each have an owning test/gate.
- Proportion: exact function bodies are delegated to the validated spike; the plan fixes interfaces, tests, inventories, measurements and branch boundaries without transcribing implementation.
