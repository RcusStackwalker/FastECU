# Denso ISO-15765 Duplication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Share the proven Wave 4 seed/key and erase flows across all four Denso ISO-15765 executors, and the N83M in-car exchange sequence across its two consumers, while retaining their characterized wire behavior.

**Architecture:** Add three narrow operations to the existing portable Denso ISO-15765 common module. Keep `write_memory` orchestration and all read, reflash, connect/probe, kernel-jump, checksum, and family-specific response policies in their executors. The N83M common operation owns the shared in-car exchange table used by both N83M executors.

**Tech Stack:** C++23, Bazel, GoogleTest, existing UDS and scripted CAN transport ports.

**Spec:** [2026-10-06 Denso ISO-15765 duplication design](../specs/2026-10-06-denso-iso15765-duplication-design.md)

**History check:** The N83M 1.5M and 4M in-car tables both send `10 63` to CAN ID `0x7E1`, matching the design. The SH72531 and SH72543 diesel tables send `10 03` there. Both forms are present in the pre-port legacy sources; the plan shares only the identical N83M sequence and leaves the SH-family sequences local. The repository does not record the hardware rationale for the family-specific subfunction.

## Global Constraints

- Use a single `2,000 ms` `read_timeout` for both seed and key exchanges; retain the UDS client's existing `pending_timeout` and pending-repeat behavior.
- The seed remains level `0x61`, the key remains level `0x62`, the four seed bytes are taken from the same payload offsets, and the existing `denso_seed_key` calculation remains the source of the derived key.
- Set only `uds::ExchangePolicy::read_timeout`; do not propagate this timeout to probes, checksum verification, memory reads, or other exchanges.
- Preserve the erase PDU, expected prefix `20 01 05`, `500 ms` post-trigger delay, at most 20 receives, the existing `500 ms` receive and retry cadence, the sleep after each unsuccessful receive, the `71 01 02` success check, and the rule that polling never resends the erase trigger.
- Keep the N83M fire-and-forget table private to the common operation used by 1.5M and 4M. Preserve all request IDs, payload bytes, order, and per-exchange timeout; read and discard each reply without checking its ID or contents.
- Keep `write_memory`, `read_memory`, `reflash_block`, connection and probe flows, kernel jumps, checksum verification, and family-specific response policies local. Add no configuration switches or general-purpose family state machine.
- Keep the common code portable: no Qt, owned threads, or direct filesystem I/O. First-party warnings remain errors; add no warning suppressions.
- Keep executor test expectations independent of production table constants. Keep all four hardware statuses unchanged until existing qualification evidence supports a change; automated tests alone do not establish hardware qualification.

## Review Focus

- A seed reply missing any of its four seed bytes must fail before the key request. Pin with `SecurityAccessRejectsShortSeedWithoutSendingKey` in Task 1.
- NRC `0x78` must use the UDS client's existing pending timeout and reread without retransmitting SecurityAccess. Pin with `SecurityAccessRereadsPendingReplyWithoutResending` in Task 1.
- A missing or malformed erase setup response must stop before the erase trigger. Pin with `EraseSetupMismatchDoesNotSendTrigger` in Task 2.
- Twenty unsuccessful erase polls must produce one trigger, twenty `500 ms` reads, and a `500 ms` sleep after each failed receive. Pin with `ErasePollingStopsAfter20ReceivesAndNeverResendsTrigger` in Task 2.
- The N83M in-car sequence must discard replies from unexpected CAN IDs and retain its shared `0x7E1` request (`10 63`) in both consumers. Keep the SH72531 and SH72543 diesel request (`10 03`) unchanged. Pin the N83M behavior with `N83mInCarSequencePreservesBothFamilyTranscripts` in Task 3.

---

### Task 1: Share Wave 4 seed/key exchange

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.cpp`
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.cpp` and `_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.cpp` and `_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh72531_can_executor.cpp` and `_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.cpp` and `_test.cpp`

**Interfaces:**
- Consumes: `CanExecutorContext`, `fatal_query`, `exchange_context`, and `denso_seed_key` from the existing common code.
- Produces: `Status denso_security_access(const CanExecutorContext& ctx);`

- [ ] **Step 1: Capture the SonarCloud baseline before source edits.** Record the current `duplicated_lines_density` and target clone groups for `subaru_denso_1n83m_1_5m_can_executor.cpp` from the linked measure. If the base revision has no successful analysis, run the repository SonarCloud workflow against it and save the analysis link with the baseline.

- [ ] **Step 2: Write the failing common and executor tests.** In the common test, add `SecurityAccessReadsLiteralSeedAndKeyAtTwoSeconds`, `SecurityAccessRereadsPendingReplyWithoutResending`, `SecurityAccessRejectsShortSeedWithoutSendingKey`, and `SecurityAccessPropagatesCancellation`. Add its scripted-transport, UDS, clock, event-sink, and result-matcher Bazel dependencies. The success fixture is `27 61` → `67 61 11 22 33 44`, then `27 62 35 B6 83 BF` → `67 62`; assert normal reads use `2000ms`. For two seed `0x78` replies before the seed result, assert reads are `{2000ms, 3000ms, 3000ms, 2000ms}` and only the seed and key requests were written. The short-seed test must return `BadResponse` after one write; the cancellation test must return `Cancelled` without writing the key. Update each executor's bench fixture to use those literal bytes, and assert its `BenchReadReturnsPaddedImage` test sees `2000ms` at `readTimeouts()[8]` and `[9]` (seed and key).

- [ ] **Step 3: Run the focused tests to confirm they fail.**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test`

Expected: compile failure because `denso_security_access` is not declared; the new behavior is not implemented.

- [ ] **Step 4: Implement `denso_security_access` and route all four executors through it.** Set only `.read_timeout = 2000ms`; leave `pending_timeout` and `max_pending_repeats` at their UDS defaults. Validate seed and key responses with `fatal_query`, require five seed payload bytes, derive from payload bytes 1–4 with `denso_seed_key`, and retain the four existing success/failure log lines. Remove the four local `security_access` implementations and their now-unused subfunction constants.

- [ ] **Step 5: Run the common and four executor tests.**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test //src/backend/flash/ecu:subaru_denso_sh72531_can_executor_test //src/backend/flash/ecu:subaru_denso_sh72543_can_diesel_executor_test`

Expected: all five targets pass; each executor transcript retains the literal seed/key bytes and observes the two-second timeout at reads 8 and 9.

- [ ] **Step 6: Commit the seed/key change.**

```bash
git add src/backend/flash/ecu/BUILD.bazel src/backend/flash/ecu/denso_iso15765_can_common.h src/backend/flash/ecu/denso_iso15765_can_common.cpp src/backend/flash/ecu/denso_iso15765_can_common_test.cpp src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.cpp src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.cpp src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp src/backend/flash/ecu/subaru_denso_sh72531_can_executor.cpp src/backend/flash/ecu/subaru_denso_sh72531_can_executor_test.cpp src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.cpp src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor_test.cpp
git commit -m "refactor: share Denso ISO-15765 security access"
```

### Task 2: Share Wave 4 erase flow

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`, `.cpp`, and `_test.cpp`
- Modify: `src/backend/flash/ecu/BUILD.bazel`
- Modify: all four Wave 4 executor `.cpp` and `_test.cpp` files listed in Task 1

**Interfaces:**
- Consumes: `CanExecutorContext`, `fatal_query`, and each executor's existing `setup_pdu` helper.
- Produces: `Status denso_iso15765_erase(const CanExecutorContext& ctx, bytes::ByteView request_download_setup_pdu);`

- [ ] **Step 1: Add failing erase tests.** Add common tests `EraseSetupMismatchDoesNotSendTrigger`, `EraseAccepts71_01_02AfterPolling`, `ErasePollingStopsAfter20ReceivesAndNeverResendsTrigger`, and `EraseCancellationAfterTriggerStopsPolling`. Use a literal RequestDownload setup PDU and literal `31 01 02 01 FF FF FF FF` trigger. The setup-mismatch test returns `BadResponse` after one write. The polling-success test accepts `71 01 02` after a failed `71 01 03` poll, uses only two writes, and records `{500ms, 500ms, 500ms}` reads plus `{500ms, 500ms}` sleeps. The exhaustion test returns `BadResponse`, records 21 reads at `500ms` (setup plus 20 polls), 21 sleeps at `500ms` (settle plus one after each failed poll), and exactly two writes. The cancellation test returns `Cancelled` after the one trigger and performs no poll. Assert the shared setup, erase, success, and failure log lines. Update each executor's existing `EraseRetryExhaustionFails` to prove there was no second trigger.

- [ ] **Step 2: Run the common target to confirm the new erase tests fail.**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test`

Expected: compile failure because `denso_iso15765_erase` is not declared.

- [ ] **Step 3: Implement `denso_iso15765_erase` and replace the four local erase bodies.** Have each executor pass `setup_pdu(uds::kSidRequestDownload, region)` and keep that helper for its read/upload paths. Use the shared setup message `Setting flash start & length`, followed by `Erasing ECU ROM`; retain `Flash erased! Starting flash write, do not power off!` and `Flash area erase failed`. Check the `71 01 02` prefix, use one `500ms` post-trigger sleep, receive at most 20 times with `500ms` timeout, sleep `500ms` after each unsuccessful receive, and never resend the trigger.

- [ ] **Step 4: Run the common and four executor tests.** Run the Task 1 focused Bazel test command. Expected: all five targets pass, including setup rejection, cancellation, terminal success, retry exhaustion, and progress-event assertions.

- [ ] **Step 5: Commit the erase change.**

```bash
git add src/backend/flash/ecu/BUILD.bazel src/backend/flash/ecu/denso_iso15765_can_common.h src/backend/flash/ecu/denso_iso15765_can_common.cpp src/backend/flash/ecu/denso_iso15765_can_common_test.cpp src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.cpp src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.cpp src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp src/backend/flash/ecu/subaru_denso_sh72531_can_executor.cpp src/backend/flash/ecu/subaru_denso_sh72531_can_executor_test.cpp src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.cpp src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor_test.cpp
git commit -m "refactor: share Denso ISO-15765 erase flow"
```

### Task 3: Share the N83M in-car sequence

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`, `.cpp`, and `_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.cpp` and `_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.cpp` and `_test.cpp`
- Modify: `docs/superpowers/specs/2026-10-06-denso-iso15765-duplication-design.md`

**Interfaces:**
- Consumes: `CanExecutorContext` and the existing `fire_and_forget` primitive.
- Produces: `Status n83m_in_car_fire_and_forget(const CanExecutorContext& ctx, ICanFlashTransport& can);`

- [ ] **Step 1: Add the failing common test `N83mInCarSequencePreservesBothFamilyTranscripts`.** Assert these ten ordered `(CAN ID, PDU)` pairs, all with a `200ms` read timeout: `(0x7A2, 10 C0)`, `(0x7E0, 10 63)`, `(0x7DF, 10 03)`, `(0x7E1, 10 63)`, `(0x7B0, 10 03)`, `(0x7B0, 85 02)`, `(0x7DF, 85 02)`, `(0x7B0, 85 02)`, `(0x7DF, 85 02)`, `(0x7DF, 28 03 01)`. Script wrong reply IDs and contents; assert success, ten writes, and ten `200ms` reads. Keep both N83M executor script expectations literal and independent of production constants.

- [ ] **Step 2: Run the common target to confirm the test fails.**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test`

Expected: compile failure because `n83m_in_car_fire_and_forget` is not declared.

- [ ] **Step 3: Implement the common sequence and replace only the two N83M local tables.** Keep the ten-entry table private in `denso_iso15765_can_common.cpp`, use `fire_and_forget` for each entry, and encode the shared `0x7E1` subfunction as `0x63`. Leave the SH72531 and SH72543 diesel tables untouched; they send `0x03` to `0x7E1`. The design's identical-table statement is accurate and needs no correction.

- [ ] **Step 4: Run the common and two N83M executor tests.**

Run: `bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test`

Expected: all three targets pass; both literal in-car transcripts preserve their `0x7E1` request and ignore mismatched reply IDs and contents.

- [ ] **Step 5: Commit the N83M sequence change.**

```bash
git add src/backend/flash/ecu/denso_iso15765_can_common.h src/backend/flash/ecu/denso_iso15765_can_common.cpp src/backend/flash/ecu/denso_iso15765_can_common_test.cpp src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.cpp src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.cpp src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp docs/superpowers/specs/2026-10-06-denso-iso15765-duplication-design.md
git commit -m "refactor: share N83M in-car exchange sequence"
```

### Task 4: Update the durable rationale and qualification notes

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`
- Modify: `docs/design-notes.md#share-only-proven-protocol-equivalence`
- Modify: `docs/flash-qualification-matrix.md` rows for the four Wave 4 families

**Interfaces:**
- Consumes: the final shared-operation scope from Tasks 1–3.
- Produces: current rationale that identifies the three shared operations and the retained family-local flows.

- [ ] **Step 1: Update the common-header rationale and design notes.** Describe the seed/key and erase operations shared by all four Wave 4 executors, the N83M-only in-car operation and its `0x7E1` variant, and the operations that remain local. Remove the old claim that erase must stay family-local while preserving the existing warning against broad state-machine factoring.

- [ ] **Step 2: Update only the relevant qualification-matrix notes.** Record `2000ms` as the shared seed/key `read_timeout` for all four families and identify the change from `200ms` as deliberate for 1.5M, 4M, and SH72531. Leave every family's `hardware_status` and `hardware_evidence` unchanged.

- [ ] **Step 3: Check links and review the documentation diff.**

Run: `prek run lychee --all-files`

Expected: no broken Markdown links; the diff changes only the shared-flow rationale and timeout notes.

- [ ] **Step 4: Commit the documentation update.**

```bash
git add src/backend/flash/ecu/denso_iso15765_can_common.h docs/design-notes.md docs/flash-qualification-matrix.md
git commit -m "docs: record shared Denso ISO-15765 flows"
```

### Task 5: Run repository gates and verify SonarCloud results

**Files:**
- No source changes; report results in the pull request with links to the successful runs and analysis.

**Interfaces:**
- Consumes: completed implementation and documentation from Tasks 1–4.
- Produces: passing affected targets and repository gates, plus verified before/after SonarCloud evidence.

- [ ] **Step 1: Build and test the affected Bazel targets.**

Run: `bazel build --config=release //src/backend/flash/ecu:denso_iso15765_can_common //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor //src/backend/flash/ecu:subaru_denso_sh72531_can_executor //src/backend/flash/ecu:subaru_denso_sh72543_can_diesel_executor`

Run: `bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test //src/backend/flash/ecu:subaru_denso_sh72531_can_executor_test //src/backend/flash/ecu:subaru_denso_sh72543_can_diesel_executor_test`

Expected: all builds and tests pass.

- [ ] **Step 2: Run the repository gates.** Run `prek run --all-files`, `python3 scripts/gazelle_check.py --fix` and review generated BUILD changes, `bazel run //:clang_tidy_report_changed`, `bazel build --config=release //:fastecu`, and `bazel test --config=release //...`. Expected: each applicable gate passes without warning suppressions or unrelated generated changes.

- [ ] **Step 3: Complete a successful SonarCloud analysis on the resulting revision.** Use the repository's [SonarCloud workflow](../../../.github/workflows/sonar.yml) and require its quality gate to pass. Compare the target file's `duplicated_lines_density` and addressed clone groups with the pre-change baseline. Record both density values and the analysis link in the pull request; confirm the addressed clone groups no longer appear before claiming reduction.

- [ ] **Step 4: Confirm qualification status remains evidence-based.** Verify the matrix still marks all four families `experimental` with no new hardware evidence. Do not infer qualification from the automated results or SonarCloud analysis.
