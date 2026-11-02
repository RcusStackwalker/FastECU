# CAN Cancellation Ownership with Normalized Logging

## Intent

Complete the guarded CAN cancellation rollout from PR #908 while making the
distinction between a refused operation and a possibly accepted operation
explicit. The delivered stack keeps conservative family evidence, removes the
pre-I/O polls made redundant by the guard or `UdsClient`, and accepts a bounded
production and test NLOC increase in exchange for centralized ownership and
accurate operator logging.

This design revises the size failure condition in the
2026-10-31 CAN cancellation cleanup design. It does not authorize implementation
until its follow-on plan is approved.

## Why the previous completion gate failed

The stack rebuilt on trunk `e62684af59f1bd3c98b20596ee0503327eea8a94`
measured 66,405 production and 88,783 test NLOC before the rollout. After the
ADR, reconstructed #908, interface-level binding, test compression and the only
behavior-equivalent poll deletion, it still measured 66,522 production and
89,699 test NLOC: `+117` production and `+916` test NLOC.

The remaining pre-I/O polls differed from the lower owner in cancellation detail
or operator logging. In particular, the shared failure reporter could describe a
request as possibly running on the ECU even when `UdsClient` or a guarded
transport had refused it before delegation. Preserving that misleading behavior
prevented otherwise valid deletion; treating every cancellation as quiet would
remove a safety warning after an accepted request.

A disposable two-commit spike resolved the ambiguity with typed cancellation
exposure, removed 33 redundant poll sites, and passed all 88 flash targets. It
finished at 66,409 production and 89,884 test NLOC: `+4` production and `+1,101`
test NLOC against the pinned trunk. The spike proved the behavior and bounded
cost, but its commits are research evidence rather than delivery commits.

## Goals

- Select CAN and mixed-CAN guards once at the production execution boundary.
- Refuse pre-cancelled I/O without delegation or a misleading safety warning.
- Preserve the safety warning when a request may already have reached the ECU.
- Remove all 33 spike-proven redundant CAN and mixed-CAN pre-I/O poll sites.
- Test generic refusal, exposure and binding at their shared owners.
- Retain family tests only for unique continuation, error precedence,
  post-operation checkpoints, transcripts and lifecycle behavior.
- Deliver the complete stacked change atomically, with the ADR amendment in its
  own bottom PR.
- Keep the final stack within the validated NLOC budget.

## Non-goals

- Normalize K-Line, lifecycle, restart, mode-change, buffer-control,
  `RequestUnblock`, wait or platform cancellation behavior.
- Change wire bytes, accepted-operation ordering, timeouts, delays, retries,
  confirmations, progress, address-window guards or family continuation policy.
- Remove a post-operation or non-I/O cancellation checkpoint.
- Parse error detail strings to infer whether I/O was attempted.
- Add a new `ErrorKind`.
- Claim hardware qualification or change any family's qualification status.
- Obtain size savings by deleting unrelated code or moving code outside the
  measured scope.

## Domain language

**Pre-delegation refusal** is a `kCancelled` result with positive typed evidence
that the represented I/O operation was not delegated. It is quiet: no warning
claims that the request may be running on the ECU.

**May-have-started cancellation** is a `kCancelled` result after an operation
may have been accepted, or where exposure is unknown. It retains the existing
operator safety warning. Unknown exposure is conservative and therefore uses
this category.

Exposure describes the represented operation, not merely the last low-level
call. After a successful Send, cancellation from a delay, Receive or pending
reread is may-have-started even when that later call itself was refused before
delegation.

## Decision

### Typed cancellation exposure

`Error` gains a defaulted cancellation-I/O-exposure field. Its default is
may-have-started so existing errors remain conservative. Existing two-field
aggregate construction remains valid. The existing `kCancelled` kind continues
to determine workflow cancellation; exposure refines operator reporting and is
not a new outcome kind.

The guarded CAN and mixed-CAN transports mark their own entry refusals as
pre-delegation. Delegated results still pass through unchanged. `UdsClient`
marks its own pre-request refusal as pre-delegation, preserves provenance from a
failed Send, and promotes cancellation after a successful Send to
may-have-started for the pre-read delay, Receive and pending rereads.

The shared CAN exchange reporter logs nothing for typed pre-delegation
cancellation. It retains the existing safety warning for may-have-started
cancellation and existing rejection logging for non-cancellation failures. The
CVT Hitachi raw erase boundary and MH8104 raw exchange perform the corresponding
promotion after a successful Send. No decision depends on error-detail text.

### Execution and test ownership

`BoundAttempt` selects guarded execution by transport interface: CAN and
mixed-CAN receive their guards; K-Line remains undecorated. Preparation,
configuration, Open, Close, unblock and caller-owned lifetimes stay outside the
guard.

After redundant executor polls are removed, direct undecorated executor
execution assumes that the supplied CAN transport enforces entry refusal.
Production satisfies that assumption through `BoundAttempt`; direct family
tests use cancellation-aware transports or exercise `BoundAttempt` when entry
refusal is part of the case. Direct tests of family-owned logic remain
undecorated where cancellation entry refusal is irrelevant.

Generic next-I/O refusal and typed exposure belong to the guard, `UdsClient` and
shared reporter tests. Binding tests prove production selection for CAN,
mixed-CAN and unchanged K-Line. Family tests do not repeat generic refusal;
they remain only where the family contributes observable continuation,
precedence, post-operation, transcript or lifecycle behavior.

### Poll deletion boundary

Remove the 33 spike-proven pre-I/O poll sites across the Denso ISO-15765, Denso
BEEF, Hitachi, CVT, TCU, EEPROM, SH7058, SH72543R and mixed DensoCAN owners.
Each removed check must lead directly to guarded or `UdsClient`-owned I/O with
no intervening wait, log, event, confirmation, validation or computation whose
ordering it owns.

Retain every post-operation check, cancellable wait, accepted-operation safety
warning, retry bound, confirmation, progress/event boundary, validation,
lifecycle check and non-I/O checkpoint. The spike's detailed removal and
retention ledger is an implementation input; the delivered repository keeps a
compact durable ownership matrix rather than the spike transcript.

## Size and verification budget

Net-negative NLOC is no longer a completion condition. Against pinned trunk
`e62684af59f1bd3c98b20596ee0503327eea8a94`, the delivered stack must not exceed:

- 66,409 production NLOC (`+4`);
- 89,884 test NLOC (`+1,101`).

These are hard maximums, not targets. A rebase onto a newer trunk requires a
new baseline and preserves the same maximum deltas of `+4` production and
`+1,101` test NLOC unless a separately approved ADR amendment changes them.
Every cleanup still prefers deletion and shared ownership; the budget does not
authorize unrelated growth.

Top-of-stack verification includes the release application build, full release
test suite, all Prek hooks, full clang-tidy, convergent Gazelle generation,
coverage, mutation checks for changed cancellation lines, exact NLOC
measurement and scoped review. Hardware qualification remains a separate
recorded activity.

## Atomic stack

Deliver the work as a linear `gh stack` and merge it atomically with squash
commits only after every layer is green and reviewed:

1. standalone ADR 0025 amendment and revised design;
2. conservative reconstructed #908 rollout;
3. interface-owned guard binding and removal of migration aliases;
4. generic test-ownership compression;
5. typed cancellation exposure and normalized CAN logging;
6. the 33-poll deletion with corresponding test alignment;
7. compact durable evidence and temporary-plan closure.

No branch is pushed, force-updated, marked ready or merged without explicit
authorization at that execution stage.

## Approaches considered

### Chosen: retain the rollout with normalized logging and a bounded growth budget

This keeps centralized production ownership and conservative family evidence,
fixes misleading cancellation warnings, and removes every poll proven redundant
by the spike. It accepts measured test growth rather than weakening behavior or
pretending the rollout is net-negative.

### Rejected: stop broad expansion after the Colt pilot

This best satisfies the original size objective and avoids typed exposure, but
abandons the desired uniform CAN/mixed-CAN execution boundary and leaves
cancellation ownership distributed across families.

### Rejected: radically shrink family evidence to recover the old size gate

Shared-owner tests could replace more family cases, but deleting unique
continuation, precedence and post-operation evidence would make the rollout
harder to trust. Test simplicity is defined by clear ownership, not by removing
behavior-specific cases merely to satisfy a line metric.

### Rejected: retain current logging and accept larger growth

This preserves literal historical output but keeps a warning that can falsely
say a refused request may be running on the ECU. Once the distinction is known
and can be represented without string parsing or a new outcome kind, retaining
that behavior is not justified.

## Consequences

- Cancellation detail text is no longer a stable family-specific contract for
  pre-delegation refusal; `ErrorKind`, typed exposure and operator-event policy
  are the durable contract.
- `Error` carries conservative transport-exposure metadata used by CAN flash
  reporting, while existing callers remain source-compatible through its
  default.
- Test NLOC grows because generic provenance and accepted-operation safety
  behavior receive explicit owner tests. This is accepted within the cap.
- Direct executor tests must respect the executor's transport contract after
  family entry polls are removed.
- The size budget, 33-poll inventory and exact pinned baseline make the accepted
  growth auditable rather than open-ended.
- K-Line and other domains may adopt the terminology only through separate
  evidence and decisions; this ADR does not broaden their behavior.
