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
owns its enduring boundaries. Expansion and hardware qualification remain gated.

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
- Removing Colt's four checks requires an explicit inventory of changed
  cancellation boundaries and operator events. Family-specific early returns
  and tests protecting forbidden subsequent work remain necessary.
- Centralization and test compression are independently assessed changes.
  Neither a transcript framework nor a generated-scenario framework is required
  for the cancellation pilot, which retains existing Colt behavior tests.
  Initial measured support growth is permitted and must not be called savings.
- The flash contract owns runner opt-in; the pilot results retain preservation
  evidence, measurements and delivery gates.
  A separately assessed compression change and the applicable recorded Colt
  bench checklist precede any expansion. K-Line, additional families, and
  deferral need separate decisions after the pilot's evidence is reviewed.
