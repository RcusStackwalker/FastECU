# CAN cancellation post-merge cleanup

Status: proposed

## Problem Statement

The merged CAN cancellation ownership stack left a small amount of contradictory
or shallow residue around an otherwise completed migration. Current-facing
documentation reports both the measured production size and the approved maximum
as `66,215`, although the final measurement is `66,214`. The closeout evidence
also preserves statements that delivery checks and merge were outstanding when
the document was written, but it does not yet distinguish that historical state
from the stack's present merged state.

The implementation has the same kind of narrow residue. Seven pre-delegation
refusal sites repeat the three-field construction that must keep cancellation
kind and I/O exposure consistent. Three family executors wrap the shared failure
reporter without adding policy. A Colt executor comment overstates what direct
callers must provide even though production guard selection belongs to the bound
flash attempt.

This is not evidence that the migration's remaining family checkpoints, concrete
family tests, guard adapters, or hardware qualification gates are redundant. A
cleanup that generalized those surfaces would risk weakening the ownership and
wire-behavior evidence the stack was built to preserve.

## Solution

Deliver two separately reviewable cleanup slices.

The first slice reconciles durable documentation. It distinguishes the measured
`66,214` production NLOC and `+3` delta from the approved `66,215` and `+4`
maximum, adds a dated post-merge delivery addendum, and updates current-facing
summaries without rewriting statements that were accurate when the closeout
evidence was recorded. The addendum records that the replacement checks for the
stack passed and that the zero-duration run attached to PR #976 was cancelled in
favor of a higher-priority replacement run rather than failing its checks.
Hardware qualification remains explicitly separate and outstanding.

The second slice deepens two existing modules without changing behavior. The
portable error module gains one named constructor for cancellation refused before
delegation. Guarded CAN, guarded mixed-CAN, and UDS request entry use that
constructor while retaining their exact detail strings. Family executors call
the shared exchange-failure reporter directly instead of routing through three
thin forwarding functions. The Colt comment states only the production
ownership rule: the bound flash attempt installs the transport guard.

No generic transport-guard framework, type erasure, new cancellation-policy
seam, or family-test parameterization is introduced.

## User Stories

1. As a maintainer, I want the final measurement and its approved ceiling to be
   clearly distinguished, so that I do not report unused budget as shipped code.
2. As a maintainer, I want current-facing documents to describe the stack as
   merged, so that I do not schedule delivery work that already completed.
3. As an evidence reviewer, I want statements that were true before delivery to
   remain identifiable as historical evidence, so that later updates do not
   rewrite the qualification record.
4. As an evidence reviewer, I want a superseded cancelled workflow run separated
   from its successful replacement, so that automation cancellation is not
   misreported as a product or test failure.
5. As a hardware qualifier, I want software delivery status kept separate from
   bench qualification, so that a green merged stack is never presented as ECU
   or adapter qualification.
6. As a flash maintainer, I want pre-delegation refusal constructed through one
   named interface, so that cancellation kind and I/O exposure cannot drift
   apart at individual call sites.
7. As a protocol maintainer, I want existing refusal detail strings preserved,
   so that cleanup does not silently alter diagnostics.
8. As a protocol maintainer, I want delegated transport results to remain
   unchanged, so that cleanup cannot alter error precedence or wire behavior.
9. As a family-executor maintainer, I want shared failure reporting called at its
   owning module, so that I do not have to trace zero-value family wrappers.
10. As a family-executor maintainer, I want family contexts retained where they
    hold family policy, so that removing wrappers does not erase meaningful
    locality.
11. As a test maintainer, I want concrete Hitachi CVT and MH8104 exposure tests
    retained, so that each executor's call-site promotion remains independently
    visible.
12. As a test maintainer, I want CAN and mixed-CAN guard suites to retain their
    distinct interfaces and lifecycle coverage, so that a generic harness does
    not hide transport-specific failures.
13. As a reviewer, I want documentation and source cleanup delivered separately,
    so that each change has a verification burden proportional to its risk.
14. As a reviewer, I want every source edit to be demonstrably
    behavior-preserving, so that the cleanup does not reopen the completed poll
    and checkpoint audit.
15. As a future contributor, I want the Colt executor comment to describe the
    production rule precisely, so that direct tests are not forced through an
    interface the durable contract does not require.
16. As the repository owner, I want unrelated untracked plans and specifications
    left untouched, so that post-merge cleanup cannot destroy workspace material
    of uncertain provenance.
17. As the repository owner, I want completed working specifications removed only
    after their enduring decisions are incorporated into owner documents and the
    originals are preserved in delivered history, so that temporary planning
    material does not become a second contract.

## Approaches

### Documentation-only correction

This would resolve the misleading measurement and delivery status while leaving
the source residue for later. It has the smallest immediate verification cost,
but leaves a repeated typed invariant and three shallow wrappers in place.

### Broad guard and test consolidation

This would introduce a generic guard implementation and shared adapter or family
test harnesses. It promises more line reduction, but the CAN and mixed-CAN
interfaces have different operation and lifecycle contracts. The concrete family
tests also prove promotion at distinct call sites. The abstraction would increase
interface complexity and reduce locality, so it is rejected.

### Two narrow cleanup slices

This is the selected approach. It delivers documentation truth alignment first,
then centralizes only the refusal invariant and removes forwarding layers that
add no policy. It produces leverage and locality without inventing a new seam or
weakening concrete evidence.

## Implementation Decisions

- Documentation reconciliation and source cleanup are separate changes. The
  documentation slice lands first.
- Durable owner documents are updated in place. The measured final production
  size is `66,214` with a `+3` delta; `66,215` and `+4` remain only the approved
  hard maximum.
- Historical passages that say checks were pending remain historical. A dated
  post-merge addendum records the later result instead of silently rewriting the
  earlier evidence.
- Current-facing summaries no longer state that submission, readiness, merge,
  or replacement platform checks are pending.
- The addendum records successful replacement checks for PRs #974, #908, and
  #975 through #979. It describes the earlier PR #976 run as superseded and
  cancelled by concurrency, not as a failed verification run.
- Hardware qualification is not changed. Recorded Colt bench work, the Hitachi
  TCU erase-scope question, and other family qualification gaps retain their
  current owners and status.
- The portable error module exposes a named constructor with one detail-string
  parameter. It always returns a cancelled error whose I/O exposure is refused
  before delegation.
- The constructor replaces the seven production aggregate constructions at the
  CAN guard, mixed-CAN guard, and UDS pre-request refusal sites. Exact detail
  strings remain byte-for-byte unchanged.
- No new error kind is added. The conservative default exposure and the existing
  may-have-started promotion helper remain unchanged.
- Three family-local exchange-failure wrappers that only forward their event
  sink and arguments are removed. Their callers invoke the existing shared
  reporter directly.
- Family context types and all family-specific control flow remain local. The
  cleanup does not move protocol policy into the shared reporter.
- The Colt executor comment is narrowed to production composition ownership. It
  does not prescribe a guarded adapter for every direct test caller.
- The CAN and mixed-CAN guards remain separate adapters at separate interfaces.
  The K-Line execution path remains unguarded by this mechanism.
- Existing concrete family exposure tests remain separate. No parameterized
  cross-family fixture or type-erased guard suite is added.
- Remaining cancellation checks, waits, post-operation checkpoints, lifecycle
  checks, validation, confirmations, and family continuation rules are not
  cleanup candidates under this specification.
- Untracked workspace plans and specifications are excluded from all edits and
  deletion decisions.

## Testing Decisions

- Documentation verification follows the repository's documentation-only gate:
  run the Markdown link checker and review the scoped diff. Do not build the
  application for the documentation slice.
- The documentation review verifies every occurrence of the final measurement,
  approved maximum, delivery status, and hardware-qualification status. A value
  search is part of the review, but owner documents remain the source of truth.
- The named refusal constructor is tested at the portable error-module interface.
  Its observable result must contain the supplied detail, cancellation kind, and
  refused-before-delegation exposure.
- Existing guarded CAN, guarded mixed-CAN, and UDS suites remain the primary
  behavioral tests for refusal without delegation, exact details, token
  forwarding, and delegated-result pass-through.
- Existing bound-attempt tests remain the production composition proof for CAN,
  mixed-CAN, and unchanged K-Line selection.
- Existing shared-reporter tests remain the owner of quiet refusal versus
  may-have-started warning behavior.
- Existing concrete Hitachi CVT and MH8104 tests continue to prove exposure
  promotion at each family call site. They are not replaced by reporter-only
  coverage.
- Removing forwarding wrappers requires focused tests for the three affected
  family executors and the shared reporter. Assertions continue to observe
  returned results and emitted events rather than wrapper presence.
- The source slice runs focused package tests during development, followed by
  the applicable release build, full release test suite, Prek hooks, convergent
  Gazelle generation, and clang-tidy before delivery.
- No hardware test is required for these behavior-preserving source edits, and
  no hardware qualification claim may be derived from their software tests.

## Out of Scope

- Changing wire bytes, ordering, timeouts, delays, retry counts, address windows,
  or family abort and continuation policy.
- Removing any additional cancellation poll or checkpoint.
- Changing operator-event policy or any existing diagnostic detail string.
- Adding an `ErrorKind`, changing default cancellation exposure, or changing
  may-have-started promotion.
- Combining CAN and mixed-CAN guard adapters or their contract suites.
- Parameterizing the concrete family exposure tests.
- Extending guarded cancellation to K-Line, lifecycle operations, configuration,
  open, close, or unblock.
- Closing hardware qualification gaps or changing the qualification matrix.
- Deleting, importing, or otherwise modifying untracked workspace artifacts.
- Broader flash-family, test-NLOC, or repository cleanup.

## Further Notes

The delivery addendum uses these authoritative workflow runs:

- PR #974: [successful replacement checks](https://github.com/RcusStackwalker/FastECU/actions/runs/41442185997)
- PR #908: [successful replacement checks](https://github.com/RcusStackwalker/FastECU/actions/runs/41452289210)
- PR #975: [successful replacement checks](https://github.com/RcusStackwalker/FastECU/actions/runs/41452289587)
- PR #976: [successful replacement checks](https://github.com/RcusStackwalker/FastECU/actions/runs/41452291404), replacing the
  [concurrency-cancelled run](https://github.com/RcusStackwalker/FastECU/actions/runs/41452290045)
- PR #977: [successful replacement checks](https://github.com/RcusStackwalker/FastECU/actions/runs/41452288353)
- PR #978: [successful replacement checks](https://github.com/RcusStackwalker/FastECU/actions/runs/41452292234)
- PR #979: [successful replacement checks](https://github.com/RcusStackwalker/FastECU/actions/runs/41452290990)

The source cleanup is intentionally small. Its purpose is not to recover the
CAN migration's accepted support cost or claim a size reduction. The deletion
test justifies removing the three forwarding wrappers because deleting them
does not move complexity to callers; the shared reporter already owns the
behavior. The same test rejects deleting the distinct guard adapters or family
tests because their contracts and verification responsibilities would reappear
as type switches or hidden test machinery.

This specification is temporary active-work material. After both slices are
delivered, enduring facts belong in the ADR, flash contract, migration results,
and current strategy document. The specification may then be removed only after
its original contents are preserved in delivered Git history.

Publishing this specification to the issue tracker and applying a
`ready-for-agent` label is a separate external action requiring explicit
authorization.
