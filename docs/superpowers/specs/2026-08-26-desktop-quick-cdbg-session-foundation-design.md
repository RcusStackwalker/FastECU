# Desktop Quick Configurable CDBG Session Foundation — Design

**Status:** Approved 2026-08-26.

## Context

The approved [Desktop Quick Application and Configurable CDBG Dashboard
design](2026-08-24-desktop-quick-dashboard-design.md) divides delivery into
focused checkpoints. The QtQuick/Bazel shell, portable `.ohd` foundation, and
shared desktop logging runtime now have designs and implementation plans. The
next narrow checkpoint prepares a dashboard document for CDBG logging without
yet discovering or opening a local adapter.

The `.ohd` model already stores CDBG request and reply identifiers, stream
instance, sampling interval, retry policy, channels, conversions, and cards.
The portable CDBG implementation still uses fixed Colt CAN identifiers and
hard-coded stream settings, while the generic logging policy expresses retry
spacing as missed-poll counts. A document cannot drive the protocol correctly
until those contracts become explicit and time-based.

## Goals

- Convert a validated `DashboardDocument` into one internally consistent,
  portable prepared-session object.
- Select only channels referenced by dashboard cards and apply each card's
  chosen conversion.
- Make CDBG request/reply identifiers, stream instance, and sampling interval
  explicit validated protocol configuration.
- Replace poll-count reconnect scheduling with monotonic elapsed-time
  scheduling.
- Give `.ohd` sessions a finite reconnect-attempt budget while preserving the
  Widgets application's existing unlimited behavior and effective timing.
- Keep configuration and preparation failures detectable before transport or
  hardware access.

## Non-goals

- Adapter discovery, selection, matching, or session-local adapter overrides.
- Serial-port or raw-CAN configuration, bitrate application, identifier-width
  application, transport construction, or hardware opening.
- A desktop connection service, QtQuick controller, QML model, or live
  dashboard presentation.
- Changes to `.ohd` persistence, card editing, visualization, or file
  associations.
- SSM or MUT/DMA document profiles.
- Live reconfiguration of a running CDBG stream.

## Chosen architecture

The portable dashboard backend gains one preparation boundary:

```text
DashboardDocument
    -> prepare_dashboard_session()
        -> PreparedDashboardSession
             |-- LoggingSession
             `-- CdbgProtocolConfig
                    -> CdbgLoggingProtocol + caller-supplied transport
```

`src/backend/dashboard/dashboard_session_builder.{h,cpp}` owns document-aware
resolution. It returns `Result<PreparedDashboardSession>`. The prepared object
owns exactly one `LoggingSession` and one `CdbgProtocolConfig`; private
construction prevents callers from pairing results prepared from different
document revisions. It is movable so it can cross the later connection-service
boundary without copying the session.

`CdbgProtocolConfig` lives beside the portable CDBG driver under
`src/backend/protocol`. It contains only wire-protocol settings:

- request CAN identifier;
- reply CAN identifier;
- stream instance; and
- sampling interval in milliseconds.

A validating factory is the only public construction path. Bitrate, selected
identifier width, and preferred adapter remain connection concerns. Retry
behavior remains generic logging-session policy. The existing Colt constants
remain named defaults and the Widgets path uses them, preserving its wire
traffic.

`CdbgLogDriver` and `CdbgLoggingProtocol` receive the configuration explicitly.
The driver uses it for every handshake write, reply filter, streamed-frame
filter, reset command, and start command. The generic `ICanTransport` continues
to carry explicit arbitration identifiers; its documentation no longer claims
that every caller is inherently restricted to 11-bit identifiers because the
connection layer selects and enforces the transport mode.

## Dashboard session preparation

`prepare_dashboard_session(const DashboardDocument&)` performs an
all-or-nothing pipeline:

1. Revalidate the complete document defensively.
2. Validate and resolve every card's channel and conversion references.
3. Determine which catalog channels are referenced by cards.
4. Emit selected channels in catalog order, independent of card display order.
5. Apply the referencing card's selected conversion to its `LoggingChannel`.
6. Create the generic `LoggingSession` and `CdbgProtocolConfig` through their
   validating factories.
7. Return the combined prepared object only after every step succeeds.

Format version 1 permits a channel on at most one card, so each selected
channel has exactly one active conversion. Catalog order, rather than card
order, makes CDBG frame construction deterministic when users rearrange the
dashboard. Channels not referenced by cards consume no wire capacity.

The mapping to `LoggingChannel` retains the channel ID, address, byte length,
and unsigned-integer-decimal raw assembly, and takes expression, unit, and
precision from the selected conversion. Card title, display type, gauge bounds,
and sparkline history remain presentation data and do not enter the logging
session.

Preparation maps the document retry fields as follows:

| `.ohd` field | Generic logging policy |
|---|---|
| `poll-timeout-ms` | poll timeout |
| `silence-threshold` | consecutive misses before `CarNotResponding` |
| `reconnect-period-ms` | initial reconnect delay and later attempt spacing |
| `reconnect-attempts` | finite maximum restart invocations |

The preferred adapter, bitrate, and identifier width are intentionally not
copied into the prepared object. The later connection layer reads those values
from its own validated connection input and supplies a configured transport.

## CDBG protocol configuration and validation

The `CdbgProtocolConfig` factory rejects:

- request or reply identifiers above the absolute 29-bit CAN maximum;
- identical request and reply identifiers;
- an unrepresentable sampling interval; and
- any value that would require silent narrowing.

The document validator separately checks identifiers against its selected
11-bit or 29-bit width. The protocol factory repeats only the absolute CAN
limit because identifier-width selection is not a CDBG wire concern.

The CDBG start frame represents intervals directly through 65,535 ms. Larger
values use 10 ms units in a 16-bit field. Therefore valid intervals are:

- 1 through 65,535 ms; or
- multiples of 10 from 65,540 through 655,350 ms.

Both dashboard validation and the protocol-config factory enforce this rule.
No interval is truncated or rounded during frame construction.

The stream instance is already represented as a byte in the document and
configuration. All byte-sized and identifier values remain strongly typed or
range-checked before reaching frame builders.

## Time-based reconnect policy

The generic `LoggingPolicy` becomes:

- `poll_timeout_ms`;
- `car_silence_miss_threshold`;
- `reconnect_initial_delay_ms`;
- `reconnect_period_ms`; and
- optional `max_reconnect_attempts`.

`LoggingUseCase` receives an `IClock&` and compares monotonic deadlines. It
does not add sleeps: the existing poll loop supplies natural opportunities to
check the deadline. Time calculations use saturating arithmetic and tolerate a
monotonic clock value that reaches its maximum without wrapping deadlines.

After consecutive misses reach the silence threshold, the use case emits
`CarNotResponding` once and records the start of the reconnect wait. The first
restart invocation occurs when `reconnect_initial_delay_ms` has elapsed.
Further invocations occur no more frequently than `reconnect_period_ms`.
Every restart invocation consumes one attempt, whether `start()` succeeds or
returns a retryable `BadResponse`. Only receipt of a valid sample resets the
attempt count and returns the state to `Running`.

A reconnect error other than `BadResponse` remains terminal immediately.
When a finite budget is exhausted without a valid sample, the use case returns
`BadResponse` with the stable detail `logging reconnect attempts exhausted`.
The most recent underlying retry failure is sent to diagnostics so its detail
is retained without becoming an unstable public result contract.

For dashboard preparation, initial delay and repeat spacing both equal the
document's `reconnect-period-ms`, and `max_reconnect_attempts` is the document's
positive `reconnect-attempts`.

The Widgets compatibility boundary preserves its current effective schedule:

```text
initial delay ms =
    max(0, reconnect_attempt_threshold - car_silence_miss_threshold)
    * poll_timeout_ms

repeat interval ms = reconnect_retry_period * poll_timeout_ms
maximum attempts = unlimited
```

Compatibility conversion uses checked, saturating arithmetic. Existing
protocol-specific poll timeouts and miss thresholds remain unchanged.

## Errors and safety

Preparation errors use `ErrorKind::InvalidConfig` and stable dashboard field
paths. This includes missing channel or conversion references, duplicate card
orders or IDs, unsupported raw assembly, invalid retry/configuration values,
and a selected channel set that exceeds CDBG's eight-frame capacity. The
builder returns no partial session or configuration.

Configuration validation happens before transport access. Cancellation,
disconnection, handshake failure, and streamed-frame behavior retain their
existing error kinds. Unexpected reply identifiers or empty handshake replies
remain `BadResponse`; an unrelated or unusable streamed frame remains a normal
no-response poll.

This slice exposes no ECU write, flash, diagnostic, or actuator operation. Its
only protocol commands are the existing CDBG logging handshake and stream
configuration commands.

## Testing

### CDBG protocol configuration

- Accept the Colt default identifiers and stream settings.
- Accept non-default request/reply identifiers and stream instance.
- Reject equal identifiers and identifiers above 29 bits.
- Cover the interval boundaries at 1, 65,535, 65,536, 65,540, 655,350, and
  655,360 ms, including rejection of nonmultiples of 10 above 65,535.
- Prove frame construction never rounds a validated interval.

### CDBG driver and logging protocol

- Prove every handshake write uses the configured request identifier.
- Prove handshake and streaming reads filter the configured reply identifier.
- Prove reset and start frames use the configured instance and interval.
- Retain cancellation, closed-adapter, silence, frame decoding, and wire-shape
  coverage.
- Prove the Widgets/default construction remains byte-for-byte compatible with
  the current Colt traffic.

### Dashboard session builder

- Select only card-referenced channels.
- Preserve catalog order when card order changes.
- Apply each card's selected conversion, unit, and precision.
- Map `.ohd` retry and CDBG fields exactly.
- Reject empty selections, broken references, duplicate order/identity,
  invalid conversion input, and over-capacity wire shapes.
- Prove every failure returns no prepared object.

### Time-based logging use case

- Enter `CarNotResponding` at the configured miss threshold.
- Attempt no reconnect before the initial monotonic deadline.
- Enforce repeat spacing independently of poll duration and frequency.
- Count both successful and `BadResponse` restart invocations until a valid
  sample arrives.
- Reset state, deadline, and attempt budget only after a valid sample.
- Return the stable exhaustion error after a finite budget.
- Preserve unlimited Widgets retries.
- Cover cancellation, fatal reconnect errors, saturated deadlines, and
  deterministic timing with `FakeClock`.

### Regression gates

- Run the focused dashboard, logging-session, logging-use-case, CDBG driver,
  CDBG logging-protocol, worker, coordinator, and legacy-factory tests.
- Keep all affected backend targets in `//:portable_closure`.
- Build both `//:fastecu` and `//:fastecu-desktop-quick`.
- Run formatting, clang-tidy, and repository diff checks for changed files.

## Delivery sequence

1. Add validated `CdbgProtocolConfig` and make the portable driver/protocol use
   it while preserving Colt defaults.
2. Change generic reconnect policy and `LoggingUseCase` to injected-clock,
   elapsed-time scheduling.
3. Adapt the desktop worker and Widgets compatibility boundary, proving its
   effective timing and unlimited retries remain unchanged.
4. Add `PreparedDashboardSession` and the dashboard session builder.
5. Run portable-closure, both desktop builds, and focused regression gates.

Each step leaves the Widgets application buildable. The preparation boundary
is added only after its downstream protocol and retry contracts are stable.

## Acceptance criteria

- A valid `.ohd` document produces one prepared object containing a matching
  `LoggingSession` and `CdbgProtocolConfig`.
- Only card-referenced channels enter the logging session, in deterministic
  catalog order, with the selected conversions.
- Configured request/reply identifiers, stream instance, and exact sampling
  interval drive all relevant CDBG wire operations.
- Invalid or unrepresentable protocol settings fail before transport access.
- Dashboard reconnects use monotonic elapsed time and stop after the configured
  number of attempts without a valid sample.
- The Widgets application retains its current effective reconnect schedule,
  unlimited retry behavior, and default Colt CDBG traffic.
- No adapter discovery, hardware opening, transport construction, or QtQuick
  presentation behavior is introduced.
- Portable and desktop regression gates pass for all affected targets.

## Implementation-planning boundary

The implementation plan for this design covers only configurable CDBG protocol
settings, time-based generic reconnect policy, compatibility adaptation, and
dashboard session preparation. The next design checkpoint owns adapter
discovery, raw-CAN setup, transport construction, and the desktop connection
service.
