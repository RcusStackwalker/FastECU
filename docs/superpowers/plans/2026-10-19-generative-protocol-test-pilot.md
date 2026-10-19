# Generative Protocol Test Pilot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Determine whether named generated scenarios preserve and strengthen the two N83M executor suites while removing at least 222 net NLOC with at most 25% ordinary runtime growth.

**Architecture:** Extend the existing scripted CAN fake additively, then use independently constructed scripts and known-answer payloads against the real executors. Share operation-specific typed/parameterized tests across the two families, retaining distinctive family scripts and frozen boundary requests. Measure against the pinned source baseline; report failed experimental gates without weakening coverage.

**Tech Stack:** Qt-free C++23 test support, GoogleTest/Google Mock, Bazel release configuration, Gazelle, Lizard 1.24.1, repository prek and static-analysis gates.

**Spec:** [Generative protocol test pilot design](../specs/2026-10-19-generative-protocol-test-pilot-design.md). Native execution was authorized on 2026-10-19. The [results record](../../reference/generative-protocol-test-pilot-results.md) documents the bounded-migration ruling and failed size/runtime gates.

## Global Constraints

- “Compare against master at `b866782bcb6295d4dde8e16eb95567bf7c855e48`.”
- “The delivered change is test-only: test sources, test support, their generated Bazel targets, and documentation.”
- “At least **222 net NLOC** is removed, including all new/changed support code and its tests.”
- “Median ordinary test execution time grows by at most **25%**”.
- “Use five measured runs after warm-up with Bazel test-result caching bypassed.”
- “Neither its code nor its reported delta is part of this experiment. Leave it untouched.” This refers to PR #756.
- “Record attempts before early returns without adding token polls or changing return values.”
- “Preserve its scripting API and the existing queue-consumption meaning of `ScriptConsumed()`.”
- “Select fault positions by named operation and occurrence, not a global exchange number or cancellation-token poll count.”
- “Do not derive expected address windows or policies by reading them back from the production plan.”
- “Do not impose a universal stop-on-any-error rule”.
- “Hardware status remains unchanged.” No new qualification or another family migration is authorized.
- No production changes, emulator, interpreter, transcript parser, reference cipher, fuzzing framework, guarded transport, tooling retirement, warning suppression, or visibility widening.
- Follow the [coding conventions](../../coding-style.md), [sharing contracts](../../reference/flash-contracts.md), [family differences](../../reference/denso-iso15765-families.md), and [qualification matrix](../../flash-qualification-matrix.md). Use existing domain language; the 4M setup is not a “tolerant probe”.

## Review Focus

1. A mismatched or extra operation followed by recovery must remain visible even when the executor swallows its error.
2. No frame, an empty PDU inside a CAN envelope, a truncated raw frame, and an actual transport error must retain their different family/site outcomes.
3. Adjacent-page swaps, a one-page shift, and subtracting the nonzero image origin incorrectly must change independent expected payload/content.
4. Cancellation before acceptance of an operation must be distinguishable from an accepted request and must prove the intended boundary was reached.
5. Reusing immutable fixtures must never reuse transport, observer, clock, or cancellation state, including when cases run in a different order.

Each item has explicit assertions in Tasks 2–5.

---

## File Structure and Task Interfaces

All new C++ files are package-owned test support. Keep unrelated working-tree files untouched. Use a feature branch/worktree at execution time via `superpowers:using-git-worktrees`; the current documentation branch is not an instruction to implement here.

| Path | Responsibility |
| --- | --- |
| `docs/reference/generative-protocol-test-pilot-results.md` (new) | Pinned baseline, assertion replacement inventory, measured results, mutation evidence, gate verdict and limitations |
| `src/backend/flash/testing/scripted_can_flash_transport.h` | Persistent first unexpected-operation diagnostic and compact attempted-I/O observations |
| `src/backend/flash/testing/scripted_flash_transports_test.cpp` | Additive fake-contract self-tests, retaining existing recovery/consumption tests |
| `src/backend/flash/ecu/testing/n83m_payloads.h` (new) | Literal known-answer dictionary and two deterministic complete-image fixtures |
| `src/backend/flash/ecu/testing/n83m_payloads_test.cpp` (new) | Payload samples, uniqueness, origin sensitivity and fixture determinism |
| `src/backend/flash/ecu/testing/n83m_executor_scenarios.h` (new) | Small operation-specific case records, script construction/order expectations and common family tests |
| `src/backend/flash/ecu/testing/n83m_executor_scenarios_test.cpp` (new) | Verify scenario construction, order checking and cancellation injection without testing production behavior twice |
| `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp` | 1.5M literal traits, adapters, frozen boundaries and scenario instantiation |
| `src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp` | 4M literal traits, adapters, distinctive combined faults and scenario instantiation |
| `src/backend/flash/ecu/testing/BUILD.bazel`, `src/backend/flash/testing/BUILD.bazel`, `src/backend/flash/ecu/BUILD.bazel` | Gazelle-generated support/test dependencies; inspect generation rather than hand-maintaining managed targets |
| `docs/adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md`, `docs/superpowers/specs/2026-10-18-size-reduction-strategy.md` | Scope and disposition of this reversible two-family exception |
| `docs/coding-style.md`, `docs/tooling.md`, `docs/README.md` | Semantic operator-information contract, scoped measurement pointer and durable evidence navigation |

Avoid modifying `can_executor_run.h`, `denso_can_frames.h` or conformance support unless a concrete two-consumer saving requires it. Count any such change and its affected tests. Keep the independent algorithm and plan suites intact.

### Task 1: Establish the pinned comparison and obligation ledger

**Files:** Create `docs/reference/generative-protocol-test-pilot-results.md`.

**Interfaces:** Produces the baseline XML inventory, source/file measurement set and runtime manifest consumed by Tasks 4–6. This task changes documentation only; the commands below characterize existing software.

- [x] **Step 1: Create an isolated baseline checkout at the exact SHA and record the environment.** Record source SHA, candidate starting SHA, `.bazelversion`, compiler/Qt versions, release flags, CPU/OS and scheduling flags. Preserve raw XML/logs outside tracked sources in a task-specific temporary evidence directory; record content hashes and the eventual durable artifact location. Do not use a moving master or PR #756.
- [x] **Step 2: Discover and run baseline tests.** Run the two executor targets, their corresponding `_can_plan_test` targets, common/SSM/support tests, and the union of test consumers returned by these queries:

```sh
bazel query 'kind(".*_test rule", rdeps(//..., //src/backend/flash/testing:scripted_flash_transports))'
bazel query 'kind(".*_test rule", rdeps(//..., //src/backend/flash/ecu/testing:can_executor_conformance))'
bazel test --config=release //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_executor_test //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test //src/backend/flash/ecu:subaru_denso_1n83m_1_5m_can_plan_test //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_plan_test //src/backend/flash/ecu:denso_iso15765_can_common_test //src/algorithms/protocol/ssm:ssm_protocol_core_test //src/backend/flash/testing:scripted_flash_transports_test
```

Expected: all targets pass; enumerate XML names/parameter rows, checking the spec's predicted 63 executor cases rather than treating 63 as a gate. Record existing failures separately and resolve comparison validity before claiming preservation.

- [x] **Step 3: Expand the spec inventory into an assertion ledger.** One row per declaration and internal branch/parameter outcome, including all eight inherited cases per family, five checksum outcomes, both short-page stop outcomes, identity errors, OBK misses/errors and jump variants. Columns: baseline case/branch, protected assertion, candidate case/assertion, removed assertion classification/reason. Classify only exact narration and incidental progress as incidental; preserve family identity, complete bytes, cleanup, admission-before-configuration, timing and meaningful progress.
- [x] **Step 4: Capture size and runtime baseline.** Run `python3 scripts/nloc_loop.py measure` on the formatted pinned tree and retain per-file values from the pinned `nloc` hook. Confirm or explain the published 523/584 suite NLOC. Warm up the two executor targets plus existing affected support self-tests, then run that fixed target set five times using `bazel test --config=release --nocache_test_results --jobs=1 --local_test_jobs=1 <targets>`. Copy XML after each run before the next overwrites it. Sum test execution durations per run, then take the median; exclude build/wall time. Record the exact target manifest and all five sums.
- [x] **Step 5: Commit the baseline ledger.** Stage only `docs/reference/generative-protocol-test-pilot-results.md`; commit as `docs: establish generative protocol pilot baseline`.

### Task 2: Make swallowed fake violations and all I/O attempts observable

**Files:** Modify scripted CAN transport and its existing test file; regenerate `src/backend/flash/testing/BUILD.bazel` if needed.

**Interfaces:** Add these nested types/accessors on `ScriptedCanFlashTransport`:

```cpp
enum class OperationKind { kWrite, kRead };
struct OperationAttempt {
    OperationKind kind;
    bool accepted; // matched write or dequeued/blocking read; false before acceptance
    std::optional<ErrorKind> error; // includes queued errors and cancellation
    bool operator==(const OperationAttempt&) const = default;
};
const std::vector<OperationAttempt>& OperationAttempts() const;
const std::optional<std::string>& FirstUnexpectedOperation() const;
```

No full payload copies in observations. Existing timeout/lifecycle accessors remain authoritative. A queued error is an accepted read even if its kind is `kInternal`; a cancellation rejected before queue consumption is unaccepted. An unblocked blocking read is accepted with `kCancelled`.

- [x] **Step 1: Add failing GoogleTests.** `CanMismatchRecoveryRetainsViolation` performs wrong write, then correct write and read: assert `ScriptConsumed()` true, first diagnostic still present, attempts exactly write/false/Internal, write/true/no-error, read/true/no-error. `CanExtraSwallowedOperationsRemainVisible` checks exhausted write and exhausted read independently. `CanQueuedInternalErrorIsNotUnexpected` asserts no violation and read/true/Internal. `CanCancelledAttemptsAreRecorded` checks cancelled write/read leave queues untouched and produce false/Cancelled; extend the existing blocking-read test for true/Cancelled. Assert no extra token polls with a counting token whose `Cancelled()` result is stable.
- [x] **Step 2: Run `bazel test --config=release //src/backend/flash/testing:scripted_flash_transports_test`.** Expected: compile failure for the new accessors/types before implementation.
- [x] **Step 3: Implement the additive observations.** Append the attempt at entry, update its outcome at each existing return, and latch the first diagnostic only in mismatched/exhausted expectation branches. Preserve queue consumption, blocking synchronization, timeouts and return values; do not add cancellation checks. No global order enforcement in this fake.
- [x] **Step 4: Regenerate and verify.** Run `python3 scripts/gazelle_check.py --fix`; inspect generated changes. Run the self-test and all discovered reverse-dependent test targets from Task 1. Expected: existing mismatch-then-correct consumption behavior and all consumers still pass.
- [x] **Step 5: Commit named changed paths** as `test: retain scripted CAN violations and attempted operations`.

### Task 3: Add independently paired image fixtures

**Files:** Create `n83m_payloads.h` and `n83m_payloads_test.cpp`; regenerate the package BUILD file.

**Interfaces:** In `fastecu::flash::testing`:

```cpp
enum class N83mPattern { kForward, kReverse };
struct N83mPayload { bytes::Bytes plain; bytes::Bytes wire; };
N83mPayload MakeN83mPayload(std::size_t image_size, N83mPattern pattern);
```

`image_size` is a supported complete image size (`0x184000` or `0x3E4000`). Both outputs index from image origin, not the selected flash-block origin. No production crypto calls or production-table dependency.

Use four literal pairs with provenance comments: `00010203/E0D3852B`, `FCFDFEFF/C5FE4B39` from Denso common tests, and `00112233/ED9FD931`, `44556677/AFACD594` from SSM tests. For absolute image page `p = byte_offset / 256` and word `w = (byte_offset % 256) / 4`, choose dictionary index `(p >> (2 * (w % 16))) & 3` for forward and `3 - ((p >> (2 * (15 - w % 16))) & 3)` for reverse. This encodes every page's identity in 16 words and repeats that identity within, never across, pages.

- [x] **Step 1: Add failing tests with literal samples.** `KnownAnswerSamples` checks forward page 0 words 0/1 are `00010203/E0D3852B`, page 1 word 0 is `FCFDFEFF/C5FE4B39`, page 2 word 0 is `00112233/ED9FD931`, page 3 word 0 is `44556677/AFACD594`. Reverse page 0 word 0 is `44556677/AFACD594`, page 1 word 15 is `00112233/ED9FD931`. At image offset `0x10000`, forward word 4 is pair 1, distinguishing it from image offset zero. Assert sizes exactly match both image sizes.
- [x] **Step 2: Add structural fixture tests.** `EveryPageHasDistinctIdentity` verifies all pages are unique for both patterns and image sizes, including plaintext and ciphertext, using a set of page views or compact identity keys without quadratic copying. `PatternsAreDistinctAndRepeatable` compares independent constructions and verifies first/middle/last pages differ, patterns differ on every page, and page data at image offset `0x10000` differs from offset zero. Compare literal sample words, not expectations generated by the same index function.
- [x] **Step 3: Generate the test target and run `bazel test --config=release //src/backend/flash/ecu/testing:n83m_payloads_test`.** Expected: missing implementation failure.
- [x] **Step 4: Implement `MakeN83mPayload` using only the committed dictionary and selection rule.** Cache immutable fixtures at suite scope only in consumers; every executor run gets fresh mutable collaborators. Do not replace old frozen boundary requests with these generated expectations.
- [x] **Step 5: Run fixture, SSM and Denso common tests.** Expected: all pass, preserving the independent algorithm evidence. Regenerate/review BUILD changes and commit named paths as `test: add independent N83M payload patterns`.

Execution ruling: steps below were closed with a bounded migration. Existing
fault, short-page, jump and close obligations remain in their original cases;
only proven read-success/first-page-disconnect predecessors were removed. The
[results](../../reference/generative-protocol-test-pilot-results.md#final-review-disposition)
record the narrower conclusion, review fixes and numerical failures. Checked
steps do not imply that a full compression trial or external platform gates
completed.

### Task 4: Introduce operation-specific scenarios and migrate 1.5M

**Files:** Create `n83m_executor_scenarios.h` and its self-test; modify the 1.5M executor test, generated BUILD files and results ledger.

**Interfaces:** Shared support uses the Task 2 observations and Task 3 payloads. Define small records `ReadSuccessCase { bool in_car; N83mPattern pattern; }`, `ReadSetupCase { SetupSite site; ReplyShape reply; }` and `TransferFaultCase { TransferOperation operation; FaultPosition position; }`. Enumerators: `SetupSite::{kDownload,kUpload}`, `ReplyShape::{kSuccess,kNoFrame,kEmptyPdu,kTruncatedRaw,kMismatch,kTransportError}`, `TransferOperation::{kRead,kWrite}`, `FaultPosition::{kFirst,kMiddle,kLast}`. Keep close/checksum and jump outcomes separate; do not combine these into a universal scenario record.

Use a `N83m15mTraits` adapter in the family file: `using Executor`, `static Result<FlashPlan> ReadPlan()`, `static Result<FlashPlan> WritePlan(bytes::Bytes)`, and `static void ScriptBenchConnect(ScriptedCanFlashTransport&)` / `ScriptInCarConnect(ScriptedCanFlashTransport&)`. Keep existing conformance forwarders and `HandBuiltPlan`. Shared entry points are `template<class Traits> void ExpectN83mRead(ReadSuccessCase)`, `ExpectN83mSetup(ReadSetupCase)`, and `ExpectN83mTransferFault(TransferFaultCase)`; define them in the new header. Traits carry independent literal image/block sizes and explicit setup outcome expectations, never values extracted from a plan.

Use separate GoogleTest parameter fixtures for the three records, with custom alphanumeric names exposing family, site/path, pattern and outcome. Add typed tests for identical preserved obligations where no extra parameter is needed. Keep distinctive cases local. Share only identical frozen literal request buffers, preserving their old-pattern execution case independently of the two new pattern writes.

- [x] **Step 1: Add scenario-construction self-tests before migration.** Expose `void ExpectN83mOperations(const ScriptedCanFlashTransport&, const std::vector<ScriptedCanFlashTransport::OperationAttempt>&)` as the shared assertion boundary. `WrongInterleavingFails` feeds an otherwise consumed W,W,R,R script against independently supplied W,R,W,R expectations and uses GoogleTest SPI to assert failure. `RecoveredViolationFails` requires failure despite later successful consumption. `InjectedErrorIsAllowed` passes a deliberately queued Internal error. `FreshRunsDoNotShareState` executes a success/error/success sequence with fresh collaborators and asserts identical success observations. Expected order is built while scripting expected requests/replies, never copied from actual observations.
- [x] **Step 2: Run the new support test.** `bazel test --config=release //src/backend/flash/ecu/testing:n83m_executor_scenarios_test` must fail for missing interfaces. Implement the minimal order/violation assertion and operation-specific script builders. Reuse existing preliminary and frame helpers; construct only the executed prefix and permitted suffix on failure.
- [x] **Step 3: Add 1.5M replacement cases alongside predecessors.** Pin literals: image origin `0x08F9C000`, block start `0x08FAC000`, length `0x173F00`, image size `0x184000`, page/chunk size `0x100`, lead pad `0x10000`, tail `0x100`. Cover bench/in-car × forward/reverse reads and forward/reverse writes, plus retained old-pattern frozen first/last requests. Assert full output equality including padding, complete write bytes/order, operation/read-bytes shape, erase-before-transfer, meaningful progress, lifecycle/configuration and seed/key timing. Check 1.5M operator logs/notices retain its family identity and do not name 4M.
- [x] **Step 4: Add the bounded setup and transfer matrix.** Both `0x34` and `0x35` get all six `ReplyShape` rows. Use literal empty envelope `000007E8`, truncated raw `000007`, mismatched PDU `7F3431` or `7F3531`, and `QueueError(kDisconnected)`; For strict 1.5M, no frame returns `kTimeout`; empty PDU, truncated raw and mismatched PDU return `kBadResponse`; injected disconnection returns `kDisconnected`. Assert these with `IsErr(...)` in named rows; success returns `IsOk()`. Successful setup continues through patterned transfer; failing setup permits no later I/O. Read/write fault indices are `0`, `page_count / 2`, `page_count - 1` using literal expected length. First injects disconnected transport, middle cancels, last rejects reply (`7FB731` read, `7FB631` write). Use forward pattern only. Assert no later transfer/stop/close/checksum except the case's expressly permitted cleanup; retain existing combined faults.
- [x] **Step 5: Prove injection and timing boundaries.** For new middle cancellation, define a pilot-local `CancelAtTransferProgressSink(ManualCancellationToken&, std::string_view phase, int completed_bytes)` overriding `PhaseProgress(const PhaseProgressEvent&)` and exposing `bool Triggered() const`. Copy the phase into owned storage. Cancel at `done == (page_count / 2) * 0x100`, in `Read ROM` or `Write ROM` respectively. Assert `Triggered()` true, exactly `page_count / 2` successful transfer exchanges, `IsErr(kCancelled)`, and no request for the next page/chunk: the loop-top check rejects it before transport acceptance. Task 2 separately pins cancelled attempts inside I/O. Preserve the existing log-triggered close/stop cancellation cases unchanged. Use a local `MockClock` delegating to `RecordingClock`, sampling `OperationAttempts().size()` at each contractual sleep, to pin settle placement as well as duration.
- [x] **Step 6: Migrate the remaining ledger rows without losing branches.** Preserve admission/conformance, OBK variants, nonfatal identity failures, absent selector, fatal seed, short pages and best-effort stop, jump exhaustion/acknowledgment/cancellation, twenty receive-only erase attempts, six mixed-fault closes, close-settle cancellation, and all five checksum outcomes. Keep 1.5M pending reread at `500ms`. Register each moved internal branch as a named case. Record each exact candidate assertion before removing its predecessor. No full diagnostic sentences need migration unless they are still used to trigger a boundary.
- [x] **Step 7: Verify coexistence, then remove mapped predecessors.** Run the new support target, 1.5M executor/plan, common and SSM tests before deletion and again after deletion. Expected: all pass and the ledger has no unmapped obligations. Run parameter cases shuffled with a recorded seed using `--test_arg=--gtest_shuffle --test_arg=--gtest_random_seed=431` to check fresh state. Measure interim scoped NLOC including all support; report the trajectory without weakening obligations to meet the threshold.
- [x] **Step 8: Regenerate/review and commit named paths** as `test: migrate N83M 1.5M to named protocol scenarios`.

### Task 5: Migrate 4M while preserving its distinct policies

**Files:** Modify 4M executor test, shared scenarios only where required, generated BUILD files and results ledger.

**Interfaces:** Add `N83m4mTraits` matching Task 4's adapter signatures. Literal differences: block length `0x3D3F00`, image size `0x3E4000`; same origin/start/page/lead/tail values. Actual executor and plan-builder selection is independent of these expected literals.

- [x] **Step 1: Add failing 4M cases alongside predecessors.** Instantiate all Task 4 success/setup/transfer records with named 4M outcomes. At each setup site, assert `IsOk()` for success, no frame, empty PDU and mismatched PDU; assert `IsErr(kBadResponse)` for truncated raw frames and `IsErr(kDisconnected)` for injected transport failure. The channel rejects the truncated envelope before tolerant setup sees a PDU. This distinction is visible in `can_flash_uds_channel.cpp` and must not be collapsed into absence. Preserve the existing combined malformed bench and in-car setup cases. Do not normalize the family by changing production.
- [x] **Step 2: Verify policy detectors before removing old tests.** Assert the first matching bench jump reply is discarded, the subsequent wait/read counts and inter-read `50ms` settle occur at the expected operations, in-car variants preserve existing `200ms` reads, and pending checksum reread is `3000ms` instead of `500ms`. Preserve all existing acknowledgment variants and unchecked final-receive behavior. Ensure every ledger branch has its candidate name/assertions.
- [x] **Step 3: Run coexistence tests and complete the migration.** Run both executor targets and their plan tests, common/SSM tests and all new support tests; expected PASS. Remove only mapped predecessors, retaining frozen old-pattern boundaries and distinctive family-local cases. Rerun the same targets, then shuffled cases at seed 431. Expected: no state leakage or changed result because of ordering. Record NLOC for both suite files and every changed/new support file.
- [x] **Step 4: Regenerate/review and commit named paths** as `test: migrate N83M 4M to named protocol scenarios`.

### Task 6: Demonstrate detection and publish the experimental verdict

**Files:** Update results ledger and the five documentation owners in the file map. Temporary production mutations must never be staged or delivered.

**Interfaces:** Consumes Task 1 baseline manifest, assertion ledger and Tasks 2–5's named test cases. Produces a reviewable pass/fail determination for each of the three independent acceptance gates.

- [x] **Step 1: Run paired deliberate-mutation experiments.** For each row below, save the exact patch, apply it independently to baseline and candidate checkouts, run the smallest named detector, record the failing assertion or explicit survival, then restore and verify production files match their checkout revision. Never infer detection from a crash. Use actual family/common call sites identified in source; do not change the test's own expectation to manufacture a kill.

| Temporary defect | Required candidate assertion |
| --- | --- |
| Strict setup continues; tolerant setup aborts (both sites) | Named setup outcome plus exact allowed operation sequence |
| Ignore a fatal exchange result | Original error kind and absence of forbidden subsequent I/O |
| Add swallowed unexpected read or write | Persistent violation/order failure, including after recovery |
| Shift image indexing by `0x10000`; swap/omit a page/chunk | Independent full patterned content/request comparison |
| Truncate final read/write iteration | Complete image/bytes, last request and operation-count assertion |
| Change bench first-reply handling | Family-specific read count/order and sleep placement |
| Change pending checksum timeout | Named pending checksum timeout assertion (`500ms` / `3000ms`) |
| Move/remove cancellation boundary | Trigger proof plus completed/attempted operations and no forbidden suffix |

Record baseline/candidate detector names and assertion text. Explain pre-existing baseline gaps separately from lost detection. Explain equivalent/unreachable mutants, including the redundant post-admission TestWrite guard. If a newly exposed production defect blocks expected behavior, report it separately; do not fix production in this pilot.

- [x] **Step 2: Run ordinary correctness and repository gates on the restored formatted candidate.** Regenerate Gazelle first, inspect the diff, then run:

```sh
python3 scripts/gazelle_check.py --fix
prek run --all-files
bazel build --config=release //:fastecu
bazel test --config=release //...
bazel run //:clang_tidy_report_changed
```

Expected: successful gates; no production changes, warning exceptions, visibility widening or relaxed address guards. Run discovered reverse-dependent consumers explicitly if not included in `//...`. Record platform CI/packaging results from the eventual PR; unavailable platform runs remain pending and preclude a claim that all delivery gates passed. No hardware qualification claim.

- [x] **Step 3: Repeat matched runtime and NLOC measurement.** Candidate runtime set is the same two executors and existing affected support self-tests as baseline plus genuinely new support self-tests. Use Task 1's machine/toolchain/configuration/scheduling, one warm-up and five uncached runs; preserve each XML and summed duration. Record both medians and `(candidate / baseline - 1) * 100`; PASS requires at most 25%. Record build and mutation times separately. For size, sum baseline and candidate per-file NLOC over the union of both suites and every new/changed support/test C++ file, including files outside the initial map; new files have baseline zero. PASS requires baseline minus candidate at least 222. Report whole-tree and production delta too: production must be zero, and unrelated changes/relocation/excluded data cannot count as savings. A relevant rebase requires checking comparability and repeating affected measurements on both sides.
- [x] **Step 4: Update owner documents and disposition.** Add the scoped reversible experiment and results link to ADR 0024 and the strategy without replacing general transcript policy or touching #756. Clarify semantic operator information versus exact narration in the coding guide. Add a scoped pilot-check pointer in tooling instead of obsolete case-count guards; do not retire tooling. Link durable results from the documentation index. Results include assertion classifications, added coverage, mutation comparison, all size/runtime numbers, artifact provenance, limitations and an explicit verdict for each gate. A failed gate is a completed experimental finding; no additional families or assertion weakening. A successful result only supports a separate rollout decision.
- [x] **Step 5: Verify documentation and scoped diff.** Run `prek run lychee --all-files` and `git diff --check`; inspect all named paths and the source-baseline diff to confirm only authorized test/support/generated-target/documentation files changed. Preserve this spec and plan during review; retire them only after enduring evidence is extracted and their original contents are present in delivered Git history. Use a separate later deletion change if squash merging would otherwise erase originals.
- [x] **Step 6: Commit the verdict and scoped documentation** as `docs: record generative protocol pilot results`. Request final review with behavior, size and runtime evidence; push/create a PR only when authorized. Report pending external gates honestly.

## Plan Self-Review

- Spec coverage: Tasks 1/6 own pinned comparison and measurements; Task 2 owns additive observations and affected consumers; Task 3 owns independent payloads; Tasks 4/5 own every inventory group, additional matrices and family differences; Task 6 owns mutations, gates and documentation/disposition.
- Interfaces: Both family adapters use the same signatures; scenario records remain operation-specific; payload indices are image-relative while requests carry literal absolute block addresses.
- Review focus: Task 2 and Task 4 self-tests cover recovered/swallowed violations and interleaving; Tasks 4/5 cover reply distinctions and cancellation; Task 3 and patterned executor cases cover page identity/base offsets; shuffled fresh-state cases cover fixture reuse.
- Scope: This plan writes no production code and performs no hardware operations. Native execution produced a bounded candidate and recorded a failed size/runtime experiment. Other original obligations were retained in place under the execution ruling documented in the results; no broader migration is authorized.
