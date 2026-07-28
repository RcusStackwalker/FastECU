# Package-owned Mocks Adherence Design

## Purpose

Bring all existing test doubles for interfaces owned by `src/backend/**` and
`src/algorithms/**` into full compliance with
[ADR 0008](../../adr/0008-use-package-owned-mocks.md).

This is a migration of current test doubles only. It does not add a lint,
presubmit, or other mechanism to prevent future violations.

## Scope

A test double is in scope when it substitutes for an interface whose source
package is under `src/backend/**` or `src/algorithms/**`. The double is in scope
regardless of whether it currently lives beside a unit test or under the
top-level `tests/` package.

The current audit found violations for backend ports, logging, flash, and
protocol interfaces. It found no current violations for interfaces owned by
`src/algorithms/**`.

The following are out of scope:

- interfaces owned outside `src/backend/**` and `src/algorithms/**`;
- production implementations such as `NullEventSink`, `NeverCancelled`,
  concrete logging protocols, and flash cancellation tokens;
- test fixtures and helpers that do not substitute for an in-scope interface;
- new automated enforcement.

## Ownership and package structure

Each interface-owning package owns its test doubles in a `testing/`
subpackage. Every concrete double has:

- its own header;
- its own `cc_library`;
- its own `cc_test`.

Each `testing/BUILD.bazel` declares `package(default_testonly = True, ...)`.
Targets inherit test-only status from the package instead of repeating
`testonly = True`. Visibility is limited to the repository packages that need
the doubles. Consumer targets depend on individual doubles rather than a
combined mocks library.

### Backend ports

`src/backend/ports/testing/` remains the owner of doubles for backend port
interfaces.

- Add one configurable cancellation token. It supports a constant state, a
  mutable state, cancellation after a configured number of checks, and a
  predicate tied to another test object's state.
- Extend `FakeClock` instead of adding a second clock double. Its default mode
  keeps the current behavior: `now_ms()` is stable and `sleep(ms)` advances by
  the non-negative duration. An optional auto-advance step makes each
  `now_ms()` call return the current time and then advance it. An optional
  fixed sleep step advances by that step instead of the requested duration.
  Configuring both steps to 10 ms preserves the behavior currently local to
  `tests/test_ssm_logging_protocol.cpp`.
- Extend existing in-memory and recording doubles when a consumer-local
  specialization expresses reusable interface behavior. In particular,
  `InMemoryFileSystem` gains explicit `create_directory` failure injection,
  and the file-repository double gains the recording behavior needed by the
  snapshot adapter tests.

### Backend logging

Create `src/backend/logging/testing/`.

- A scripted `LoggingProtocol` owns queued start and poll results, stop
  behavior, call counts, poll timeouts, and other history currently asserted
  by logging tests.
- A recording `ILoggingEventSink` owns state transitions and emitted sample
  batches.

These replace both test-local logging doubles and the shared
`tests/scripted_logging_protocol.h`.

### Backend flash

Create `src/backend/flash/testing/`.

- Move the scripted CAN flash transport into this package.
- Move the scripted K-line flash transport into this package.
- Add a minimal base `IFlashTransport` stub for tests that require an object
  of the wrong concrete transport family.

The moved scripted transports preserve their expectation, response queue,
call-history, unblock, and script-consumption APIs.

### Backend protocol

Create `src/backend/protocol/testing/`.

- Move the scripted CAN transport into this package.
- Move the scripted K-line transport into this package.
- Move the scripted SSM transport into this package.

The moved doubles preserve their existing scripts and observable histories.

## Behavior and data flow

Consumers configure a package-owned double, pass it through the production
interface, execute the production unit, then inspect the double's public state
or script status. Test-framework assertions remain in the consumer or the
double's dedicated test, not inside reusable double methods.

The configurable cancellation token replaces the existing fixed, mutable,
check-count, and poll-dependent token classes. Predicate configuration handles
poll-dependent cancellation without coupling the ports package to logging.

Scripted doubles retain queued results and explicit expectations. When
overlapping existing implementations differ, the canonical double keeps the
union of behavior that current consumers exercise. Migration changes includes,
Bazel dependencies, and construction/configuration; it does not change
production behavior or consumer assertions.

## Error handling

Package-owned doubles use the same `Status`, `Result`, and `ErrorKind` types as
their interfaces. Their defaults are successful and deterministic.

Failure injection is explicit configuration on the owning double rather than
consumer-side inheritance. Script exhaustion and expectation mismatches remain
observable through each scripted double's existing result or status/history
API. Reusable double methods do not throw exceptions or invoke test-framework
assertions.

## Migration sequence

For each interface:

1. Characterize the behavior and observability used by all current consumers.
2. Add or extend the package-owned double and its dedicated test.
3. Switch consumers to the package-owned target without changing their
   behavioral assertions.
4. Run the double test and directly affected consumer tests.
5. Remove the superseded local class or top-level shared header.

After all consumers migrate, remove obsolete top-level `tests/scripted_*.h`
files that implement in-scope interfaces. Test helpers unrelated to an
in-scope interface remain untouched.

## Verification

Every double's independent `cc_test` covers:

- default behavior;
- every configuration and failure mode used by consumers;
- call recording, ordering, and script consumption where applicable;
- cancellation and clock boundary behavior;
- expectation mismatch or exhausted-script behavior where applicable.

Verification proceeds from the narrowest affected targets to all directly
affected consumer tests, then to the repository's broader Bazel test suite and
existing formatting and static checks.

A final source audit covers `src/backend/**`, `src/algorithms/**`, and
top-level `tests/**`. It identifies classes implementing interfaces owned by
the two modern source trees and confirms that every test double resides in the
owning package's `testing/` subpackage.

## Completion criteria

The migration is complete when:

- no test-local or top-level shared double remains for an interface owned by
  `src/backend/**` or `src/algorithms/**`;
- every remaining in-scope double resides under its owner's `testing/`
  subpackage;
- every double has its own `cc_library` and `cc_test`;
- every `testing/` package sets `default_testonly = True`;
- consumer targets depend only on the individual doubles they use;
- existing test behavior remains equivalent, including the SSM clock's
  auto-advancement;
- the affected tests, broader Bazel suite, formatting checks, and static checks
  pass; and
- no automated future-enforcement mechanism is introduced.
