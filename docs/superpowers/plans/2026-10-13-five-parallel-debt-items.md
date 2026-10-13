# Five Parallel Technical-Debt Tasks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct three demonstrated defects and add two focused coverage matrices.

**Architecture:** Five independent tasks retain existing public interfaces and
have disjoint source/test ownership. The coordinator owns shared contracts,
MainWindow integration, generated BUILD files, and final checks. Each task can
be reviewed and delivered independently; the task sections are separate subsystem
plans collected here for scheduling.

**Tech Stack:** Qt 6, C++23, GoogleTest/GoogleMock, pugixml, Bazel, prek.

**Spec:** [Approved five-task design](../specs/2026-10-13-five-parallel-debt-items.md).

## Global Constraints

- Include demonstrated correctness fixes with regressions and focused coverage
  growth.
- No task requires another task's output, a preliminary source split, new port,
  new library, or new test target.
- Do not retain references to run-owned data across notifications.
- MUT reads: do not retry, skip, fill, or return partial success.
- MUT helpers: do not wire new callers or relax the writable window.
- Preserve existing untracked specs/plans.
- Follow [repository instructions](../../../AGENTS.md), [coding conventions](../../coding-style.md),
  and [layering](../../design-notes.md#layering). No new error kinds or warning
  suppressions; GoogleTest suites and mocks remain package-owned.
- Only coordinator edits shared documentation, BUILD files, or MainWindow tests.
  Stage named paths. Never commit on master; push only when authorized.

## Review Focus

These conditions extend the spec's explicit examples within the chosen contracts:

- Shutdown inside sample-error delivery must abandon remaining samples (Task A).
- A failed replacement startup must not let the old batch resume (Task A).
- Cancellation after a successful MUT chunk must return cancellation, not partial
  success (Task B).
- A malformed MapPack record after a valid record must identify its own row
  (Task D).
- A checksum input view into a larger buffer must ignore surrounding bytes
  (Task E).

## Scheduling and handoff

Read the spec and owning references before changes. When implementation is
authorized, use the repository's isolated-workspace workflow. Keep the original
untracked documents outside this batch's commits. Start A/B/C together; start
D/E when slots free. With a root coordinator and four slots there are at most
three active child agents, not five simultaneously. The user selected subagents;
retain that method unless they change it.

Give each implementer its task section and both artifacts. Request focused test
results and the final diff, not a shared-file commit. Coordinator stages/commits
one reviewed task at a time to avoid concurrent index mutations. Review each
task against its spec and tests before its task commit; review the whole branch
after integration. Shared-document closeout is Task F.

## Task A: Logging batch identity

**Files:** Modify [implementation](../../../src/ui/desktop/widgets/desktop_logging_run.cpp),
[header](../../../src/ui/desktop/widgets/desktop_logging_run.h), and
[tests](../../../src/ui/desktop/widgets/desktop_logging_run_test.cpp).

**Interfaces:** Consumes the borrowed `LoggingEngine`, `valuesUpdated` batches,
and existing `LoggingCsvFile`. Produces unchanged public
`Status DesktopLoggingRun::start(DesktopLoggingRunStart request)`, `void stop()`,
`void shutdown()`, and `DesktopLoggingRunView view() const`; other tasks consume
none of its internals.

- [ ] **A1: Replace the existing replacement characterization with a regression.**
  Name it `replacementDuringSampleErrorDiscardsOldBatch`. Enable CSV, start A,
  retain A's path, and use a guarded error subscriber to stop A and start B.
  Deliver `unknown=8` followed by `parameter:rpm=99`. Assert B is active, its rpm
  is empty, its CSV contains only the header, and A's file remains closed.
  Deliver B's rpm `43` and assert B's cache and CSV contain `43` and never `99`.
  Reuse fixture `makeRequest`, `readFile`, and `registerBlocked`.
- [ ] **A2: Run the named regression against the defect.**
  Run `bazel test --config=release //src/ui/desktop/widgets:desktop_logging_run_test --test_arg=--gtest_filter=DesktopLoggingRunTest.replacementDuringSampleErrorDiscardsOldBatch`.
  Expect failure showing `99` in B's cache/output.
- [ ] **A3: Add coordinator-local identity in the owned implementation/header.**
  Advance an unsigned run counter on accepted startup and release. Capture it
  in `void DesktopLoggingRun::handleSamples(const QVector<fastecu::logging::LogSample>& samples)`.
  Require matching identity, active snapshot, and non-shutdown state at the
  checkpoints defined by spec A. Keep public signatures and engine generation
  fencing unchanged; do not hold borrowed run data across emitted signals.
- [ ] **A4: Add the remaining reentrancy tests.**
  `replacementDuringPresentationDiscardsOldCsvRow` uses identical selections
  and a one-shot presentation subscriber; B's CSV remains header-only until B's
  own sample arrives. `shutdownDuringSampleErrorDiscardsOldBatch` asserts no
  active snapshot, no open output, and no later presentation.
  `failedReplacementDoesNotResumeOldBatch` attempts a startup that returns an
  existing engine error after stopping A; assert no active cache/output receives
  `99`. Preserve stop-only and unchanged-run healthy-peer tests; add a missing
  case rather than duplicating existing coverage.
- [ ] **A5: Verify and mutation-check.** Run the full focused target. Temporarily
  restore presence-only guards; A1 must fail. Restore identity guards and rerun
  the focused target; expect all tests passing. Return the diff and evidence for
  a task review and named-path commit `fix(logging): fence samples across run replacement`.

## Task B: MUT memory integrity and ranges

**Files:** Modify [implementation](../../../src/backend/protocol/mut_memory.cpp),
[header](../../../src/backend/protocol/mut_memory.h), and
[tests](../../../src/backend/protocol/mut_memory_test.cpp).

**Interfaces:** Existing `mutdma::ReadMemory(IKlineTransport&, std::uint16_t,
std::size_t, const fastecu::ICancellationToken&) -> fastecu::Result<bytes::Bytes>`
and `mutdma::WriteMemory(IKlineTransport&, std::uint16_t, bytes::ByteView,
const fastecu::ICancellationToken&) -> fastecu::Status`. No other task consumes
these helpers; do not change driver or transport interfaces.

- [ ] **B1: Replace partial/gapped characterizations with failing regressions.**
  Reuse `ScriptChunk` and `Counting`. Name tests `ReadFailsWhenALaterChunkFails`
  and `ReadFailsWhenPollReturnsNoFrame`; expect `IsErr(kDisconnected)` and
  `IsErr(kTimeout)` respectively. Script no later chunk and assert
  `ScriptConsumed()` so extra I/O cannot pass unnoticed.
  Add `ReadRejectsAddressWrapWithoutIo` for `0xFFFF, 2`, and
  `WriteRejectsCrossingWindowWithoutIo` for `0xBFFF, {0x01, 0x02}`; both expect
  `IsErr(kInvalidConfig)` and consumed empty transport scripts.
- [ ] **B2: Run B1 before fixing code.**
  Run `bazel test --config=release //src/backend/protocol:mut_memory_test`.
  Expect failure on partial-read, empty-poll, or range assertions.
- [ ] **B3: Implement range checks and exact-read errors.**
  In the existing functions require `len <= 0x10000 - addr` for reads and valid
  start plus `data.size() <= 0xC000 - addr` for writes, before I/O. Retain zero
  length behavior. Propagate setup/poll errors immediately; empty values return
  `kTimeout`; unexpected reassembled chunk size returns `kBadResponse`.
  Preserve chunk size 40, timeouts, commands and driver ACK policy. Update the
  header to describe complete-read success and the entire write interval.
- [ ] **B4: Add parameterized boundary/failure cases.**
  Retain lengths 39/40/41/80; add `0xFFFF, 1` success and zero-length success.
  Reject a `std::numeric_limits<std::size_t>::max()` read without allocation/I/O.
  Test one-byte `0xBFFF` success and `0xBFD5` lengths 43/44 success/rejection;
  script ACKs using existing write fixtures. Cover full-window range, one byte
  too many, and empty writes at valid and invalid starts. Test first/later
  handshake and poll errors, invalid-checksum and short payload replies, and
  `ReadPropagatesCancellationAfterFirstChunk` with `IsErr(kCancelled)`.
  Every failure must leave no later exchange; preserve error details for errors
  returned by ports. Use `TEST_P` or `SCOPED_TRACE` for case matrices.
- [ ] **B5: Verify and mutation-check.**
  Run `bazel test --config=release //src/backend/protocol:mut_memory_test //src/backend/protocol:test_driver`.
  Separately restore partial-success, skipped-empty-poll, start-only write guard,
  and missing read-wrap guard; each corresponding regression must fail.
  Restore fixes and rerun both targets. Return evidence for task review and
  named-path commit `fix(protocol): enforce MUT memory integrity and bounds`.

## Task C: DTC characterization and description

**Files:** Modify [implementation](../../../src/backend/diagnostics/dtc_session.cpp)
and [tests](../../../src/backend/diagnostics/dtc_session_test.cpp).

**Interfaces:** Unchanged `Result<DtcReport> RunDtcSession(const DtcRequest&,
IDiagnosticLink&, IClock&, const ICancellationToken&, IEventSink&)` in
`fastecu::diagnostics`. Use existing `Harness`, `Lines`, and scripted link calls.
Produces only the corrected CAN-init log text; no cross-task dependency.

- [ ] **C1: Add named characterization cases.**
  `ClearShortFrameThenAckSucceeds`, `ClearShortFrameThenSilenceFails`,
  `ClearNrcStopsReading`, and `ClearWrongServiceStopsReading` reuse the existing
  clear setup, asserting success/`kBadResponse`, exact existing error logs,
  no extra reads, and final header clearing/reset.
  `CanInitMissingOrShortReplyStopsBeforeVehicleInfo` covers silence and lengths
  0–4; `CanInitWrongServiceStopsBeforeVehicleInfo` covers non-`0x41`/non-`0x7F`
  services. Assert `kBadResponse`, hex log where applicable, no later requests,
  and cleanup. `FastInitCancellationDoesNotFallBack` injects `kCancelled` after
  open/init and asserts no five-baud exchange and cleanup.
- [ ] **C2: Verify characterization against current code.**
  Run `bazel test --config=release //src/backend/diagnostics:dtc_session_test`.
  Expect pass. Investigate any disagreement before changing behavior.
- [ ] **C3: Change the NRC regression expectation and prove failure.**
  Rename the offset-three test `Iso15765InitNrcDescribesActualPayload` and assert
  literal `Wrong response from ECU: Subfunction not supported` for
  `00 00 07 E8 7F 01 12`, plus `IsErr(kBadResponse)`.
  Run the focused target; expect that test to fail on `Not a valid answer`.
- [ ] **C4: Correct only the description slice.** In `CanInit()` change
  `NrcDescription(bytes::ByteView(f).subspan(3))` to `subspan(4)` and update its
  comment. Preserve every other gate, exchange, fallback and log string.
- [ ] **C5: Verify and mutation-check.** Run the focused target; expect pass.
  Restore offset 3 temporarily; the named NRC regression must fail. Restore
  offset 4 and rerun; return evidence for review and named-path commit
  `fix(diagnostics): describe CAN init NRC payload correctly`.

## Task D: MapPack validation evidence

**Files:** Modify only [MapPack tests](../../../src/backend/definition/mappack_converter_test.cpp).

**Interfaces:** Existing `fastecu::definition::ConvertMappackCsv(std::string_view csv,
std::string_view ecu_id) -> Result<std::string>`. Use existing hand-authored `Csv`
and `kHeader` fixtures; no production change or cross-task dependency.

- [ ] **D1: Add named parameterized invalid-input cases.**
  Cover all spec D cases with assertions shaped as
  `IsErrWith(kInvalidConfig, AllOf(HasSubstr("row 2"), HasSubstr("Fieldvalues.Factor"), HasSubstr("finite nonzero")))`.
  Use numeric CSV columns for quoting failures, row 1/named columns for header
  errors, and record-row/named fields for value failures. Add
  `InvalidThirdRecordReportsItsOwnRow`: valid header and first data record,
  invalid factor in the next record, expecting `row 3`.
- [ ] **D2: Add accepted neighbors and inspect XML.**
  `AcceptsPrecisionBoundaries` checks formats `#` for 0 and `#0.` plus 100 zeros
  for 100; rejection cases cover -1/101. `AcceptsUppercaseHexAddressPrefix`
  checks `0X1234` emits storage address `0x1234`; overflow case uses
  `0x10000000000000000`. Retain existing quoting and dimensionality tests.
- [ ] **D3: Verify evidence without changing conversion policy.**
  Run `bazel test --config=release //src/backend/definition:mappack_converter_test`.
  Expect pass; coverage additions need not fail against correct production code.
  Report any contract discrepancy instead of fixing production in this task.
  Return the test-only diff for review and named-path commit
  `test(definition): cover MapPack CSV validation boundaries`.

## Task E: Independent checksum vectors

**Files:** Modify only [primitive tests](../../../src/algorithms/checksum/checksum_primitives_test.cpp).

**Interfaces:** Existing `fastecu::checksum::CksAdd8(std::span<const std::uint8_t>)`,
`NegatedSum8(bytes::ByteView)`, `Crc32(bytes::ByteView)`, and
`Crc32(const unsigned char*, std::uint32_t)`. Rebalance helpers remain internal
and accept mutable byte views, offsets, observed and target values.

- [ ] **E1: Add fixed CRC vectors and pointer boundaries.**
  Expected literals: empty `0x00000000`, `{0x00}` `0xB2D311D3`, `{0x80}`
  `0xE876B489`, and `{0xAA,0x55,0xAA,0x55}` `0xA5C473EB`.
  These were independently calculated by XORing each byte into initial
  `0xFFFFFFFF`, performing eight reflected shifts with polynomial `0x5AA5A55A`,
  and XORing final `0xFFFFFFFF`; document that provenance.
  Assert null/zero and non-null/zero pointer calls return zero. Add
  `ChecksumsOnlyTheRequestedSubview` using sentinel-surrounded `{0x80}` and
  assert CRC `0xE876B489`, additive `0x80`, and negated sum `0x80` for the subview.
- [ ] **E2: Add additive and rebalance boundary vectors.**
  `CksAdd8`: `{0x80}` → `0x80`, `{0xFF,0x01,0x01}` → `0x02`,
  `{0x80,0x7F,0x80}` → `0x80`.
  `NegatedSum8`: empty → `0`, `{0xFF,0x01}` → `0`, `{0xFF,0x02}` → `0xFF`.
  At offset 1 rebalance stored U16 `0x1234` with observed `0x0020`, target
  `0x0010` to `0x1224`; U32 `0x12345678` to `0x12345668`. Assert leading/trailing
  sentinels remain unchanged with container matchers.
- [ ] **E3: Verify test-only coverage.**
  Run `bazel test --config=release //src/algorithms/checksum:checksum_primitives_test`.
  Expect pass. Do not add invalid-span cases or modify production CRC/layout
  policy. Return vectors/provenance and the diff for review and named-path commit
  `test(checksum): cover primitive golden and boundary vectors`.

## Task F: Coordinator integration and delivery

**Files:** Coordinator only: [MainWindow tests](../../../src/ui/desktop/widgets/mainwindow_test.cpp),
[logging contracts](../../reference/logging-contracts.md),
[desktop contracts](../../reference/desktop-contracts.md),
[technical debt](../../tech-debt.md), generated BUILD files if Gazelle changes
them, and the applicable qualification records selected via the matrix.

**Interfaces:** Consumes reviewed A–E diffs and focused evidence. Produces an
integrated branch with updated contracts and measured verification results.

- [ ] **F1: Update the MainWindow regression after A.** Replace its equivalent
  replacement characterization with the empty-new-cache expectation for old
  rpm `99`, and verify B's own subsequent sample is accepted. Run
  `bazel test --config=release //src/ui/desktop/widgets:test_mainwindow //src/ui/desktop/widgets:desktop_logging_run_test`.
  Expect all tests passing; do not change window production behavior.
- [ ] **F2: Update enduring documents.** Logging reference states that replacement
  discards the suspended batch. Diagnostic-tools reference records offset 4.
  Remove resolved A/B debt only with evidence; narrow C to any remaining missing
  cases, retaining the broad coverage goal. Consult the matrix/checklists and
  record MUT outcome divergence without claiming bench qualification.
- [ ] **F3: Regenerate and review BUILD output.** Run
  `python3 scripts/gazelle_check.py --fix`. Expect no new targets or visibility
  widening; review any generated differences rather than hand-editing targets.
- [ ] **F4: Run final gates.** Run `bazel build --config=release //:fastecu`,
  `bazel test --config=release //...`, `prek run --all-files`, and
  `bazel run //:clang_tidy_report_changed`. Require successful exits; record
  blocked/unrun checks accurately. Obtain established platform CI/packaging
  results before delivery claims that depend on them.
- [ ] **F5: Review and commit coordinator-owned paths.** Verify the full diff
  matches scope and no unrelated files were staged. Commit the integration
  changes, prepare the PR description from actual evidence, and push/open the
  PR only when authorized. Keep specs/plans until their original contents are
  preserved in delivered history and enduring knowledge is extracted.

## Plan review

All spec requirements map to A–F. Public signatures remain unchanged and tasks
do not exchange new types. The five Review Focus conditions have named tests in
their owning task. For this documentation-only change, run
`prek run lychee --all-files` and scoped diff review; do not run application gates.

User review of this plan is required before implementation. Preserve the chosen
subagent execution method; approval of the spec did not approve a plan that did
not yet exist.
