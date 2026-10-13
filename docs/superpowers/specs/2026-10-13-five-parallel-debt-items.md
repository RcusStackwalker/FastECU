# Five small technical-debt tasks for parallel implementation

Status: approved specification (user confirmation, 2026-10-13). Implementation
is not authorized by this document. The interview settled the selection and
behavior below; the implementation plan is the next review artifact.

## Purpose and scope

Deliver five bounded tasks from the [technical-debt roadmap](../../tech-debt.md),
with disjoint source and test ownership so future subagents can start independently.
Include demonstrated correctness fixes with regressions and focused coverage
growth. Exclude work requiring new bench captures, broad interface redesign,
SonarCloud scheduling without a current scan, or hardware qualification claims.

The selected approach combines three corrections and two coverage tasks. A
coverage-only batch would leave the demonstrated defects unresolved; selecting
five behavior changes would pull in larger or bench-dependent work. Splitting
MUT read and write fixes into separate tasks would create shared-file ownership,
so they remain one task.

Use the existing [domain language](../../../CONTEXT.md), [layering decisions](../../design-notes.md#layering),
and [coding and testing conventions](../../coding-style.md). No new ADR is needed:
these choices are local, reversible corrections or coverage additions within
existing boundaries.

## Ownership and parallel execution

| Task | Debt source | Exclusive implementation ownership | Focused Bazel targets |
| --- | --- | --- | --- |
| A: Logging batch fencing | Replacement-run correctness gap | [desktop_logging_run.cpp](../../../src/ui/desktop/widgets/desktop_logging_run.cpp), [header](../../../src/ui/desktop/widgets/desktop_logging_run.h), [tests](../../../src/ui/desktop/widgets/desktop_logging_run_test.cpp) | `//src/ui/desktop/widgets:desktop_logging_run_test` |
| B: MUT memory integrity and bounds | MUT memory correctness gap | [mut_memory.cpp](../../../src/backend/protocol/mut_memory.cpp), [header](../../../src/backend/protocol/mut_memory.h), [tests](../../../src/backend/protocol/mut_memory_test.cpp) | `//src/backend/protocol:mut_memory_test`, `//src/backend/protocol:test_driver` |
| C: DTC coverage and NRC description | DTC correctness/coverage gap | [dtc_session.cpp](../../../src/backend/diagnostics/dtc_session.cpp), [tests](../../../src/backend/diagnostics/dtc_session_test.cpp) | `//src/backend/diagnostics:dtc_session_test` |
| D: MapPack validation matrix | Parser/model validation coverage growth | [mappack_converter_test.cpp](../../../src/backend/definition/mappack_converter_test.cpp) | `//src/backend/definition:mappack_converter_test` |
| E: Checksum golden and boundary vectors | Checksum coverage growth | [checksum_primitives_test.cpp](../../../src/algorithms/checksum/checksum_primitives_test.cpp) | `//src/algorithms/checksum:checksum_primitives_test` |

The integration coordinator exclusively owns shared documentation, generated
BUILD updates, and the [MainWindow integration test](../../../src/ui/desktop/widgets/mainwindow_test.cpp).
No task requires another task's output, a preliminary source split, new port,
new library, or new test target. Agents report discoveries outside their assigned
files instead of expanding ownership. They return changes, focused test results,
and any remaining evidence gaps.

The five tasks are independent; simultaneous execution is limited by the
available agent slots. With four total slots and a coordinating root agent,
run three implementation agents first and schedule the remaining two as slots
free. Do not infer implementation authorization from this scheduling guidance.

Preserve existing untracked specs/plans. Older matcher-cleanup and checksum-route
plans provide context only; do not execute their changes as part of this batch.

## A: Fence suspended logging batches

### Evidence and contract change

The coordinator checks only whether a snapshot exists. Its existing
`characterizesReplacementDuringSampleNotification` test shows an error callback
stopping A and starting B, followed by A's rpm `99` entering B's fresh cache.
The window has an equivalent characterization. Engine generation checks cannot
protect a handler already executing.

The [desktop run contract](../../reference/logging-contracts.md#desktop-run-ownership)
currently records this defect. Correct it so a batch belongs to the logging run
active when its handler enters; replacement invalidates all remaining work from
that batch.

### Design

Use a coordinator-local run identity. Every accepted startup receives a distinct
identity, even with identical selections; release and shutdown invalidate it.
Capture identity at sample-handler entry. Check that the same run remains active
before each sample, after synchronous sample-error delivery, before presentation
delivery, and after presentation delivery before CSV append. Do not retain
references to run-owned data across notifications.

Once invalidated, return from the old handler. Keep engine signals, backend APIs,
thread ownership, protocol traffic, and normal completion ordering unchanged.
Healthy peers continue following a sample error when the same run remains active.
Already committed A readings are not rolled back; completion releases A's cache
and closes A's output before a replacement starts.

### Acceptance

- Replace the characterization with a regression: an error subscriber replaces
  A with B; A's later rpm `99` enters neither B's cache nor B's CSV.
- B remains active and accepts a subsequent B batch into its cache and CSV.
- A presentation subscriber replacing A with B prevents A's pending CSV append
  from writing a row to B's file, even when both runs have identical selections.
- Stop without restart during error or presentation delivery suppresses remaining
  batch work; unchanged-run error handling still permits healthy peers.
- Preserve immediate completion-subscriber restart, exclusive output creation,
  copied-view lifetime, and terminal shutdown behavior covered by existing tests.
- Coordinator updates the window characterization and runs
  `//src/ui/desktop/widgets:test_mainwindow` alongside the task's focused target.

## B: MUT memory exact reads and complete write bounds

### Evidence and contract change

The helpers remain uncalled in application code. Current header/tests explicitly
permit partial successful reads after later errors and skipped empty polls.
Write validation checks only the starting address. The chosen contract changes
these documented behaviors; do not characterize the existing partial-read result
as an already-correct complete read.

### Design

A successful read returns exactly `len` contiguous bytes. Validate
`len <= 0x10000 - addr` before I/O using a sufficiently wide unsigned type;
never narrow a wrapped address. Zero length returns an empty successful result.
Retain existing chunks of at most 40 bytes and their wire sequence/timeouts.
Propagate any setup or poll error immediately, regardless of chunk position.
An empty poll returns existing `kTimeout`. Malformed or wrong-length frames retain
the driver's `kBadResponse`. Do not retry, skip, fill, or return partial success.
Verify reassembled chunk length before appending; an unexpected length is
`kBadResponse` rather than an incomplete successful result.

For writes, require a start within inclusive `0x4000–0xBFFF` and
`data.size() <= 0xC000 - addr`, checked before initialization or transport I/O.
Use subtraction rather than unchecked end-address addition. Empty writes retain
success for valid starts and rejection for invalid starts. Keep driver framing,
acknowledgements, and cancellation behavior unchanged. Do not wire new callers
or relax the writable window.

### Acceptance

- Retain exact successful reads of 39, 40, 41 and 80 bytes.
- First and later setup/poll failures propagate their error; empty polls fail
  with `kTimeout`; malformed/short replies fail with `kBadResponse`; a scripted
  cancellation propagates `kCancelled`. No later chunk is started after failure.
- Read at `0xFFFF` for one byte succeeds; two bytes fail with `kInvalidConfig`
  without I/O. Zero length succeeds without I/O; reject oversized lengths before
  allocation or iteration.
- Write at `0xBFFF` for one byte succeeds; two bytes fail without I/O.
- Write at `0xBFD5` for 43 bytes succeeds; 44 bytes fail without I/O.
- Entire-window length fits the range check; one extra byte is rejected before
  I/O. Use scripted acknowledgements for any successful write tested end to end.
- Preserve existing low/high start rejection, and cover empty writes at valid
  and invalid starts. Update the helper header to own the corrected contract.

## C: DTC failure cases and CAN-init NRC text

### Evidence and design

`Iso15765InitNrcIsLoggedFromOffsetThree` pins the current error. For response
`00 00 07 E8 7F 01 12`, description parsing begins with `E8` and reports
`Not a valid answer`. Describe the NRC starting at byte 4 instead; the corrected
event log is `Wrong response from ECU: Subfunction not supported`.

Add scripted characterization before the correction. Change only the CAN-init
description slice and its explanatory comment. Preserve request bytes, adapter
settings, success/rejection gates, five-baud fallback, cancellation timing, and
all other log wording. This corrects description text, not wire behavior.

### Acceptance

- Clear loop: a short frame followed by `0x44` succeeds; short frame followed by
  silence fails. NRC and wrong-service replies log existing strings, fail, stop
  reading, and restore the link.
- CAN init: silence, frames of length at most four, and a service other than
  `0x41` or `0x7F` return `kBadResponse` without later vehicle-info requests.
- Fast-init cancellation returns `kCancelled`, performs no five-baud fallback,
  and restores the link. Preserve the existing best-effort cancellation contract
  for an already-cancelled run's initial exchange.
- Assert the corrected NRC message as a literal independent expectation;
  preserve normal CAN-init success and prove no extra scripted exchanges.

## D: MapPack CSV validation coverage

Exercise the public `ConvertMappackCsv` API with a hand-authored valid CSV fixture
and parameterized invalid cases. Own only the test file; do not change conversion
policy or production output.

Cover unexpected quotes in unquoted fields, text after closing quotes, duplicate
and missing required headers, extra nonempty record fields, nonfinite/zero/malformed
factors, precision outside 0–100, address overflow beyond `uint64_t`, missing
required X-axis headers, invalid axis data-header values, and invalid required
axis addresses. For rejected cases assert `kInvalidConfig` with semantic
row/column/reason fragments using Result matchers, rather than pinning full prose.

Pair rejection cases with accepted precision boundaries 0/100 and `0X` address
prefix input; inspect resulting XML format and storage-address values. Preserve
existing quoting, multiline, dimensionality, and successful-conversion fixtures.
Use named parameterized cases or `SCOPED_TRACE`. Report a discrepancy with the
documented API before proposing any production behavior change.

## E: Checksum primitive coverage

Own only the primitive test file. Add missing cases without duplicating existing
golden vectors or changing checksum dispatch, ROM layouts, or production code.

| Primitive | Additional evidence |
| --- | --- |
| `CksAdd8` | Single high-bit byte, carry followed by another addition, mixed high/low bytes |
| `NegatedSum8` | Empty input, wrapped sum exactly zero, nonzero wrapped sum |
| `Crc32` | Empty view, single zero byte, single high-bit byte, alternating bytes |
| CRC pointer overload | Non-null zero-length input and null zero-length input |
| U16/U32 big-endian rebalance | Downward adjustment at nonzero offset with surrounding bytes preserved |

Derive fixed CRC expectations independently with a bitwise oracle using the
documented ECU polynomial `0x5AA5A55A`; commit literal expected values and short
provenance comments. Do not derive expectations through production checksum
helpers. Invalid spans and out-of-bounds rebalance offsets violate preconditions
and are outside scope. Report unexpected production behavior before expanding
the task into a correction.

## Integration, verification, and closeout

Each agent runs its listed focused tests with `bazel test --config=release`.
For corrected protocol bounds/reply interpretation, perform the repository's
mutation check: temporarily restore the defect, demonstrate that the named
regression fails, and restore the fix. Apply the same check to logging run fencing.
Record actual results, including environmental blockers; an unrun check is not
passing evidence.

The coordinator then:

1. Reviews scope and combines changes without shared-file conflicts. Regenerates
   managed BUILD files with `python3 scripts/gazelle_check.py --fix` and reviews
   the output; no graph change is expected.
2. Updates the logging ownership reference, diagnostic-tools reference, and
   technical-debt roadmap. Removes resolved A/B debt, narrows C only to evidence
   still missing, and retains the broad coverage-growth goal after D/E.
3. Runs applicable full gates: `bazel build --config=release //:fastecu`,
   `bazel test --config=release //...`, `prek run --all-files`, and
   `bazel run //:clang_tidy_report_changed`. Platform CI and packaging retain
   their established gates; local tests do not establish hardware qualification.
4. Records any wire/outcome divergence from MUT corrections in the owning
   qualification records after consulting the [qualification matrix](../../flash-qualification-matrix.md)
   and applicable [checklists](../../checklists/). Existing bench status remains
   unchanged by automated evidence.
5. Delivers through a feature branch and PR. Stages named paths and pushes only
   when authorized. Preserves this spec in delivered Git history before retiring
   it after enduring knowledge is extracted, following the
   [documentation lifecycle](../../README.md#recover-completed-work-and-deleted-source-citations).

This design-document change itself requires only
`prek run lychee --all-files` and a scoped diff review; do not build the app for
specification work.
