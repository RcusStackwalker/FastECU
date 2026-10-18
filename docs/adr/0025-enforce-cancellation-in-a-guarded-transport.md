# ADR 0025: Enforce Cancellation in a Guarded Transport

## Status

Accepted. Piloted on the Mitsubishi Colt M32R CAN family in phase 3 of the
[size-reduction strategy](../superpowers/specs/2026-10-18-size-reduction-strategy.md),
then rolled out per family.

## Context

Every flash executor tests the cancellation token itself and maps transport
failures itself. Production flash code mentions `cancel` about 1,100 times,
and the executor suites spend about 193 cases re-proving the same generic
behavior family by family. Half the executors also reach the wire through
`UdsClient`, which already checks the token before each request, so those
families check it twice. [ADR 0024](0024-wire-transcripts-and-goldens-are-the-test-contract.md)
requires generic behavior to be tested once, which needs one place that owns
it.

## Decision

- The flash runner wraps the caller-owned transport in a **guarded
  transport**: a decorator implementing the existing `ICanFlashTransport` and
  `IKlineFlashTransport` ports. Executor signatures do not change, and the
  caller still owns transport lifetime ([ADR 0015](0015-caller-owns-flash-transport-lifetime.md)).
- Before each I/O the guarded transport checks the token, so cancel takes
  effect at the next I/O. It maps transport errors and disconnects to one
  typed error that executors propagate unchanged.
- Executors stop checking the token, and `UdsClient` stops once all its users
  are behind a guarded transport. Critical sections are not part of the pilot.
  A family that must not be interrupted declares one, designed with its first
  user in phase 4. A real section must pass `inner` a never-cancelled token,
  survive `RequestUnblock`, and get past `UdsClient`'s own check. An exchange that may get no reply
  declares **expected silence** instead of having it inferred.
- Plan confirmations that the user declined are not cancellation and stay in
  the executor.

## Alternatives

- **Enforce it in `UdsClient` and the exchange helpers.** This is less new
  code, but only the 15 executors using `UdsClient` would be covered. The raw
  K-Line families would keep their own checks and tests.
- **Keep per-executor checks and share only the tests.** This leaves the
  1,100 call sites and the decision about where cancel can land spread across
  30 files.

## Consequences

- An executor that never touches the token is deliberate: cancellation reaches
  it as an error from its next transport call.
- Cancel granularity becomes "next I/O" everywhere. A family whose current
  code defers cancel must declare that span before its own checks are deleted.
  A new deferral is a wire-behavior change that needs bench evidence.
- The CAN decorator lands with the Colt pilot, and the K-Line one with the
  first K-Line family. Absent-reply mapping and expected silence move into the
  guarded transport with the first family that bypasses `UdsClient`.
