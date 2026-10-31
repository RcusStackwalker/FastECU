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

Generic refusal of the next I/O belongs to the guarded transport and is tested
there. Production binding tests prove that `BoundAttempt` selects each guarded
interface, including both raw and ISO operations for mixed-CAN. Family suites
retain error precedence, tolerated continuation, post-operation checkpoints,
wire behavior and forbidden subsequent family work. Ordinary family-unit tests
exercise undecorated execution; composition cases exercise `BoundAttempt`.

Remove an executor poll only when its sole effect is equivalent to refusal of
the immediately following guarded I/O. Preserve returned errors and details,
logging, progress, events, ordering, timeouts, delays, retries, confirmations,
post-operation checks and non-I/O boundaries. Runner setup, cancellable waits,
platform checks and `UdsClient` checks for other callers keep their owners.

The migration is complete only after redundant polls and generic family tests
are removed and both production and test NLOC are below the recorded trunk
baseline. Record and measure the selected trunk before implementation; a rebase
onto newer trunk requires a new baseline. Audit rollout growth is a transitional
cost, not savings or completed ownership transfer. If safe cleanup cannot meet
both size gates, reconsider this decision rather than weaken behavior or the
measurement. Hardware evidence remains a separate qualification requirement;
this software migration makes no hardware-qualification claim.

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
- The decision, conservative audited rollout, binding cleanup, test compression
  and poll deletion remain independently reviewable. Neither a transcript
  framework nor a generated-scenario framework is required. Transitional
  support growth is permitted only before the completion gates above are met.
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
