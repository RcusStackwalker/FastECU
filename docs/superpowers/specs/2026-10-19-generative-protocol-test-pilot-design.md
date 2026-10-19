# Generative protocol test pilot

Status: approved for native execution on 2026-10-19. The
[results record](../../reference/generative-protocol-test-pilot-results.md) tracks implementation and evidence. This is a
scoped experiment, not a replacement for an accepted repository-wide decision.

## Goal and acceptance

Test whether compact, named, parameterized scenarios can reduce test code while
preserving the behavior of the N83M 1.5M and 4M flash families. The scenarios
exercise the real executors through independently constructed wire expectations.
They cover selected input classes and failure paths; they do not prove every
possible execution or establish hardware qualification.

The experiment succeeds only if all three gates pass:

1. Every existing protected behavioral obligation has an explicit replacement,
   the additional input/fault cases below pass, and targeted mutations demonstrate
   that the important assertions detect defects.
2. At least **222 net NLOC** is removed, including all new/changed support code
   and its tests. This is 20% of the two baseline executor suites' 1,107 NLOC,
   rounded up. It is a threshold, not a savings forecast.
3. Median ordinary test execution time grows by at most **25%**, measured under
   matched conditions as described below.

A result below these gates is a valid experimental finding, not permission to
weaken assertions or assume that later families will amortize the cost.

## Scope and baseline

Compare against master at `b866782bcb6295d4dde8e16eb95567bf7c855e48`.
The delivered change is test-only: test sources, test support, their generated
Bazel targets, and documentation. Production behavior and source remain
unchanged. Any production defect discovered requires separate treatment.

The two subjects are the
[N83M 1.5M executor suite](../../../src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor_test.cpp)
and the [N83M 4M executor suite](../../../src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test.cpp),
including their inherited
[CAN conformance cases](../../../src/backend/flash/ecu/testing/can_executor_conformance.h).
The scope includes their existing bench/in-car reads, write path, admission,
setup, transfer, cancellation, retry and checksum behavior. Distinctive cases
may stay family-local when parameterization would make them less clear.

Direct Lizard file measurements at the baseline are 523 and 584 NLOC,
respectively. These exclude existing shared infrastructure; the final accounting
must include its changes. Baseline test XML and runtime have not been collected
in this design session. Establish those before implementation comparisons.

This experiment is independent of [PR #756](https://github.com/RcusStackwalker/FastECU/pull/756).
At the interview's start that PR was open at
`e04546b232a5d6100b1a943f73265f310d953757`, reporting +1,088 NLOC and an outstanding
bench run. Neither its code nor its reported delta is part of this experiment.
Leave it untouched. A successful alternative earns a separate rollout decision.

Excluded: another family migration, production refactoring, guarded transports,
a new ECU emulator, a protocol interpreter, transcript files/parsers, a reference
cipher, a new fuzzing framework, and retirement of the old NLOC tooling.

## Contracts and language

The [size-reduction strategy](2026-10-18-size-reduction-strategy.md) owns the
overall objective. [ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md)
and [ADR 0025](../../adr/0025-enforce-cancellation-in-a-guarded-transport.md)
remain accepted; the documentation changes below make this two-family
experiment explicit without claiming that its results already exist.

The [sharing contracts](../../reference/flash-contracts.md) and
[Denso family differences](../../reference/denso-iso15765-families.md) own
protocol equivalence and deviations. In particular:

- 1.5M rejects absent/wrong read-setup replies that 4M tolerates. Actual transport
  failures still propagate on 4M.
- 4M discards the first bench jump reply; 1.5M does not.
- The pending checksum reread uses 500 ms on 1.5M and 3,000 ms on 4M.
- Image lengths, endpoints and literal boundary frames differ.

The [flash glossary](../../../src/backend/flash/GLOSSARY.md) remains the language
owner. No new domain term is required. Do not describe 4M's read setup as a
"tolerant probe": that glossary term aborts on absence, whereas this setup
accepts absence. Likewise, a generic scenario is not a UDS diagnostic session.

Preserve wire bytes, ordering, timing, address/data mapping, result kinds,
safety gates, cleanup and meaningful progress guarantees. Exact diagnostic
prose and incidental progress granularity need not remain asserted. Classify
each removed assertion as redundant or incidental with a reason. Preserve
meaningful operator information: the 1.5M test that prevents messages naming
4M protects family identity even if its full sentences cease to be exact matches.
Production output remains unchanged.

## Design

### Family cases and shared construction

Use ordinary C++ functions and small case records, with GoogleTest reporting a
separate named case for each family/outcome. Build on the existing
[execution helpers](../../../src/backend/flash/ecu/testing/can_executor_run.h)
and [Denso wire helpers](../../../src/backend/flash/ecu/testing/denso_can_frames.h).
The suites already share substantial code; an additional abstraction must earn
its cost within these two consumers.

Group cases by the operation they exercise, such as read setup, transfer fault,
or close/checksum outcome. Do not introduce one universal record with flags for
every stage. A record carries the inputs and expected outcome needed by that
case; family differences remain visible in literal expectations or named local
construction functions. Actual plan builders/executors are selected independently
of those expectations. Do not derive expected address windows or policies by
reading them back from the production plan.

Shared builders generate expected requests, queued replies, prescribed call
order and relevant timing from independent test inputs. The executor then runs
against that prepared script. Shared assertions check the result, data/progress,
expected operations and absence of expectation violations. Successful prefixes
may be reused; an error case describes its permitted suffix explicitly.

Select fault positions by named operation and occurrence, not a global exchange
number or cancellation-token poll count. Added fault cases normally vary one
failure; retain existing combined-fault scenarios. Log/progress-triggered
cancellation may remain where it preserves the intended boundary; changing that
trigger requires proving the new trigger reaches the same point.

### Independent payloads

Retain the existing frozen first/last complete request expectations and their
wire-framing/indexing obligations. Identical literal fixtures can be shared;
they must not become expectations calculated by production encoders.

Add two reproducible nonuniform patterns for both families' successful bench
reads, in-car reads and existing writes. Build matching plaintext/ciphertext
from literal four-byte known-answer pairs already present in the
[SSM tests](../../../src/algorithms/protocol/ssm/ssm_protocol_core_test.cpp)
and [Denso common tests](../../../src/backend/flash/ecu/denso_iso15765_can_common_test.cpp).
The payload transform operates independently on each four-byte word, so this
requires no reference cipher. The existing `ToWire` helper calls production
crypto and cannot be the independent oracle for these new patterns.

Encode the absolute page identity in the selection of known-answer words;
use two distinct deterministic arrangements. The fixture must distinguish
adjacent pages, first/middle/last pages, and the nonzero image-base offset
throughout each supported image. Avoid a pattern that simply repeats every
page. Verify those distinguishing properties and literal sample positions in
the fixture's own tests. Select plaintext and ciphertext from the same committed
pair dictionary, never by invoking the algorithm being checked.

Use one discriminating pattern for added fault cases rather than crossing every
fault with every pattern. These fixtures strengthen mapping/orchestration tests
but do not establish arbitrary-input cipher correctness. Preserve the existing
algorithm tests and known-answer vectors as independent evidence dependencies.

### Fault and outcome matrix

Preserve all obligations in the inventory below and add the following bounded
coverage. Assertions apply to the actual family executors, including their
propagation and policy selection.

| Area | Required distinctions |
| --- | --- |
| Each read-setup site, `0x34` and `0x35` | Success, absent frame, malformed/mismatched reply, actual transport failure; strict 1.5M versus tolerant 4M behavior |
| Reply shape | No frame, empty PDU within a valid CAN envelope, and invalid/truncated raw frame remain distinct where behavior differs |
| Read/write loops | First, middle and last iteration failures using representative errors/cancellation; no full error-by-position-by-payload product |
| Family choices | Bench first-reply handling, in-car sequence, checksum timeout, memory origin, padding and result/operation shape |
| Recovery and termination | Immediate abort, bounded retry, tolerated continuation or required cleanup according to the case's contract |

For added loop cases use a transport failure at the first iteration, cancellation
at a middle iteration and a rejected/malformed reply at the last iteration,
where meaningful to the operation; existing cases cover additional combinations.
Use the independent expected transfer length to select the middle and last
iterations. A cancellation trigger must prove that the selected iteration was
reached. Document a different representative if the protocol cannot express a
listed reply class; do not silently omit the boundary.

Shared-layer tests retain responsibility for algorithmic retry limits and
response decoding. Family tests retain responsibility for selecting those
policies and honoring their results. Existing multi-fault close/retry paths
remain represented. Do not impose a universal stop-on-any-error rule: this
dialect deliberately swallows some stop/close and identity-query errors.

### Scripted transport observations

Make a minimal additive extension to the package-owned
[scripted CAN transport](../../../src/backend/flash/testing/scripted_can_flash_transport.h).
Preserve its scripting API and the existing queue-consumption meaning of
`ScriptConsumed()`. Existing tests intentionally send wrong writes before a
correct one and then assert consumption; their meaning remains valid.

Expose separately:

- Persistent evidence of an unexpected operation, set where the fake detects
  mismatched bytes or exhausted expectations. Retaining the first diagnostic
  is sufficient; later successful calls cannot erase it.
- A compact ordered record of attempted writes/reads, including attempts that
  return cancellation or an error. Avoid duplicating full payload buffers.

A deliberately queued error is not an expectation violation, even if its kind
is `Internal`. Record attempts before early returns without adding token polls
or changing return values. Expected order is constructed by the test case,
never learned from the executor. Recording alone is insufficient: the pilot
assertions must compare the order and require no unexpected operations.

For failures, construct the expected script prefix and permitted suffix rather
than requiring consumption of an unexecuted happy-path tail. Distinguish an
attempt rejected by cancellation from an accepted wire operation. Keep timeout,
delay and lifecycle assertions. Where a delay's placement relative to I/O is
contractual, assert that placement as well as its duration with the recording
clock and operation observations.

Support self-tests must detect mismatched-write-then-recovery, extra swallowed
writes/reads and wrong interleaving, while accepting intentional injected
errors. They must observe cancelled attempts. Keep strict order assertions
local to the pilot; old users may enqueue separate read/write queues without
implying global order. Subclasses that bypass the base methods are not covered
by its observations; the pilot must not bypass them or claim universal coverage.
No K-Line/mixed-CAN redesign is included. Run all affected consumers.

## Baseline obligation inventory

This inventory maps the existing test declarations into migration groups.
Names shared by both families appear once; `C` marks inherited conformance.
Before removing a case, the implementation records its replacement case and
assertions, or the reason an incidental assertion is removed under the contract
above. Existing parameter rows and branches inside tests are obligations too.

| Group | Existing cases | Protected behavior |
| --- | --- | --- |
| Admission | C: `RejectsAPlanFromAnotherFamilyBeforeAnyIo`, `RefusesATestWritePlanRatherThanWritingForReal` | Reject before I/O/configuration |
| Configuration | C: `TransportSetupReturnsThePlansWireParameters`, `ConnectProbesReadWithThisFamilysProbeTimeout` | Wire settings and probe timing/count |
| Read success | `BenchReadReturnsPaddedImage`, `InCarReadReturnsPaddedImage` | Both paths, complete image, content/padding, seed/key timing |
| Operator identity | 1.5M: `OperatorFacingLogLinesNameThe1_5MFamily` | Correct family identity; exact surrounding prose is incidental |
| Write success | `WriteErasesThenFlashesBlockOne` | Erase-before-write, selected block, complete bytes/frozen boundaries, base offset, result and delays |
| OBK probe | `ObkMismatchAbsentAndEmptyAreMisses`, `ObkErrorsAndCancellationPropagateBeforeInitialization` | Miss/error distinction and branch selection |
| Identity/selector | `IdentityExchangeFailuresIncludingCancellationAreNonFatal`, `EmptyBranchSelectorReplyFails` | Tolerated identity errors, fatal absent selector, timing |
| Read setup | 1.5M: `NegativeResponseAtDumpSetupFails`; 4M: `ProceedsPastMalformedConnectAndDumpSetupResponses`, `ProceedsPastMalformedInCarProbeAndDumpSetup`, `ReadTimeoutAtTheTolerantDumpSetupStillPropagates` | Strict/tolerant setup, combined malformed paths, actual transport errors |
| Fatal connect/read | `NegativeResponseDuringConnectFails`; C: `ReadPropagatesADisconnectedTransport`, `ReadReportsAnEmptyReplyAsTimeout`, `ReadTimeoutPropagates` | Seed rejection and error propagation at each family's chosen fatal site |
| Dump failure | `ReadDisconnectMidDumpLoopPropagates`, `PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` | Page failure, no later stop/progress; the existing named "MidDumpLoop" case actually fails at the first page |
| Cancellation | `CancellationAfterPageFailsButCancellationAtStopIsSwallowed`; C: `ReadStopsAtTheNextChunkWhenCancelledMidRead` | Partial-read termination versus best-effort-stop tolerance |
| Short pages | `ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` | Partial decryption, requested-size address/progress advance, padding and bounded stop attempts |
| Kernel jump | `BenchJumpExhaustionPreservesReadCountAndTiming`, `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` | Discarded first reply, waits/timeouts, exhaustion, unchecked final receive, cancellation and existing acknowledgment variants |
| Erase exhaustion | `EraseRetryExhaustionFails` | Twenty receive-only attempts, no trigger resend or write entry |
| Close/checksum | `CancellationAtCloseSettlePreventsChecksum`, parameterized `CloseRetriesSettleAndChecksumOutcomes` | Six mixed-fault close attempts, settle cancellation, five checksum outcomes and family timing |

Static inspection finds 19 local declarations in 1.5M, 20 in 4M, and eight
conformance declarations per family. Each checksum test expands to five cases.
This suggests 63 executed baseline cases; confirm with test XML. Case count
is a useful completeness cross-check, not an acceptance ratchet.

## Verification and measurement

### Behavioral evidence

Run the baseline before restructuring. Add support self-tests and replacement
cases before removing predecessors; migrate 1.5M, then 4M. Preserve the
independent plan and algorithm suites. Report any baseline failures separately;
do not describe an incomplete comparison as preservation evidence.

Use temporary, deliberate defects in the affected executor/common code to
prove the assertions, even though production is unchanged in the delivered diff:

| Deliberate defect | Required detector |
| --- | --- |
| Change strict setup to continue, or tolerant setup to abort | Named family/setup outcome case |
| Ignore a fatal exchange result | Family propagation case and forbidden subsequent-I/O assertion |
| Add an unexpected operation whose error is swallowed | Persistent violation/order assertion |
| Change image-base indexing or reorder/omit payload | Independent patterned transfer/content case |
| Truncate the final read/write transfer | Endpoint, complete content and operation assertions |
| Change bench first-reply handling | Family jump order/read-count/timing case |
| Change pending checksum timeout | Family checksum timing case |
| Move/remove a cancellation boundary | Boundary case with proof that injection occurred |

Tie each to a named failing assertion; a harness crash alone is not sufficient.
Compare baseline and candidate detection on these obligations and explain
pre-existing gaps versus lost detection. Restore all mutations before normal
verification and measurement. Explain unreachable/equivalent mutations rather
than introducing production suppressions; the redundant post-read TestWrite
guard is not independently reachable after admission rejects that operation.
Ordinary changed-line mutation CI cannot establish this test-only comparison.

Focused tests include the two executor suites, their plan suites,
`//src/backend/flash/ecu:denso_iso15765_can_common_test`,
`//src/algorithms/protocol/ssm:ssm_protocol_core_test`,
`//src/backend/flash/testing:scripted_flash_transports_test`, and new support tests.
Find additional consumers from the Bazel graph, not a copied permanent list:

```sh
bazel query 'kind(".*_test rule", rdeps(//..., //src/backend/flash/testing:scripted_flash_transports))'
bazel query 'kind(".*_test rule", rdeps(//..., //src/backend/flash/ecu/testing:can_executor_conformance))'
```

Before delivery run the applicable repository gates: release build/tests,
formatting, Gazelle regeneration/review, changed-code static analysis and
platform CI/packaging. Hardware status remains unchanged. Documentation-only
changes to this design use link checks and scoped diff review, not an app build.

### Size and runtime

Measure NLOC on the formatted tree using the repository's Lizard definition.
Report baseline/candidate scoped totals and the whole-tree delta. Include all
new or changed support and support tests, even outside the two suite files.
Relocation within counted files saves nothing. Excluded data/generated code and
unrelated cleanup cannot be used to make the pilot pass. Production delta must
be zero; repository-wide background changes are excluded by the pinned baseline.

For runtime compare the two executors plus affected support self-test targets
on the same machine, toolchain, release configuration and scheduling settings.
Include existing support targets on both sides and genuinely new targets only
on the candidate. Use five measured runs after warm-up with Bazel test-result
caching bypassed. Compare the median summed test execution durations; report
build time and mutation verification separately. Retain immutable computed
fixtures across cases where appropriate, but give each run fresh transport,
cancellation and observer state. Do not allow case order to affect results.

If upstream changes require rebasing, first verify that the measured source
baseline is still comparable; report the rebase and repeat both sides where
it changes the experiment. Do not silently substitute a moving master revision.

## Documentation and disposition

Record the two-family experiment in ADR 0024 and the size-reduction strategy,
linking to this design and later to its results. Keep the general transcript
policy and PR #756 intact. No new permanent ADR is required for this reversible
experiment; broader policy replacement requires a separate decision.

The coding guide currently calls log/error text observable behavior, while the
strategy permits non-wire wording normalization. Clarify that semantic operator
information remains protected while exact narration need not be frozen. The
[tooling guide](../../tooling.md#nloc-hill-climbing-loop) still describes the old
loop despite ADR 0023's supersession. Add a scoped pointer to this experiment's
checks instead of applying obsolete case-count guards. Tool retirement remains
separate work. These scoped owner-document changes belong with the approved
pilot, not an unannounced global policy rewrite during this design interview.

If any gate fails, record the unmet criterion, retained/added coverage, net
size, runtime and limitations. Do not expand scope, delete obligations or claim
future savings to obtain a pass. A smaller useful cleanup can be reviewed
separately. Success supports a separate rollout proposal; it does not itself
authorize another family migration or establish a path to 100k NLOC.

After the pilot, extract enduring decisions and evidence into their owning
references and qualification records as applicable. Retire this completed
spec only after its original contents are preserved in delivered Git history,
following the documentation index's lifecycle rule.

## Decision record

The user accepted all twelve interview recommendations:

| Decision | Settled choice |
| --- | --- |
| Q1 | Independent pinned-master baseline; leave #756 untouched |
| Q2 | Test-only implementation |
| Q3 | Preserve mapped behavioral obligations; allow parameterization |
| Q4 | N83M 1.5M and 4M |
| Q5 | Protect meaningful behavior; classify incidental narration assertions |
| Q6 | Compact named scenarios using existing scripts; no emulator |
| Q7 | At least 222 net NLOC removed including support |
| Q8 | At most 25% ordinary runtime growth |
| Q9 | Two independent deterministic payload patterns from literal pairs |
| Q10 | Bounded fault matrix with forbidden-I/O evidence |
| Q11 | Additive observations in existing CAN test transport |
| Q12 | Scoped experiment under existing policy; separate rollout decision |

The design frontier is closed. The user approved native execution of the
[implementation plan](../plans/2026-10-19-generative-protocol-test-pilot.md).
The original design and this plan remain in delivered history before retirement.
