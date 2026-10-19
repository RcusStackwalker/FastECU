# Colt Cancellation Centralization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Centralize refusal of cancelled Colt CAN I/O, remove its four explicit executor polls, and preserve existing behavior tests without claiming test compression.

**Architecture:** A non-owning GuardedCanFlashTransport checks each Read/Write token and otherwise forwards the existing port. BoundAttempt constructs it only around Execute for an executor naming a compatible CancellationGuard type. Colt opts in; direct tests supply the same wrapper explicitly.

**Tech Stack:** Qt-free C++23, GoogleTest/Google Mock, Bazel 9.1.1, Gazelle, pinned Lizard reporting, existing coverage and mutation tooling.

**Spec:** [Confirmed Colt cancellation design](../specs/2026-10-19-guarded-transport-refinement-design.md). Read both documents. This plan awaits review and an execution-method choice; it is not authorization to implement.

## Global Constraints

- “Pilot only Mitsubishi Colt M32R CAN, removing its four explicit executor token checks while retaining all existing behavior tests.”
- “Test compression is a separately assessed follow-up.”
- “Existing executor signatures remain unchanged.”
- “Return each delegated result unchanged, including success, an absent frame, present-empty bytes, and every typed error.”
- “Add no post-call token check, final token sweep, error remapping, or persistent stop-after-error state.”
- “No reciprocal includes, port split, runtime family registry, or layer-visibility widening is needed.”
- “Keep runner admission/setup checks, UdsClient's pre-request check, cancellable Sleep, and platform read/write/unblock handling.”
- “More than 25% ordinary-runtime growth requires explicit review before accepting or expanding the pilot”.
- “Expansion beyond Colt requires review of those results and a recorded run of the applicable Colt bench checklist.”
- Follow [repository instructions](../../../AGENTS.md), [coding conventions](../../coding-style.md), and [flash contracts](../../reference/flash-contracts.md). No warning suppressions, new ErrorKind, production Qt/thread/filesystem dependency, or qualification claim.

## Review Focus

1. Cancellation after an accepted Write must block the following Read even when the inner transport ignores cancellation (Task 3).
2. Cancellation inside a delegate must not replace its returned data/error; an earlier transport failure must not poison a later allowed call (Task 1).
3. An overridden RestartIso15765 and cleanup/unblock calls must retain their dispatch and results after cancellation (Tasks 1–2).
4. Late cancellation must preserve complete ROM success, verification failure, and invalid-plan precedence rather than introduce a final token sweep (Task 3).
5. A family swallowing a failure must not hide a forbidden erase/download attempt behind ScriptConsumed (Task 3).

---

## File Structure and Interfaces

| Path | Responsibility |
| --- | --- |
| `src/backend/flash/guarded_can_flash_transport.h/.cpp` (new) | Non-owning decorator; cancellation refusal and exact forwarding only |
| `src/backend/flash/guarded_can_flash_transport_test.cpp` (new) | Focused port-contract suite; local cancellation-blind probe |
| `src/backend/flash/flash_executor.h` | Detect the dependent guard type in BoundAttempt; preserve owned lifecycle |
| `src/backend/flash/bound_flash_attempt_test.cpp` | Opted-in execution/cleanup tests alongside existing non-opted-in cases |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h/.cpp` | Colt opt-in, direct-call precondition, removal of four polls |
| `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp` | Explicit wrapper, minimal local observations, retained cases and boundary regressions |
| `src/backend/flash/BUILD.bazel`, `src/backend/flash/ecu/BUILD.bazel` | Gazelle-managed guard/test targets and dependencies |
| `docs/reference/colt-cancellation-centralization-results.md` (new during execution) | Pinned baselines, assertion/boundary inventory, measured costs, gate verdicts and evidence provenance |
| `docs/README.md`, `docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md`, `docs/superpowers/specs/2026-10-18-size-reduction-strategy.md` | Navigation and actual implementation/exit status |
| `docs/reference/flash-contracts.md` | Enduring cancellation-boundary contract extracted before eventual plan/spec retirement |

No changes to shared scripted transports, UdsClient, platform transports/clocks, or other executors are planned. Test-local probes do not become a shared mocking framework.

### Task 1: Implement the guarded CAN port and prove its contract

**Files:** Create the three `guarded_can_flash_transport*` C++ files and results record above; regenerate `src/backend/flash/BUILD.bazel`.

**Interfaces:** Consumes ICanFlashTransport, ICancellationToken, Status and Result from existing contracts. Produces `fastecu::flash::GuardedCanFlashTransport final : public ICanFlashTransport`, with this public API:

```cpp
explicit GuardedCanFlashTransport(ICanFlashTransport& inner) noexcept;
Status ResetConnection() override;
Status Configure(const Iso15765Config& config) override;
Status Open() override;
Status Close() override;
Status RestartIso15765(const Iso15765Config& config, const ICancellationToken& cancellation) override;
void RequestUnblock() noexcept override;
Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override;
Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                      const ICancellationToken& cancellation) override;
```

- [ ] **Step 1: Freeze the comparison before any C++ edits.** At execution time use the worktree skill to establish an isolated feature branch; preserve unrelated files. Pin `PILOT_BASE_SHA` to its initial HEAD and record source drift from spec baseline `2b9613b8275a35fa485caa757a05ba628f163744`. Create a task-specific temporary evidence directory and the results record. Record toolchain/Qt, CPU/OS, scheduling, source hashes, named baseline cases, and the four check sites. Do not use either old PR as the baseline.

  Freeze this ordinary-runtime manifest as `PILOT_TESTS` in the execution shell and preserve it in the results record:

```sh
PILOT_TESTS=(
  //src/backend/flash:flash_executor_test
  //src/backend/flash:bound_flash_attempt_test
  //src/backend/flash:can_flash_uds_channel_test
  //src/backend/flash/ecu:mitsu_colt_m32r_can_plan_test
  //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test
  //src/backend/flash/ecu:uds_client_exchange_common_test
  //src/backend/flash/testing:scripted_flash_transports_test
  //src/backend/protocol/uds:uds_client_test
  //src/backend/ports/testing:fake_clock_test
)
bazel test --config=release --nocache_test_results --local_test_jobs=1 "${PILOT_TESTS[@]}"
python3 scripts/nloc_loop.py measure --json
prek run nloc --hook-stage manual --all-files
```

  Require all tests to pass. Run one warm-up followed by five measured invocations of the test command; after each, preserve each target's XML from `bazel info bazel-testlogs` before the next invocation overwrites it. Save both NLOC outputs. For each target XML, use the root `time` attribute once when present, otherwise sum its direct testsuite children's `time` attributes; never also sum nested testcase times; store all five sums and their median. Exclude build/wall time and competing builds. Resolve a failing baseline before comparing candidates.

- [ ] **Step 2: Declare the guard API in its header.** Store only `ICanFlashTransport& inner_`. Include existing flash_executor contracts; do not add a token member, lifecycle owner, or reciprocal include. Leave method definitions for the green step.
- [ ] **Step 3: Write the contract tests.** Define a test-local `ProbeCanTransport` derived from existing ScriptedCanFlashTransport. Its overridden Read/Write deliberately ignore token state, record bytes/timeout/token identity and call counts, optionally cancel a supplied ManualCancellationToken inside the call, and return a configured result. Override RestartIso15765 to record config/token and return a distinctive configured result without invoking the port default. Count RequestUnblock; reuse inherited lifecycle counters/results. Do not copy payloads beyond this tiny unit fixture or add common fake APIs.

  Use suite name `GuardedCanFlashTransport`. Write these named tests; rows describe concrete arrangements and assertion expressions. Each row gets fresh transport/token/guard state; parameterized rows use named GoogleTest parameters.

| Test | Arrangement and assertions |
| --- | --- |
| `PrecancelledWriteDoesNotDelegate`, `PrecancelledReadDoesNotDelegate` | Cancel first; `ASSERT_THAT(guard.Write(data, token), IsErrWith(ErrorKind::kCancelled, "CAN write cancelled before delegation"))` or corresponding Read/read text; `EXPECT_EQ(inner.write_calls, 0)` / `EXPECT_EQ(inner.read_calls, 0)`. |
| `ForwardsBytesTimeoutAndOriginalToken` | Write `{0x00,0x80,0xFF}`, Read at `37ms`; `ASSERT_THAT(sent, IsOk())`; `EXPECT_THAT(inner.last_write, ElementsAre(0x00,0x80,0xFF))`; `EXPECT_EQ(inner.last_timeout, 37ms)`; both recorded token addresses equal `&token`; one call each. |
| `ReadOutcomesPassThrough` | Cases absent optional, present-empty Bytes, and `{0x12,0xA5}`; `ASSERT_THAT(actual, IsOk())`; `EXPECT_EQ(*actual, expected_optional)` with exact engagement and contents. Repeat with cancellation triggered inside Read; `EXPECT_TRUE(token.Cancelled())` and the same outcome assertions. |
| `TypedFailuresPassThrough` | Read and Write, each ErrorKind (`kInvalidConfig`, `kTimeout`, `kDisconnected`, `kBadResponse`, `kCancelled`, `kUnsupported`, `kInternal`), detail `"delegate detail"`, with/without inside-call cancellation: `ASSERT_THAT(actual, IsErrWith(kind, "delegate detail"))`; exactly one delegate call. |
| `AcceptedWriteSuccessSurvivesInsideCallCancellation` | Inner Write cancels then returns success; `ASSERT_THAT(actual, IsOk())`; `EXPECT_TRUE(token.Cancelled())`; `EXPECT_EQ(inner.write_calls, 1)`. |
| `FailureDoesNotPoisonTheNextCall` | Live token: inner Write returns kDisconnected, then succeeds; `ASSERT_THAT(first, IsErr(ErrorKind::kDisconnected))`; `ASSERT_THAT(second, IsOk())`; `EXPECT_EQ(inner.write_calls, 2)`. |
| `LifecycleRestartAndUnblockForwardAfterCancellation` | Cancel first; config `{500000,0x7E0,0x7E8,false}`. Reset/configure/open/close counters each one and lifecycle order matches calls; all four config fields preserved. Restart returns configured `kUnsupported/"restart override"`, restart count one, same config/token, and no extra reset/configure/open; unblock count one. Parameterize lifecycle failure returns to assert unchanged kind/detail as well. |

- [ ] **Step 4: Generate the targets and run red.** `python3 scripts/gazelle_check.py --fix`, then `bazel test --config=release //src/backend/flash:guarded_can_flash_transport_test`. Expect unresolved guard definitions, not a missing dependency or toolchain failure. Review generated targets; do not hand-edit managed BUILD files.
- [ ] **Step 5: Implement the declared methods in the .cpp.** Read/Write return the exact cancellation details above only when the supplied token is observed cancelled at entry; otherwise return the inner call's result directly. Forward every other operation directly, including the inner restart override and noexcept unblock. Generate BUILD updates again.
- [ ] **Step 6: Run green.** Run `bazel test --config=release //src/backend/flash:guarded_can_flash_transport_test //src/backend/flash/testing:scripted_flash_transports_test`; expect PASS. Recheck formatting and generated dependencies: guard → flash_executor only, no family dependency. Do not expand the runtime manifest to hide self-test cost.
- [ ] **Step 7: Commit the independently tested decorator.** Stage only the three new C++ files, generated flash BUILD, and the results record; commit `feat: add cancellation-only CAN transport guard`.

### Task 2: Install compatible opt-in guards at the runner boundary

**Files:** Modify `src/backend/flash/flash_executor.h`, `src/backend/flash/bound_flash_attempt_test.cpp`; regenerate `src/backend/flash/BUILD.bazel`.

**Interfaces:** Consumes Task 1's constructor and port implementation. Produces support in existing `BoundAttempt<Executor, Transport>::Run(IClock&, const ICancellationToken&, IEventSink&) -> Result<FlashExecutionResult>` for `typename Executor::CancellationGuard`, constructible from the owned transport and derived from `Executor::TransportType`. No public signature changes.

- [ ] **Step 1: Write opted-in runner tests.** Add a local cancellation-blind CAN transport with write/unblock counters, and a `CanExecutorProbe : ICanFlashExecutor` that returns config `{500000,0x7E0,0x7E8,false}` and can cancel its supplied ManualCancellationToken immediately before Execute calls Write. A `GuardedCanExecutorProbe` subclass adds only `using CancellationGuard = GuardedCanFlashTransport` and inherits the constructor. Bind concrete types with existing BindFlashAttempt; keep existing K-Line/mixed suites untouched.

| New runner test | Assertions |
| --- | --- |
| `OptedInExecutorRefusesCancelledWrite` | Cancel inside Execute; `ASSERT_THAT(result, IsErr(ErrorKind::kCancelled))`; `EXPECT_EQ(inner.write_calls, 0)`; configure/open/close each one. |
| `UnoptedInCanExecutorKeepsItsExistingPath` | Same cancellation-blind probe without alias; `ASSERT_THAT(result, IsOk())`; `EXPECT_EQ(inner.write_calls, 1)`; close one. |
| `OptedInExecutionErrorWinsOverCloseError` | Execution cancellation plus close kInternal; `ASSERT_THAT(result, IsErr(ErrorKind::kCancelled))`; close one; warning retained. |
| `OptedInCloseOnlyFailureIsReturned` | Successful execution plus close `kInternal/"close failed"`; `ASSERT_THAT(result, IsErrWith(ErrorKind::kInternal, "close failed"))`; close one. |
| `OptedInUnblockReachesOwnedTransport` | Call attempt.RequestUnblock before/after Run; inner unblock count increments each time, including without a live stack guard. |

- [ ] **Step 2: Generate dependencies and run red.** `python3 scripts/gazelle_check.py --fix`; `bazel test --config=release //src/backend/flash:bound_flash_attempt_test`. The opted-in refusal test must fail with one raw write instead of zero; existing tests remain valid.
- [ ] **Step 3: Implement opt-in execution in Run.** Use dependent-type detection (`requires { typename Executor::CancellationGuard; }`) and enforce compatibility/constructibility at compile time. Construct the named guard on the stack only in that branch and pass it to Execute; otherwise pass the owned transport. Keep both branches within the execution-result computation so the existing final-close/error precedence is shared. Leave preparation, configure/open, admission polls and RequestUnblock unchanged. No concrete guard/Colt include or virtual capability is added to flash_executor.h.
- [ ] **Step 4: Run green.** Run `bazel test --config=release //src/backend/flash:bound_flash_attempt_test //src/backend/flash:flash_executor_test //src/backend/flash:guarded_can_flash_transport_test`. Expect PASS, including existing `CancellationIsNotUniversallyPolledBetweenConfigureAndOpen`, mixed lifecycle, and close precedence cases. Review generated deps and static assertions.
- [ ] **Step 5: Commit.** Stage only the runner header/test and generated flash BUILD; commit `refactor: support executor-selected cancellation guards`.

### Task 3: Migrate Colt and pin the changed boundaries

**Files:** Modify `src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h/.cpp`, `src/backend/flash/ecu/mitsu_colt_m32r_can_executor_test.cpp`; regenerate `src/backend/flash/ecu/BUILD.bazel`; update the results inventory.

**Interfaces:** Consumes Tasks 1–2. Colt produces `using CancellationGuard = GuardedCanFlashTransport`. Define suite-local `ExecuteGuarded(MitsuColtM32rCanExecutor&, const FlashPlan&, ICanFlashTransport&, IClock&, const ICancellationToken&, IEventSink&) -> Result<FlashExecutionResult>`; it constructs the guard and calls the real executor. This is test support, not a new production execution API.

- [ ] **Step 1: Adapt direct tests without removing cases.** Replace direct executor.Execute calls with ExecuteGuarded, retaining every assertion except the documented cancellation-owner detail in `ACancellationThatArrivesAfterTheRequestStopsBeforeTheReply`: expect `"CAN read cancelled before delegation"` instead of `"scripted CAN read cancelled"`. Retain other detail/log assertions. Add Read/Write attempt counters to the existing suite-local ScriptedCanFlashTransport subclass, incrementing before forwarding with the original token. Add local `WatchWrite(bytes::ByteView)` and `WatchedWriteAttempts() const -> std::size_t` for one owned request header; count attempted matches before the base returns, with no bulk-payload trace copies or common-fake change.
- [ ] **Step 2: Add deterministic boundary injection.** Define suite-local `CancelOnReplyTransport(ManualCancellationToken&, bytes::Bytes trigger_request)` with Write/Read overrides and `Triggered() const -> bool`: match an accepted Write, then cancel only after its successful Read, returning that Read result unchanged. Define `CancelAtPhaseProgressSink(ManualCancellationToken&, std::string phase, int done)` with PhaseProgress override/Triggered, forwarding recorded events before matching. Retain existing injectors; no poll-count triggers, sleeps, or threads.
- [ ] **Step 3: Write the boundary regressions.** All new tests use the real executor and existing script builders/ROMs; each asserts its cancellation trigger actually fired.

| Test | Setup and assertions |
| --- | --- |
| `PrecancelledExecutionReportsConnectingBeforeRefusal` | Valid ReadPlan, precancel, ExecuteGuarded; `ASSERT_THAT(result, IsErr(ErrorKind::kCancelled))`; connecting info log is present; Write/Read attempts both zero. |
| `InvalidPlanStillWinsOverPrecancel` | Reuse existing inconsistent hand-built plan with region `{0x8000,0x78000}` and image `0x60000` for the 384 protocol; precancel. `ASSERT_THAT(result, IsErr(ErrorKind::kInvalidConfig))`; events and I/O attempts empty. |
| `ReadStopsAtTheNextChunkWhenCancelledMidRead` | Extend existing mid-read cancellation case: one 192-byte chunk at zero, then cancel. `ASSERT_THAT(result, IsErr(ErrorKind::kCancelled))`; read attempts `4` (three handshake plus one chunk), writes `4`; warning names `"the flash read at 0xc0"`; partial progress remains below total. |
| `TopComparisonCancellationIsReportedByTheNextExchange` | 512 write; cancel after accepted comparison read at `0x60000`, fill EE. `ASSERT_THAT(result, IsErr(ErrorKind::kCancelled))`; writes/reads `4`; warning names `"the flash read at 0x600c0"`; no routine upload. |
| `AcceptedEraseReplyIsReportedBeforeNextRequestCancellation` | 384 write through routine uploads and accepted unlock/erase reply `{0x71,0xE0,0x00}`; CancelOnReplyTransport targets erase. `ASSERT_THAT(result, IsErr(ErrorKind::kCancelled))`; info `"Userspace flash erased"` present; watch userspace RequestDownload at `0x8000`, length `0x58000`: attempts zero. |
| `FinalReadSuccessSurvivesLateCancellation` | Full 384 read filled 5A; cancel at `"Read ROM"`, done `0x60000 - 1` (PhaseReporter clamps the final chunk update). `ASSERT_THAT(result, IsOk())`; bytes present, size `0x60000`, `BytesFilledWith(0x5A)`; writes/reads `2051`; full script consumed. |
| `FinalVerifyMismatchSurvivesLateCancellation` | Reuse 384 WriteRom384 and complete write scripts; verify payload FF mismatches the image. Cancel at `"Verify userspace"`, done `0x58000` (phase total is `0x58001`). `ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse))`; verify-failed log present; complete verification script consumed. |

  Keep every predecessor case name. Use suite `MitsuColtM32rCanExecutor` for new non-fixture cases. Include `src/algorithms/protocol/testing/byte_matchers.h` for `test_bytes::BytesFilledWith`; regenerate its dependency. Use existing byte/result matchers, not new payload generators.

- [ ] **Step 4: Add actual runner conformance and explicit forbidden-suffix assertions.** `ColtBoundAttemptStopsBeforeReadWhenCancelledAfterWrite` binds a real Colt executor to a local transport that calls the existing scripted fake with a separate never-cancelled token, cancels the real token after the first accepted session Write, and exposes Read/Write attempt counters. Queue the valid session response. `ASSERT_THAT(result, IsErr(ErrorKind::kCancelled))`; Write attempts `1`, Read attempts `0`, queued reply remains, Close count `1`. This deliberate cancellation blindness is local to the test; production still forwards the real token. In retained `WriteStopsWhenTheReflashUnlockIsRejected`, watch the framed erase request `{0x00,0x00,0x07,0xE0,0x31,0xE0}` and assert zero attempted matches. In retained `WriteStopsWhenTheEraseTriggerIsRejected`, watch RequestDownload at `0x8000`, length `0x58000`, and assert zero attempted matches. Preserve their original error/log/script assertions too.
- [ ] **Step 5: Run red.** Generate BUILD changes, then run `bazel test --config=release //src/backend/flash/ecu:mitsu_colt_m32r_can_executor_test`. Before migration the real-runner conformance consumes one extra Read; precancel connecting events, next-exchange warnings, and accepted-erase completion events are absent. Record these intended failures; a fixture mismatch is not valid red evidence. Already-preserved completion/validation cases may pass.
- [ ] **Step 6: Implement the Colt migration.** Include the guard header and add the nested alias to the Colt declaration; document that direct Execute callers supply a guarded transport. Delete only the four `Cancelled()` branches in ReadFlashRange, FlashRangeMatches, UnlockAndErase, and Execute. Retain all error returns, confirmations, validation, byte construction, timeouts, delays and remaining token plumbing; update comments about removed polling ownership. Do not delete cancellation checks in any shared/platform owner.
- [ ] **Step 7: Run green and prove mutation sensitivity.** Run `bazel test --config=release "${PILOT_TESTS[@]}" //src/backend/flash:guarded_can_flash_transport_test`. Expect PASS and all original named cases retained. Temporarily bypass only the runner's opted-in wrapper: the actual Colt conformance Read-attempt assertion must fail. Restore, then separately remove the unlock-failure return and the caller's failed-erase return: each corresponding retained rejection case must fail its watched-request assertion. Restore each mutation before the next run; confirm the restored full focused set passes. Use the same Colt-target command with `--test_arg=--gtest_filter=MitsuColtM32rCanExecutor.ColtBoundAttemptStopsBeforeReadWhenCancelledAfterWrite` for bypass, and the respective `MitsuColtM32rCanWriteExecutor.WriteStopsWhenTheReflashUnlockIsRejected` / `MitsuColtM32rCanWriteExecutor.WriteStopsWhenTheEraseTriggerIsRejected` filter for abort mutations. Record exact locations, commands, failures and source hashes; commit no mutant.
- [ ] **Step 8: Commit.** Stage only the Colt source/header/test, generated ECU BUILD, and results record; commit `refactor: move Colt cancellation polls to its transport guard`.

### Task 4: Complete measurements, gates and durable documentation

**Files:** Finish `docs/reference/colt-cancellation-centralization-results.md`; update `docs/README.md`, `docs/adr/0025-enforce-cancellation-in-a-guarded-transport.md`, `docs/superpowers/specs/2026-10-18-size-reduction-strategy.md`, and the implementing spec status as warranted by evidence. Extract enduring boundaries to `docs/reference/flash-contracts.md` before permitted retirement. No test deletion, new family, or hardware status change.

**Interfaces:** Consumes Task 1's frozen source/target/XML/NLOC baseline and Tasks 1–3's candidate/tests/mutation outcomes. Produces a reproducible results record, explicit gate verdicts, and a proposed Colt-only change ready for review.

- [ ] **Step 1: Run the delivery gates on the formatted candidate.** Run `python3 scripts/gazelle_check.py --fix`, inspect its output, then `prek run --all-files`, `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, and `bazel run //:clang_tidy_report_changed`; require successful exits. Run existing coverage through `scripts/coverage-local.sh` and changed-line mutation via `python3 scripts/mutation_test.py --diff-ref "$PILOT_BASE_SHA"`, resolving changed-line gaps without changing CI policy. Confirm existing Linux/macOS/Windows, portable-core, sanitizer and packaging jobs on the delivery PR when available; distinguish local evidence from CI still pending. Fix warning causes, never silence them.
- [ ] **Step 2: Repeat matched measurements.** Run `bazel test --config=release --nocache_test_results --local_test_jobs=1 "${PILOT_TESTS[@]}" //src/backend/flash:guarded_can_flash_transport_test` for one warm-up and five measured runs under the same recorded conditions. Preserve the XML and five sums, medians, and `(candidate_median / baseline_median - 1) * 100`. More than 25% growth requires review and investigation. Repeat both NLOC commands; sum baseline/candidate per-file values over the union of all changed/added/removed C++ production/support/test files, with new/deleted sides zero, using existing `scripts/nloc_loop.py` parse_report/classify behavior. Include whole-tree and production/test splits. Report initial growth honestly; no net-reduction gate is invented for this ownership change. If support changes expand scope or a rebase affects comparability, repeat both sides with the same manifest.
- [ ] **Step 3: Finish the evidence and state the limits.** Record every retained predecessor case, assertion/event-detail changes, four deleted polls, named bypass/abort mutations, exact targets, five samples, NLOC deltas, and each gate verdict. Retain raw logs/XML/reports in durable delivery artifacts and record links plus SHA-256 hashes, not just a temporary-directory path. Link the results from the documentation index; update ADR/strategy with actual implementation status and result pointers instead of copying measurements. The separate compression proposal and recorded Colt bench run are still required before expansion; no bench pass is claimed from tests.
- [ ] **Step 4: Apply the document lifecycle rule without losing originals.** Before retiring this plan/spec, verify that their original contents will survive the chosen delivered Git history. If a squash would introduce and remove them together, deliver preservation and retirement separately, keeping retirement out of that squash. Extract enduring boundaries into the owning flash contract/results and retarget inbound Markdown links before any permitted removal; leave no stale completed plan or dangling reference in the final delivered cleanup.
- [ ] **Step 5: Verify the documentation and commit.** Run `prek run lychee --all-files` and `git diff --check`; expect PASS/no diagnostics. Stage the named evidence/owner-document paths only and commit `docs: record Colt cancellation pilot evidence and limits`. Request the required whole-branch review using the execution workflow chosen by the user. A pending platform job, artifact, runtime review, or required mutation result is reported as pending rather than a completed gate.
