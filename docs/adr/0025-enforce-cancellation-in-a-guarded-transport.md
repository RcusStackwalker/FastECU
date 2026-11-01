# ADR 0025: Enforce Cancellation in a Guarded Transport

## Status

Accepted direction, narrowed after the
[size-reduction pilots](../reference/guarded-transport-pilot-lessons.md).
The original Colt pilot in [PR #756](https://github.com/RcusStackwalker/FastECU/pull/756)
had not merged or completed its bench exit gate at the 2026-10-19 design review.
The replacement Colt-only implementation now has a guarded CAN port, executor
opt-in at BoundAttempt, and four fewer explicit Colt polls. The
[pilot results](../reference/colt-cancellation-centralization-results.md) record
verification and delivery CI; the
[flash contract](../reference/flash-contracts.md#colt-cancellation-ownership)
owns its implemented boundaries. The [durable completion contract](#durable-completion-contract)
below defines the proposed CAN and mixed-CAN end state; that rollout and cleanup
are not yet implemented. Hardware qualification remains unchanged.

## Context

At the original strategy baseline, every flash executor tested the cancellation
token itself and mapped transport failures itself. Production flash code
mentioned `cancel` about 1,100 times, and about 193 executor cases re-proved
generic behavior family by family. Half the executors also reached the wire
through `UdsClient`, which checks the token before each request, duplicating
those families' checks. [ADR 0024](0024-wire-transcripts-and-goldens-are-the-test-contract.md)
places shared-policy tests at their owner. The pilot narrows that ownership
change to cancellation refusal; protocol error handling remains separate.

## Decision

- A **guarded transport** centralizes refusal of new I/O when cancellation is
  observed before delegation. The first pilot implements only the existing
  `ICanFlashTransport` port for Colt; executor signatures and caller-owned
  transport lifetime remain unchanged
  ([ADR 0015](0015-caller-owns-flash-transport-lifetime.md)).
- The guard returns delegated results unchanged. It does not normalize error
  kinds, interpret absent replies, impose a stop-after-error policy, or check
  cancellation again after a delegated call. Existing transport and clock
  handling of cancellation during operations remains in force.
- Only the migrated executor's explicit token polls are removed. Runner setup
  checks, cancellable waits, platform checks, and `UdsClient` checks for its
  other callers remain. Removing further checks requires a separate caller
  and boundary audit.
- A cancellation signal after the final successful operation does not
  retrospectively cancel completion when no later cancellable work observes it.
  This does not override lower-layer cancellation results or family validation.
- Protocol validation, abort/continue decisions, and plan confirmations stay
  with their current owners. Declining a confirmation is a distinct operator
  decision even where the executor reports it using `kCancelled`.
- The pilot adds no critical-section or expected-silence API. A decorator alone
  cannot guarantee deferral across clocks, drivers, clients, and unblock paths.

### Durable completion contract

The Colt pilot's per-executor opt-in is a migration mechanism. Per-family audits
record the evidence needed to expand guard coverage; they do not establish
permanent executor aliases or a second cancellation-policy selection framework.
After all CAN implementations have been audited, `BoundAttempt`-owned execution
selects the guard by the executor/transport interface: CAN and mixed-CAN use
their matching guards, while K-Line retains its existing execution path.
Preparation, configuration, Open, Close, unblock and caller-owned lifetimes
remain outside the execution guard.

Cancellation reporting uses typed evidence about the represented operation.
`Error` gains a defaulted `CancellationIoExposure` field named
`cancellation_io_exposure`, with `kMayHaveStarted` as the conservative default
and `kRefusedBeforeDelegation` for positive evidence of refusal. Existing
two-field aggregate construction remains valid; `kCancelled` remains the
workflow outcome, with no new `ErrorKind` and no inference from detail strings.

**Pre-delegation refusal** is a `kCancelled` result with typed evidence that the
represented I/O was not delegated. Guards mark their entry refusals and
`UdsClient` marks pre-request refusal this way. The shared CAN reporter emits
no event for this refusal. Delegated guard results pass through unchanged, and
`UdsClient` preserves failed-Send provenance.

**May-have-started cancellation** follows possible acceptance or unknown
exposure and retains the existing ECU safety warning. After successful Send,
`AfterIoMayHaveStarted(Error)` promotes cancellation from the pre-read delay,
Receive and pending rereads, even if that later call was itself refused. CVT
Hitachi raw erase and MH8104 raw exchange apply the same promotion. The helper
promotes only `kCancelled`; non-cancellation rejection logging stays unchanged.

Generic refusal, exposure and reporting tests belong to the guard,
`UdsClient` and shared reporter. Production binding tests prove CAN, mixed-CAN
(raw and ISO) and unchanged K-Line selection. Family suites retain unique
continuation, error precedence, post-operation checkpoints, transcripts and
lifecycle behavior. Test simplicity means clear ownership, not deleting unique
evidence to meet a line metric. Direct executor tests use cancellation-aware
transports or `BoundAttempt` when entry refusal is part of the case; family-only
logic may still use undecorated execution.

Remove all 33 spike-proven redundant CAN and mixed-CAN pre-I/O poll sites. Each
removed check must lead directly to guarded or `UdsClient`-owned I/O without an
intervening wait, log, event, confirmation, validation or computation whose
ordering it owns. Preserve wire bytes, accepted-operation ordering, timeouts,
delays, retries, confirmations, progress, address-window guards, family
continuation, post-operation checks and non-I/O boundaries. Pre-delegation
detail text is no longer a stable family contract; kind, typed exposure and
operator-event policy are. Runner setup, cancellable waits, lifecycle, restart,
mode changes, buffer control, `RequestUnblock`, platform checks and K-Line keep
their owners.

Net-negative NLOC is no longer a completion condition. The pinned trunk
`e62684af59f1bd3c98b20596ee0503327eea8a94` measures 66,405 production and
88,783 test NLOC. The complete stack must not exceed 66,409 production NLOC
(`+4`) or 89,884 test NLOC (`+1,101`). These are hard maximums, not targets or
permission for unrelated growth. A rebase onto newer trunk requires a new
baseline while preserving those maximum deltas unless a separately approved
ADR amendment changes them. Cleanup still prefers deletion and shared ownership.

Deliver seven independently reviewable layers as one linear, atomically merged
stack: this ADR amendment and revised design; conservative reconstructed #908;
interface-owned guard binding; generic test-ownership compression; typed
exposure and normalized logging; all 33 poll removals and test alignment; then
durable evidence and temporary-plan closure. Preserve designs/plans in lower
delivered history before their later deletion. Every layer must be green and
reviewed before atomic squash merge. Remote submission, readiness and merge
each require explicit authorization at that execution stage.

Top-of-stack gates include the release application build, full release tests,
all Prek hooks, clang-tidy, convergent Gazelle generation, coverage, changed-line
cancellation mutation checks, exact NLOC measurement and scoped review.
Hardware evidence remains a separate recorded qualification requirement; this
software migration makes no hardware-qualification claim. The
[revised design](../superpowers/specs/2026-11-01-can-cancellation-normalized-logging-design.md)
records the measured rationale for replacing the former size gate.

## Alternatives

- **Enforce it in `UdsClient` and the exchange helpers.** This is less new
  code, but only the 15 executors using `UdsClient` would be covered. The raw
  K-Line families would keep their own checks and tests.
- **Keep per-executor checks and share only the tests.** This leaves repeated
  polling and decisions about cancellation boundaries scattered across families.

## Consequences

- A migrated executor can propagate cancellation from its transport rather
  than polling explicitly. This is an ownership change, not evidence that all
  cancellation checks throughout the stack can be removed.
- "Next I/O" describes a cancellation observation before delegation, not an
  atomic guarantee against a concurrent cancellation signal. Existing bounded
  driver operations and cancellable waits retain their own semantics.
- Changed cancellation boundaries and operator events require explicit removal
  evidence. Family-specific early returns and tests protecting forbidden
  subsequent work remain necessary.
- The seven stack layers remain independently reviewable. Neither a transcript
  framework nor a generated-scenario framework is required. Measured support
  growth is accepted only within the hard completion budget above.
- This ADR owns the durable completion contract; the flash contract describes
  implemented binding, and the pilot results retain preservation evidence,
  measurements and delivery gates. Per-family audits gate software expansion;
  applicable recorded bench evidence gates hardware qualification. K-Line and
  deferral require separate decisions.

## Addendum (2026-10-25)

K-Line M32R SSM exchanges already refuse and order cancellation inside
[m32r_ssm_exchange.h](../../src/backend/flash/ecu/m32r_ssm_exchange.h): before
the write, after the write and after the read, with transport errors winning
over cancellation. That module, not a guarded transport, owns those checkpoints
for the executors that use it, and the required-reply exchange added there
inherits them unchanged. This does not expand the guarded transport to K-Line
and does not touch transfer-loop checks, which stay with each executor.
