# ADR 0025: Enforce Cancellation in a Guarded Transport

## Status

Accepted direction for CAN flash executors, expanded on 2026-10-25 through
explicit per-executor migration. The
[size-reduction pilots](../reference/guarded-transport-pilot-lessons.md)
established the narrower cancellation-refusal contract retained here.
The original Colt pilot in [PR #756](https://github.com/RcusStackwalker/FastECU/pull/756)
had not merged or completed its bench exit gate at the 2026-10-19 design review.
The replacement Colt-only implementation now has a guarded CAN port, executor
opt-in at BoundAttempt, and four fewer explicit Colt polls. The
[pilot results](../reference/colt-cancellation-centralization-results.md) record
verification and delivery CI; the
[flash contract](../reference/flash-contracts.md#colt-cancellation-ownership)
owns its implemented boundaries. Colt remains the only opted-in executor;
this revision establishes migration requirements for additional CAN executors,
not evidence that they have migrated or gained hardware qualification.

## Context

At the original strategy baseline, every flash executor tested the cancellation
token itself and mapped transport failures itself. Production flash code
mentioned `cancel` about 1,100 times, and about 193 executor cases re-proved
generic behavior family by family. Half the executors also reached the wire
through `UdsClient`, which checks the token before each request, duplicating
those families' checks. [ADR 0024](0024-wire-transcripts-and-goldens-are-the-test-contract.md)
places shared-policy tests at their owner. The ownership change covers
cancellation refusal; protocol error handling remains separate.

Other CAN executors do not all have Colt's checkpoints. Some check after a
successful operation, and some give cancellation precedence over a transport
error. Mixed raw-CAN/ISO-15765 DensoCAN uses a separate port. Adding a guard can
itself change which operation is refused, even before existing polls are removed.
Expansion therefore requires an audit for each executor rather than a universal
runner switch.

## Decision

- A **guarded transport** centralizes refusal of new I/O when cancellation is
  observed before delegation. This decision covers CAN flash executors using
  `ICanFlashTransport` and mixed raw-CAN/ISO-15765 executors using
  `IMixedCanFlashTransport`. The existing guard implements the former for Colt;
  mixed CAN requires a separate decorator covering both raw and ISO-15765
  Read/Write paths under the same contract. Executor signatures and caller-owned
  transport lifetime remain unchanged
  ([ADR 0015](0015-caller-owns-flash-transport-lifetime.md)).
- Migration is explicit per executor through the existing `CancellationGuard`
  opt-in at BoundAttempt. The runner constructs the borrowed guard only around
  Execute, after Open; direct Execute callers must supply the selected guard.
  Non-opted-in executors retain their existing path. No blanket CAN opt-in is
  introduced.
- The guard returns delegated results unchanged. It does not normalize error
  kinds, interpret absent replies, impose a stop-after-error policy, or check
  cancellation again after a delegated call. Existing transport and clock
  handling of cancellation during operations remains in force.
- Only explicit token polls shown redundant by the migrated executor's boundary
  audit may be removed. Post-operation checks, checks before non-I/O work and
  error-precedence rules are not automatically redundant. Runner setup checks,
  cancellable waits, platform checks and `UdsClient` checks remain; removing
  shared-client checks requires a separate audit of all callers.
- A cancellation signal after the final successful operation does not
  retrospectively cancel completion when no later cancellable work observes it.
  This does not override lower-layer cancellation results or family validation.
- Protocol validation, abort/continue decisions, and plan confirmations stay
  with their current owners. Declining a confirmation is a distinct operator
  decision even where the executor reports it using `kCancelled`.
- Lifecycle, in-session restart, mode changes, buffer control and RequestUnblock
  retain their existing owners and cancellation behavior. The guard adds no
  gate to them; their checkpoints must be audited separately from Read/Write.
- This decision adds no critical-section or expected-silence API. A decorator alone
  cannot guarantee deferral across clocks, drivers, clients, and unblock paths.
  K-Line and cancellation deferral remain outside this decision's scope.

## Per-executor migration requirements

Each migration must:

- Review the [Colt results](../reference/colt-cancellation-centralization-results.md)
  and [pilot lessons](../reference/guarded-transport-pilot-lessons.md), then
  inventory the executor's callers, transport paths, explicit polls and existing
  cancellation boundaries before opting in. Include shared dialect helpers,
  pending/retry reads, waits, restarts and raw/ISO-15765 transitions where used.
- Record the effect of adding the guard and of each removed poll on returned
  errors, logs, progress, confirmations and subsequent work. Retain existing
  post-operation and error-precedence behavior unless a change is explicitly
  justified and tested. Do not apply Colt's late-completion behavior to a family
  whose existing checkpoint still observes cancellation.
- Retain protocol validation and family abort/continue tests, including tolerated
  failures and best-effort operations. A guard refuses I/O; it does not force
  the executor to propagate every cancellation result or terminate its retries.
- Prove runner wiring with a cancellation-blind inner transport and deterministic
  cancellation between accepted Write and the following Read. The test must fail
  if the guard is bypassed. Cover every guarded I/O path used by the executor,
  including both modes for mixed CAN, and update direct Execute test callers.
- Run the applicable focused and full repository gates, and record boundary
  evidence and measured production/test support cost. Test compression remains
  separately assessed under
  [ADR 0024](0024-wire-transcripts-and-goldens-are-the-test-contract.md);
  centralization alone establishes neither net savings nor redundant tests.
- Consult the [qualification matrix](../flash-qualification-matrix.md) and the
  applicable family checklist. Record any cancellation-boundary divergence and
  outstanding bench evidence without changing hardware status absent a recorded
  qualification run.

## Alternatives

- **Guard every CAN executor in the runner immediately.** This gives central
  coverage, but adding refusal before unaudited calls can change error ordering,
  events and retry behavior even while executor polls remain. Explicit opt-in
  makes each ownership change reviewable against its family's evidence.
- **Enforce it in `UdsClient` and the exchange helpers.** This is less new
  code, but does not cover direct CAN calls or mixed-CAN raw traffic. The port
  boundary covers these paths without making a protocol client their owner.
- **Keep per-executor checks and share only the tests.** This leaves repeated
  polling and decisions about cancellation boundaries scattered across families.

## Consequences

- A migrated executor can propagate cancellation from its transport rather
  than polling explicitly. This is an ownership change, not evidence that all
  cancellation checks throughout the stack can be removed.
- "Next I/O" describes a cancellation observation before delegation, not an
  atomic guarantee against a concurrent cancellation signal. Existing bounded
  driver operations and cancellable waits retain their own semantics.
- Each migration requires an explicit inventory of changed cancellation
  boundaries and operator events. Family-specific early returns and tests
  protecting forbidden subsequent work remain necessary.
- Centralization and test compression are independently assessed changes.
  Neither a transcript framework nor a generated-scenario framework is required
  for migration, which retains existing family behavior tests.
  Initial measured support growth is permitted and must not be called savings.
- The flash contract owns implemented runner opt-in; the pilot results retain
  historical preservation evidence, measurements and delivery gates. This
  revision replaces the blanket requirement to complete Colt compression and a
  Colt bench run before any expansion with the per-executor requirements above.
  Those outstanding Colt tasks remain outstanding; they are not evidence for
  another family's software migration or hardware qualification.

## Addendum (2026-10-25)

K-Line M32R SSM exchanges already refuse and order cancellation inside
[m32r_ssm_exchange.h](../../src/backend/flash/ecu/m32r_ssm_exchange.h): before
the write, after the write and after the read, with transport errors winning
over cancellation. That module, not a guarded transport, owns those checkpoints
for the executors that use it, and the required-reply exchange added there
inherits them unchanged. This does not expand the guarded transport to K-Line
and does not touch transfer-loop checks, which stay with each executor.
