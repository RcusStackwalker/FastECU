# CAN Cancellation Stack Cleanup Design

## Intent

Turn PR #908 from a self-authorizing, net-positive cancellation rollout into
one layer of an atomically merged stack whose decision, safety migration and
cleanup are independently reviewable. The delivered stack must centralize
cancellation ownership while leaving the repository smaller in both production
and test NLOC than the `master` baseline from which the stack is rebuilt.

## Goals

- Preserve PR #908's identity and its conservative per-family audit evidence.
- Review the durable ADR decision separately from the implementation rollout.
- Select CAN cancellation guards once at the execution boundary by transport
  interface, not through permanent per-executor aliases.
- Move generic next-I/O cancellation refusal and its tests to the guard owner.
- Remove redundant executor polls and family tests without changing observable
  family behavior.
- Merge the complete stack atomically through `gh stack merge --squash`.

## Non-goals

- Normalize error kinds, error precedence, retries, logging, progress, event
  ordering, confirmations, waits or protocol behavior across families.
- Change K-Line cancellation ownership or remove `UdsClient` checks.
- Claim hardware qualification or make unavailable bench evidence a delivery
  prerequisite.
- Obtain savings by moving first-party code outside measured scope or by
  deleting unrelated code.

## Failure conditions

The stack must not merge if production NLOC or test NLOC is at or above the
pre-stack `master` baseline. It also must not merge if a cleanup changes wire
bytes, ordering, timeouts, delays, retries, returned errors, progress, event
ordering or a family-owned cancellation checkpoint. If safe cleanup cannot
meet the NLOC gate, the ADR decision is reconsidered rather than broadening the
stack or weakening the metric.

## Approaches considered

### Chosen: decision, conservative rollout and cleanup as one atomic stack

Build a clean linear stack from `origin/master`: a standalone ADR PR, rewritten
#908, structural binding cleanup, test compression, redundant poll removal and
final evidence/plan closure. Open all members as drafts, review bottom-up and
merge all members atomically only when the top is fully verified.

This preserves the auditability of #908 while making the transitional cost
visible and removable in later, separately reviewable layers.

### Rejected: fold all cleanup into #908

This could produce a smaller final diff, but would mix the decision, safety
migration and deletion proof into one already large PR. Reviewers could not
approve the conservative behavior baseline independently of the more invasive
poll and test deletion.

### Rejected: replace #908 with new PRs

Closing #908 and recreating its work would avoid a force-update, but would lose
its stable PR identity and fragment its existing review discussion. Rewriting
the draft branch retains that context while presenting a clean stack.

## Stack architecture

Construct the stack in an isolated worktree rooted at the current
`origin/master` commit (`c078e458` at design time). That trunk commit is the
hard NLOC and behavior baseline. If the stack is rebased onto a newer
`origin/master`, record and measure the new trunk before continuing; the hard
gate follows the rebased trunk, while the final evidence also retains the
comparison to `c078e458` for continuity with #908. Preserve unrelated untracked
files in the existing checkout.

1. **ADR decision.** Update ADR 0025 in its own PR rather than creating a
   superseding ADR. Define the durable end state: guards are selected by
   transport interface at the execution boundary; generic refusal is tested at
   its owner; per-family audits are migration evidence rather than permanent
   aliases; completion requires removing redundant ownership and achieving a
   net-negative production and test NLOC result.
2. **Rewritten #908.** Retain the audited conservative CAN and mixed-CAN
   rollout, guard implementation, preservation evidence and family-specific
   regressions. Remove self-authorizing ADR/strategy changes and make no savings
   claim. Retarget #908 onto the ADR branch and force-update its existing branch.
3. **Central guard binding.** Add non-template production execution overloads,
   declared alongside the executor interfaces and implemented in a source file
   that includes both guard definitions. The CAN overload constructs
   `GuardedCanFlashTransport`, the mixed-CAN overload constructs
   `GuardedMixedCanFlashTransport`, and the K-Line overload delegates
   undecorated. `BoundAttempt` calls this seam, avoiding a header cycle while
   mapping behavior by transport interface. Delete executor aliases, Denso
   policy specializations, synthetic per-family selection tests, their fixture
   framework and the duplicated test-only dispatch adapter. Prove the
   production path once per guarded interface.
4. **Generic test compression.** Delete or consolidate family cases that prove
   only next-I/O refusal. Retain real-family cases for error precedence,
   tolerated failures, post-operation checkpoints and forbidden subsequent
   family work. Use typed conformance cases only when they invoke real executors
   and describe a genuinely shared contract. Ordinary family-unit tests call
   the executor's undecorated `Execute` to test family logic; only retained
   composition/cancellation cases use `BoundAttempt`. Generic refusal remains
   at the guard owner.
5. **Redundant poll removal.** Remove only checks equivalent to guard-owned
   refusal immediately before the next I/O, together with tests whose sole rule
   has moved to the guard. Retain post-operation polls, cancellable waits,
   confirmations, retries, error-precedence checks and non-I/O boundaries. Work
   by shared protocol seam first, then remaining family-specific seams.
6. **Evidence and closure.** Replace the migration task diary with a compact
   matrix of executor, retained checkpoints, removed polls, surviving test
   owner, NLOC delta and qualification gap. Reconcile current contracts and
   strategy. Delete the completed plan only in this later layer so its contents
   remain in delivered history after squash merging the individual PRs.

## Deletion evidence

Each cleanup PR records, in its description:

- the removed poll or test;
- the surviving owner of the same rule;
- the family-specific behavior that remains covered;
- the focused test or mutation that distinguishes the retained owner; and
- production and test NLOC deltas against its parent and against the original
  `master` baseline.

The final repository retains only the durable compact matrix, not intermediate
command transcripts or repeated status prose.

## Verification and review

Every layer runs focused Bazel tests, convergent Gazelle generation, applicable
format/static checks, scoped diff review and an NLOC measurement. Normal PR CI
must pass for every stack member.

At the top of the stack, run the release application build, full release test
suite, all `prek` hooks, full clang-tidy, coverage, mutation checks for changed
cancellation lines and final NLOC comparison. The hard size gate is that both
production and test NLOC are below the original `master` baseline; the review
gate is that cleanup recovers more than #908's measured additions of 157
production and 1,640 test NLOC.

Open the complete stack as drafts and review it bottom-up. Propagate lower-layer
changes upward with `gh stack rebase` and `gh stack submit`. Mark every member
ready only when the reconstructed stack is stable and green. Merge the whole
stack atomically with squash merge so every PR becomes a distinct durable
commit; if any member cannot merge, none should merge.
