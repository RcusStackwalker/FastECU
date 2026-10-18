# Deduplication approaches toward 100k NLOC

Status: research and proposed experiments, not implementation authorization.
Baseline: `aebdf061`, inspected 2026-10-18. The three pre-existing untracked
specs were left untouched. This report concerns production and test C++;
the metric does not represent every maintained line in the repository.

## Finding

Exact-copy extraction alone does not support a reduction to 100k NLOC.
The largest opportunities appear to be compact test scenarios, shared
protocol operations within proven dialects, and smaller serial interfaces.
Reaching 100k remains an unproven stretch target, not an estimated delivery
outcome. No implementation experiment or behavioral qualification was performed.

The governing decisions remain [ADR 0023](../../adr/0023-hill-climb-nloc-with-kind-specific-guards.md),
the [flash sharing contract](../../reference/flash-contracts.md), and the
[coding and testing conventions](../../coding-style.md). This proposal does
not supersede them.

## Measured baseline

Lizard 1.24.1, using the configured NLOC-hook arguments and the classification
in [nloc_loop.py](../../../scripts/nloc_loop.py), measured the current checkout:

| Area | Production NLOC | Test NLOC | Total |
| --- | ---: | ---: | ---: |
| Backend flash | 23,433 | 36,017 | 59,450 |
| Desktop UI | 12,444 | 10,552 | 22,996 |
| Desktop platform | 10,515 | 7,750 | 18,265 |
| Everything else | 24,845 | 25,137 | 49,982 |
| Total | 71,237 | 79,456 | 150,693 |

The gap is 50,693 NLOC, or 33.6%. An illustrative 55k-production/45k-test
allocation would require removing 16,237 production NLOC and 34,456 test
NLOC: approximately 23% and 43%, respectively. That allocation is arithmetic,
not evidence that either reduction is feasible.

The pinned jscpd 5.4.0 reports:

| Scan | Clone pairs | Reported duplicated physical lines |
| --- | ---: | ---: |
| Repository configuration | 244 | 4,135 |
| Ignore identifiers, literals, and comments | 1,183 | 29,650 |

These figures are not removable NLOC. Clone intervals overlap, meaningful
protocol differences disappear under normalization, includes and declarations
also match, and any replacement implementation has a cost. The configured scan
excludes the DTC and built-in catalog tables; Lizard includes them. Neither
scan proves a ceiling or a savings budget for semantic refactoring.

### Reproduction

Run from the baseline checkout, with the pinned tools available:

```sh
python3 scripts/nloc_loop.py measure
prek run nloc --hook-stage manual --all-files
prek run jscpd --hook-stage manual --all-files
```

For the exploratory scan, invoke the hook environment's jscpd 5.4.0 executable:

```sh
jscpd --config .jscpd.json --mode weak --ignore-identifiers --ignore-literals \
  --output /tmp/fastecu-dedup-renamed
```

Group Lizard's file rows by the three directory prefixes in the table and
apply `nloc_loop.classify`; all remaining in-scope files form the final group.
The initial inspection ran cached pinned executables directly because the
sandbox could not write prek's cache log. It used the NLOC hook's arguments
without changing source formatting. Future before/after claims must use the
formatted-tree metric required by ADR 0023. Regenerate reports for a new base;
this dated snapshot is not a live inventory.

## Approaches

### 1. Named test scenarios and reusable execution harnesses

There are 31 `*_executor_test.cpp` files containing 22,634 NLOC, excluding
shared test support. Cases repeatedly construct plans, transport scripts,
clocks, cancellation tokens, event sinks, and execution results.

[ExecutorRun](../../../src/backend/flash/ecu/testing/can_executor_run.h) and
[CanExecutorConformance](../../../src/backend/flash/ecu/testing/can_executor_conformance.h)
already share some of this work. Extend that pattern into larger BEEF,
K-Line, and desktop transport suites where the semantics match. A case can
name its fault location, reply, expected outcome, and allowed subsequent I/O;
a package-owned harness supplies common execution and checks.

Keep expensive cases as separately named GoogleTest parameters so filtering,
sharding, mutation fail-fast, and failure diagnostics remain useful. Preserve
family-local wire expectations independently of production encoders. A shared
fixture must not make unrelated cases pass by deriving their expected bytes
from the implementation under test.

Audit existing oracle dependencies before reusing a harness. For example,
`ScriptShortPageDumpAndStopRetries` in the cited support header calls
[ToWire](../../../src/backend/flash/ecu/testing/denso_can_frames.h), which uses
production `ssm_protocol::CalculatePayload`, as do
[DensoEncryptRom and DensoDecryptPage](../../../src/backend/flash/ecu/denso_iso15765_can_common.h).
This can support orchestration tests but is not independent proof of exact
encrypted bytes, even with separately written tables. Preserve independently
derived fixed vectors separately and record which contract each test proves.
This observation does not establish that existing algorithm coverage is absent.

Savings are unestimated. Source-pool size does not identify the removable
subset. Test reductions here overlap approaches 3 and 4; any future candidate
ledger must allocate each changed file and shared helper only once.

### 2. Complete operations shared within proven protocol dialects

Extracting a complete operation can remove repeated control flow that small
helpers leave in callers: transfer loops, progress, result propagation, and
cleanup. [Denso BEEF CAN common operations](../../../src/backend/flash/ecu/denso_beef_can_common.h)
already share block comparison, reading, flashing, and write orchestration.
They also already share `BeefKernelPayload` construction and the
`UploadBeefKernelBlocks` B6 transfer loop, both used by the
[TCU executor](../../../src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp).
Subtract these existing extractions before proposing more kernel sharing.
Remaining upload orchestration includes family-specific startup, response
handling, restart, and delay. No concrete remaining production operation has
yet been established as equivalent: first name one and construct a family-by-family
matrix of bytes, checks, sequencing, and failure behavior before selecting it.

The four N83M/SH72531/SH72543 ISO-15765 executor implementation files now total
only 1,437 NLOC; much sharing has already happened. They cannot supply several
thousand more production lines. Keep family sequencing, address windows,
retry counts, response tolerance, and timing explicit. The
[sharing decision](../../design-notes.md#share-only-proven-protocol-equivalence)
rejects a universal configurable state machine inferred from similar code.

Savings are unestimated. This approach needs its own representative production
experiment; test-harness compression cannot establish a production savings
ratio. Additional test savings belong to approach 1 rather than a second
savings allocation.

### 3. Reduce the serial interface surface

The [remote serial implementation](../../../src/platform/desktop/common/serial/remote/remote_serial_backend.cpp)
and [facade](../../../src/platform/desktop/common/serial/facade/serial_port_actions.cpp)
each mark a section of 44 configuration getter/setter pairs. Their declarations
and implementations recur in interfaces, direct backends, and test doubles.
The common serial subtree totals 4,117 production and 1,761 test NLOC.

Investigate typed values for cohesive connection, framing, and adapter
configuration, with fewer operations to apply or inspect them. Immediate
actions such as control-line changes must retain explicit semantics. A smaller
interface can remove forwarding and fake-method repetition at its source.
Preserve the existing remote contract, execution context, lifetime rules, and
ordering; replacing many calls with a batch is not automatically equivalent.

There are concrete reasons to characterize before redesigning:

- [SetSerialPortParity](../../../src/platform/desktop/common/serial/direct/serial_port_actions_direct.h)
  stores the requested value, then calls `QSerialPort::setParity` when the port
  is open. A false return can therefore leave stored state changed. A batch
  cannot silently add rollback or atomicity.
- Remote setters use synchronous calls to the
  [existing remote slots](../../../src/platform/desktop/common/serial/serial_port_actions.rep).
  Preserve their order, per-call failure behavior, and interface compatibility.
  Removing local methods does not authorize removing deployed remote slots.
- Remote `SetKlineTimings` is deliberately a successful no-op. Sharing direct
  and remote configuration must not make it start sending a command.
- The [remote smoke test](../../../src/platform/desktop/common/serial/remote/remote_backend_smoke_test.cpp)
  explicitly lacks call-level remote coverage. Its construction/teardown check
  does not establish equivalence of a configuration redesign.

An implementation/comment disagreement was found during review:
[K-Line Configure](../../../src/platform/desktop/common/transport/desktop_kline_flash_transport.cpp)
says all its setters merely assign and always succeed, but its parity setter
can invoke the open port and fail. Preserve the implementation's observable
behavior; resolving the stale comment is separate from this research change.

Savings are unestimated. First classify each candidate as stored state,
immediate action, remote call, or intentional no-op; characterize closed/open
ports, partial failure, and supported peer-version combinations. Compatibility
adapters and new tests count against any saving. A migration that only adds
another layer does not meet the deduplication objective.

### 4. Test behavior through its owning interface

[mainwindow_test.cpp](../../../src/ui/desktop/widgets/mainwindow_test.cpp)
contains 4,725 NLOC. It repeatedly constructs services, drives modals, opens
ROMs, and inspects application state. Existing `WindowUnderTest` setup can be
used more consistently. Some `checkX()` methods and their one-call `TEST_F`
wrappers may also be collapsible after checking access and fatal-assertion
behavior.

Where a coordinator owns a rule, put the full behavioral matrix through that
interface and retain window tests for wiring, visible output, signals, and
lifetime interactions. Moving or splitting files alone saves nothing. Deleting
an integration test requires showing that its distinctive integration assertion
survives elsewhere, not merely that the underlying rule has a unit test.

Savings are unestimated and overlap approach 1. Deliberate merging of test cases
cannot be a test-count-preserving NLOC-loop change; ADR 0023 requires a normal PR for that
case. This report does not propose weakening the loop guards.

### 5. Typed data for repeated declarations

[builtin_catalog.cpp](../../../src/backend/config/builtin_catalog.cpp) has
1,646 NLOC, including repeated protocol properties. Named constructors or
explicit defaults may remove repetitive fields while leaving differences
readable. Family metadata and configuration bindings are other candidates.
Prefer typed C++ tables before a new generator. If generation becomes justified,
its maintained inputs, implementation, and tests must enter the cost assessment.

Keep independent safety-related agreement tests, including geometry checks;
generating both values and their expectations from one source conceals mistakes.
The 3,781 NLOC in
[dtc_tables.cpp](../../../src/algorithms/diagnostics/dtc_tables.cpp) are mostly
domain data. Moving those rows to another format is not a corresponding
maintenance saving and should not fund the 100k goal.

Savings are unestimated; no measured replacement exists. A metadata experiment
must measure its own net cost rather than inherit results from the test pilots.

### 6. Surgical extraction as supporting work

The 25 entries in the baseline [NLOC trend](../../reference/nloc-trend.csv)
record a combined reduction of 2,189 NLOC across 25 PRs. This is not the difference
between the first and last snapshots, which also include intervening changes. It illustrates
the scale of existing small extractions, not a forecast of future throughput.

Continue low-risk extraction within larger changes, but the remaining exact
clone report does not support a 50k reduction by this approach alone.

## Proposed experiments and decision gates

These are proposed pilots, not approved implementation tasks:

1. Investigate the `RestartResetConfigureAndOpenFailuresPropagateAndOuterCloseStillRuns`
   and `CancellationAfterKernelStartAndWithinRestartStopsAtTheNextBoundaryAndCloses`
   cases in the
   [TCU Denso executor suite](../../../src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp)
   as named scenarios. They already use table-driven loops, so parameterization
   alone may increase NLOC. Measure whether shared setup pays for the new cases.
   Keep the local recording transport and `BindFlashAttempt`, outer-close
   assertions, exact lifecycle order, and forbidden restart delay. Bare
   `ExecutorRun` would omit the attempt owner's cleanup behavior.
2. Start with the shared read exception/cancellation cases in the
   [CAN transport suite](../../../src/platform/desktop/common/transport/desktop_can_flash_transport_test.cpp)
   and [K-Line transport suite](../../../src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp).
   Compare standard/nonstandard backend throws with cancellation absent or
   observed during the call, plus cancellation after a successful read. Retain
   each adapter's invocation and exact outcome expectations. Exclude write
   consolidation: CAN write accepts a cancellation token and returns status;
   K-Line write has no token and returns a byte count. Keep existing ownership
   and lifetime cases outside this initial experiment.
3. Investigate one cohesive serial configuration group through callers, remote
   slots, direct implementations, and fakes. First record effects, call ordering,
   partial-failure state, and compatibility constraints. Prototype a smaller
   interface only after those observations are covered and compatibility permits
   existing maintained methods to be removed. Do not change the remote schema
   based solely on a local C++ interface reduction.

For each pilot, record the before/after formatted NLOC including every new
helper and test, the cases and assertions retained, runtime/sharding impact,
and the actual validation evidence. Stop expanding an abstraction when its
parameters reproduce the complexity it was intended to hide.

Map every old scenario, including each loop row, to its new named case and
preserved assertions. An unchanged or increased GoogleTest count can still
hide a lost loop row. This mapping covers byte expectations, error detail,
negative assertions (I/O or sleeps forbidden), ordering, and cleanup, not just
successful execution of the same production lines. Preserve fatal-assertion
boundaries in every harness: plan helpers return `Result`, callers assert
success before dereferencing, and callers must stop when a void assertion helper
has a fatal failure, for example with `ASSERT_NO_FATAL_FAILURE`. Do not allow a
failed setup helper to return into execution with invalid state.

Apply the existing kind-specific ADR 0023 guards to loop changes: test-only
changes preserve covered-line and killed-mutant sets and executed case count;
production changes use the changed-line coverage and mutation guards and keep
case count. Mull evidence requires Linux CI and does not cover UI/platform;
coverage and test count alone do not prove behavior preservation there.
Any deliberate test retirement needs a reviewed mapping of its obligations.

Protocol refactors additionally compare bytes, ordering, timing, retries,
cancellation, error precedence, image output, and cleanup. Preserve address
guards and consult the [qualification matrix](../../flash-qualification-matrix.md)
and family checklists. Characterization equivalence does not establish hardware
qualification. Do not change backend portability or Bazel layer visibility.

The initial speculative savings ranges, including the conversational 20–35k
aggregate, are withdrawn. No measured compression ratio or candidate allocation
supported them; the aggregate also failed to reconcile with its component
ranges. There is currently no repository-wide savings estimate.

Each pilot can establish savings only within its measured scope. For any later
forecast, maintain a candidate ledger with exact files/cases, observed repeated
behavior, proposed replacement, removed NLOC, added NLOC, net NLOC, overlapping
ownership, and validation evidence. Count a shared helper once, including its
tests and maintained data. Keep unpiloted approaches unestimated rather than
extrapolating test results to flash production code or metadata.

The full 50,693-NLOC gap remains unallocated. A 100k result is neither demonstrated
feasible nor demonstrated impossible. Do not fill that gap with weakened tests,
moved data, or formatting tricks.

## Adversarial review

The measurement/feasibility reviewer independently reproduced the baseline and
source-pool counts from the retained scan artifacts. Accepted findings:

- Withdraw unsupported per-approach ranges and the inconsistent aggregate.
- Do not extrapolate the three proposed pilots to unpiloted production-flash
  and metadata approaches; require separately measured candidates.
- Correct historical savings to 2,189 total across 25 PRs.

The independent architecture/behavioral-preservation reviewer identified four
material issues, all accepted and checked against the cited source:

- Existing test helpers share an encryption primitive with production. Add an
  oracle-dependency audit and separate orchestration evidence from independent
  byte-vector evidence.
- Serial configuration includes device effects, partial updates on failure,
  synchronous remote calls, and intentional no-ops. Make the serial pilot
  characterization first and preserve the remote interface.
- Kernel payload construction and B6 transfer loops are already shared. Remove
  the implication that those existing extractions are future savings; require a
  named remaining operation and an equivalence matrix.
- Broad pilot descriptions could lose attempt-owner cleanup or normalize
  distinct write interfaces. Name narrower scenarios and require loop-row,
  assertion, ownership, and fatal-failure mappings for every test migration.

No review finding was rejected. The reviewers assessed source and reasoning;
neither review establishes net savings, implementation correctness after a
future refactor, or hardware qualification. The feasibility of 100k remains open.
Both reviewers re-read the revised document and reported no remaining material
issues within their respective review scopes.
