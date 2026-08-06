# Step 5b — Logging Use Case & Thread Inversion — Design

**Status:** Approved 2026-07-22. Second sub-project of step 5; inherits the
fixed vocabulary from the
[step-5 umbrella design](2026-07-22-step5-backend-portable-design.md) and
builds on the merged step 5a foundation (`14799c3`, PR #73).

**Goal:** make the complete runtime logging workflow portable. The backend
exposes a synchronous, bounded, cancellable logging use case over typed
`start`/`poll`/`stop` protocol operations and stable, locale-independent sample
data. Qt owns the worker thread and UI adaptation; the backend owns no threads,
Qt types, mutable UI definition state, filesystem behavior, or dialogs.

Behavior is preserved unless this design explicitly changes ownership. Existing
wire bytes, channel ordering, raw-value assembly, expression evaluation,
silence/reconnect policy, status transitions, and fixed-decimal display output
are equivalence contracts.

---

## Scope and boundary with 5d

### In scope

1. A portable, validated logging-session snapshot with typed protocol identity,
   stable channel IDs, channel acquisition metadata, conversion metadata, unit,
   and display precision.
2. A Qt-free `LoggingProtocol` seam with typed, cancellation-aware, bounded
   `start`, `poll`, and `stop` operations.
3. A synchronous portable logging use case that owns handshake, polling,
   silence tracking, reconnect policy, status transitions, sample conversion,
   event delivery, cancellation, and cleanup.
4. Qt-free SSM, MUT/DMA, and CDBG logging protocols and the protocol helpers
   required by them, preserving their wire behavior byte-for-byte.
5. Result/cancellation-aware logging transport operations sufficient to
   distinguish normal silence, timeout, adapter loss, cancellation, malformed
   response, and write failure.
6. Moving `LoggingWorker` and the QObject `LoggingEngine` facade under
   `src/platform/desktop/common/logging/`. The worker hosts one synchronous
   backend call on `QThread`; it contains no logging workflow policy.
7. Desktop adapters that snapshot the legacy definition data, map stable IDs
   back to the current UI lists, format numeric samples, and preserve the
   existing Qt signals/widgets/CSV behavior.
8. Adding the converted logging targets to the portable closure.

### Explicitly deferred to 5d

Step 5b does **not** move definition parsing out of `FileActions`. The desktop
composition layer snapshots the current `FileActions::LogValuesStructure` into
the portable model before it starts a session. Step 5d will own parsing and
producing that model as part of the broader `FileActions` decomposition.

The portable logging code never receives `FileActions`, its nested structs, or
its mutable parallel Qt lists. This is the deliberate seam between the two
sub-projects.

### Out of scope

- Flash operations, `FlashPlan`, and `flash_operation_worker`.
- General `FileActions` or `MainWindow` decomposition.
- UI redesign, CSV schema changes, or locale-policy changes.
- A universal transport or protocol abstraction beyond the three existing
  logging seams.
- Real-ECU qualification; existing synthetic/golden protocol tests remain the
  step gate.

---

## Chosen architecture

Three designs were considered:

1. **Portable synchronous use case (chosen).** `LoggingUseCase::run()` owns the
   workflow; the platform worker only supplies a thread and forwards events.
2. **Portable stateful session with a platform-owned loop.** The backend exposes
   `start`/`poll_once`/`stop`, but Qt owns miss counters and reconnect policy.
   Rejected because important backend workflow behavior would remain platform
   code and would have to be duplicated by Kotlin.
3. **Callback-driven portable state machine.** The platform feeds I/O results
   into a backend transition engine. Rejected because it adds scheduling and
   transition machinery without a current consumer that needs it.

The chosen shape is intentionally synchronous:

```cpp
fastecu::Status LoggingUseCase::run(
    const LoggingSession& session,
    LoggingProtocol& protocol,
    const fastecu::ICancellationToken& cancellation,
    ILoggingEventSink& logging_events,
    fastecu::IEventSink& diagnostics);
```

`run()` blocks only inside explicitly bounded protocol calls. The desktop runs
it on a Qt worker thread; a future Kotlin application can run the same function
on a coroutine dispatcher without changing backend behavior.

### Ownership

- The caller owns the immutable `LoggingSession` for the duration of `run()`.
- The caller constructs and owns one `LoggingProtocol` implementation.
- `LoggingUseCase` owns no thread and retains no platform object after `run()`.
- The desktop worker owns its cancellation source/token and Qt signal adapter.
- Concrete transports remain platform-implemented and backend-interface-owned,
  following the step 5 umbrella dependency direction.

---

## Portable session and sample model

### Stable identity

Channel identity is an opaque `std::string` copied from the definition's
`log_value_id`. It is not an index into a mutable list. Reordering definition
rows therefore cannot silently retarget a sample.

```cpp
enum class LoggingProtocolId { Ssm, MutDma, Cdbg };

enum class RawAssembly {
  DecimalBytesConcatenated,  // historical SSM behavior
  UnsignedIntegerDecimal,    // historical MUT/DMA and CDBG behavior
};

struct LoggingChannel {
  std::string id;
  std::uint32_t address;
  std::size_t length;
  RawAssembly raw_assembly;
  std::string from_byte_expression;
  std::string unit;
  std::uint8_t decimal_precision;
};

struct LoggingPolicy {
  int poll_timeout_ms;
  int car_silence_miss_threshold;
  int reconnect_attempt_threshold;
  int reconnect_retry_period;
};

struct LoggingSession {
  LoggingProtocolId protocol;
  std::vector<LoggingChannel> channels;
  LoggingPolicy policy;
};
```

Protocol-specific immutable setup values that are not channel properties (for
example SSM target address/framing selection) live in a small typed setup value
owned by the corresponding protocol factory. They do not become loosely typed
settings on `LoggingSession`.

### Validation

Session construction is a fallible factory returning `Result<LoggingSession>`.
It rejects with `InvalidConfig`:

- empty or duplicate channel IDs;
- non-positive timeouts or thresholds, and invalid reconnect periods;
- addresses or lengths outside the selected protocol's supported range;
- malformed or unsupported conversion expressions;
- malformed unit/format metadata or unsupported decimal precision;
- an empty channel selection where the selected protocol requires channels.

The model is immutable after construction. Validation occurs before serial
configuration or ECU I/O.

Construction rejects syntactically malformed expressions and provable
constant-zero denominators. It does not reject the entire class of expressions
with value-dependent denominators: if a particular raw value makes an otherwise
valid expression non-finite, conversion returns `InvalidConfig` at runtime and
the use case terminates the session. This is the only honest bounded rule for
arbitrary definition expressions without forbidding valid formulas wholesale.

### Samples

```cpp
struct LogSample {
  std::string channel_id;
  double numeric_value;
  std::string raw_value;
  std::string unit;
};
```

`raw_value` preserves the exact historical input to RomRaider expression
evaluation. SSM continues concatenating each response byte's decimal spelling;
MUT/DMA and CDBG continue spelling their decoded unsigned integer as decimal.
This may be surprising, but normalizing it would be a behavior change and is
outside 5b.

The backend returns a numeric value and never produces localized or
display-ready text. The desktop formats `numeric_value` using the snapshotted
channel precision and the same fixed-decimal rule as today. `unit` travels with
the sample for consumers that do not retain the session metadata.

---

## Protocol seam and transport behavior

### Protocol lifecycle

```cpp
struct ProtocolSample {
  std::string channel_id;
  std::string raw_value;
};

struct PollData {
  bool responded;
  std::vector<ProtocolSample> samples;
};

class LoggingProtocol {
 public:
  virtual ~LoggingProtocol() = default;
  virtual fastecu::Status start(
      const fastecu::ICancellationToken&) = 0;
  virtual fastecu::Result<PollData> poll(
      int timeout_ms,
      const fastecu::ICancellationToken&) = 0;
  virtual fastecu::Status stop() = 0;
};
```

`poll()` is bounded by `timeout_ms`. `PollData{responded=false}` means the ECU
was silent for this cycle and remains a successful value. It is not an error,
because the existing workflow counts misses and attempts reconnection.

All three implementations consume prevalidated channel data. They no longer
include Qt, `FileActions`, `SerialPortActions`, or mutable definition structs.
Channel selection, request construction, and raw assembly use STL containers
and the already-portable algorithms packages. A protocol returns only stable
channel IDs and historical raw strings; `LoggingUseCase` performs expression
conversion against the immutable session and emits public `LogSample`s. This
keeps conversion policy out of the wire-protocol implementations.

### Transport results and cancellation

The transport operations used by logging become `Result`-based and accept a
bounded timeout plus cancellation where they may block. A read represents the
normal no-frame case explicitly (for example
`Result<std::optional<Frame>>`/`Result<std::optional<Bytes>>`) rather than
overloading an empty byte vector with both silence and failure.

Concrete platform adapters map outcomes as follows:

| Outcome | Portable representation |
|---------|-------------------------|
| No frame before the poll deadline | successful empty optional; protocol returns `responded=false` |
| Adapter closed or dropped | `ErrorKind::Disconnected` |
| Cancellation observed/unblocked read | `ErrorKind::Cancelled` |
| Transport deadline failure not equivalent to normal no-frame | `ErrorKind::Timeout` |
| Malformed or negatively acknowledged ECU frame | `ErrorKind::BadResponse` |
| Invalid transport/session setup | `ErrorKind::InvalidConfig` |

Writes report structured failure rather than only a byte count. Exceptions are
caught by the adapter and converted to `Error`; none cross a port.

Only transport methods exercised by logging are reshaped in 5b. Existing
non-logging callers receive mechanical compatibility adaptations when a shared
interface requires it; their workflows are not redesigned. The transport
interfaces remain in `src/backend/protocol/` as fixed by the umbrella spec.

### Protocol helpers

The logging-relevant portions of `MutDmaDriver`, `CdbgLogDriver`, and SSM framing
move to STL containers and `Result`. Their current Qt overloads, if still needed
by an unconverted caller, belong in a Qt compatibility target outside the
portable dependency closure. Existing request/response vectors are unchanged.

CDBG's current `SerialPortActions` setup is platform work. The desktop factory
configures and opens raw 11-bit CAN at 500 kbit/s before constructing the
portable CDBG transport/protocol. Failure to configure/open is returned as a
structured error. The portable CDBG protocol never includes or calls
`SerialPortActions`.

---

## Workflow and event flow

`LoggingUseCase::run()` performs these steps:

1. Reject an already-cancelled call with `Cancelled` and no ECU I/O.
2. Call `protocol.start(cancellation)`.
3. Emit the typed `Running` state after successful start.
4. Poll back-to-back with the session's bounded timeout; there is no added
   delay, preserving current pacing.
5. On protocol samples, resolve each stable ID in the immutable session,
   evaluate its expression from the historical raw string, reset the miss
   counter, emit `Running` if recovering, and emit the ordered public sample
   batch.
6. On normal silence, increment misses, emit `CarNotResponding` at the existing
   threshold, and attempt reconnect at the existing threshold/cadence.
7. On cancellation or a terminal error, leave the loop.
8. Attempt `stop()` exactly once and return the final `Status`.

Logging-specific workflow events use a focused backend-owned interface rather
than expanding the cross-cutting 5a port with domain concepts:

```cpp
enum class LoggingState { Running, CarNotResponding };

class ILoggingEventSink {
 public:
  virtual ~ILoggingEventSink() = default;
  virtual void state_changed(LoggingState) = 0;
  virtual void samples(std::span<const LogSample>) = 0;
};
```

Diagnostic messages continue through 5a's `IEventSink::log()`. The returned
`Status` is the authoritative session completion result; the backend does not
need a duplicate "session ended" callback.

The desktop adapter converts logging state/sample callbacks into queued Qt
signals. It maps channel IDs to the legacy list indices captured when the
snapshot was built, formats values with fixed decimal precision, then calls the
existing widget and CSV paths. UI dialogs remain UI-owned and branch on
`ErrorKind`, not on `detail` text.

---

## Error, reconnect, and cleanup semantics

### Initial start

An initial `start()` failure terminates the session and preserves its kind. The
desktop may present a handshake/configuration/adapter message based on that
kind. The following 5a carry-over mappings become pinned tests now that the
consumer branches on kind:

| Site | Required kind |
|------|---------------|
| MUT/DMA free-form handshake failure | `BadResponse` |
| CDBG empty channel selection | `InvalidConfig` |
| CDBG free-form handshake failure | `BadResponse` |

An initially disconnected adapter remains `Disconnected`. SSM's existing
negative/malformed start reply remains `BadResponse`.

### Active polling and reconnect

- Normal no-frame results retain today's miss counter behavior.
- Retryable reconnect `BadResponse` preserves today's retry cadence.
- `InvalidConfig`, `Unsupported`, `Internal`, `Disconnected`, and `Cancelled`
  terminate immediately.
- A genuine `Timeout` error terminates; ordinary poll silence is not `Timeout`.
- When a reconnect succeeds, counters reset and `Running` is emitted exactly as
  today.

`detail` is diagnostic context only. No backend or platform control flow parses
or compares it.

### Exactly-once cleanup and error precedence

The use case has one scope-guarded cleanup path. `stop()` is attempted exactly
once after any `start()` attempt that may have changed protocol state. It is not
called concurrently from the platform thread.

If the main operation failed and `stop()` also fails, the original error wins;
the cleanup failure is logged through `IEventSink`. If cleanup is the only
failure, it becomes the returned error. A user stop is represented as
`Cancelled`; the desktop suppresses failure UI when it initiated that
cancellation.

---

## Thread inversion and teardown

`LoggingWorker` and `LoggingEngine` move from `src/backend/logging/` to
`src/platform/desktop/common/logging/` because both are Qt lifecycle adapters:

- `LoggingWorker` owns `QThread` execution and a `QtCancellationToken`.
- `LoggingEngine` retains the QObject registry/session facade needed by the
  current UI.
- The worker invokes one `LoggingUseCase::run()` call and forwards typed events
  and the final result. It does not count misses, choose reconnect timing,
  convert expressions, or classify protocol errors.

Teardown follows a fixed order:

1. Mark the cancellation token.
2. Ask the concrete transport adapter to unblock an active read when the driver
   offers such a facility.
3. Wait for the bounded backend call to observe cancellation and return.
4. Destroy the protocol/session only after the worker has joined.

No backend logging type derives from `QThread`/`QObject`, emits a Qt signal,
starts a timer, or relies on a Qt event loop. The maximum shutdown latency is
bounded by the transport's documented cancellation/unblock behavior and the
configured poll timeout; it is never unbounded.

---

## Build layout and portability enforcement

The implementation plan may refine target names, but the dependency split is
fixed:

- `src/backend/logging/`: portable session/sample types, conversion, protocol
  interface, logging events, and use case.
- `src/backend/logging/protocols/`: portable SSM, MUT/DMA, and CDBG protocol
  implementations.
- `src/backend/protocol/`: portable transport interfaces and portable protocol
  helpers used by logging.
- `src/platform/desktop/common/logging/`: Qt worker, engine, event adapter,
  definition snapshot adapter, legacy ID/index adapter, and fixed-decimal UI
  formatting.
- `src/platform/desktop/common/transport/`: concrete Qt/serial transport
  implementations and cancellation/unblock translation.

Portable targets are ordinary `cc_library` targets without `QT_DEPS`. Any
temporary Qt compatibility overload is isolated in a sibling compatibility
target consumed only from platform/unconverted code.

Extend `scripts/check-portable-closure.py` (`//:portable_closure`) to cover the
converted backend logging roots. Verify the extension non-vacuously: absence of
an expected target must fail, and injecting a forbidden Qt dependency must fail
before restoring the tree. The `//:serial_compat_allowlist` remains unchanged;
5b migrates no flash family.

---

## Testing and equivalence proof

### Portable use-case tests

Co-located Qt-free host tests use scripted protocols, a fake clock,
cancellation tokens, recording logging/diagnostic sinks, and no real elapsed
time. They cover:

- session validation, including duplicate/empty IDs and protocol constraints;
- stable channel identity across reordered source definition rows;
- exact historical raw assembly for SSM and MUT/DMA/CDBG;
- expression conversion, numeric value, unit, ordering, and precision metadata;
- successful start/poll/stop;
- normal silence, threshold transition, reconnect cadence, and recovery;
- every terminal `ErrorKind` branch;
- cancellation before start and during poll/reconnect;
- exactly-once cleanup and main-error-versus-cleanup-error precedence;
- the three deferred start-failure kind assertions from 5a.

### Protocol and transport tests

Adapt the existing SSM, MUT/DMA, and CDBG tests to STL/`Result` without changing
their inputs or expected request/response bytes. A passing adapted test on the
same vectors is the behavior-equivalence proof.

Platform transport tests distinguish:

- normal no frame;
- genuine timeout failure;
- adapter disconnect;
- cancellation/unblocked read;
- malformed/negative response;
- write failure.

A deterministic worker teardown test proves that cancelling a blocked logging
session joins within its bound. It must not depend on a real ECU or a flaky
wall-clock sleep.

### Desktop compatibility tests

Adapt the current `LoggingWorker` and `LoggingEngine` tests to their platform
location. Assert the same Qt status/session signals and user-stop behavior.
Add adapter tests proving that opaque IDs update the same legacy list entries
after definition row reordering and that fixed-decimal display strings remain
identical for representative positive, negative, integer, and fractional
values. Existing CSV/logging integration behavior remains unchanged.

### Coverage and gates

New tests remain co-located under `src/**/*_test.cpp` or under `tests/`, both of
which are measured by `scripts/coverage-local.sh`. New-code coverage must remain
at least 80%, and the SonarCloud Quality Gate must pass.

The umbrella spec's reference to `docs/coverage-baseline.txt` is stale after PR
#74. That file was intentionally removed. Step 5b must **not** recreate or check
it; the SonarCloud Quality Gate is the coverage gate.

Run at minimum:

```bash
bazel build -k --config=release //:fastecu //tests/... //src/...
bazel test  -k --config=release //tests/... //src/... \
            //:bazel_openssl_wiring //:serial_compat_allowlist \
            //:portable_closure
scripts/coverage-local.sh
```

Standing gates:

- `//:fastecu` remains resolvable by both packaging scripts;
- the portable closure is non-vacuous and rejects Qt/JNI;
- existing golden bytes/vectors and display strings are unchanged;
- test count does not decrease;
- SonarCloud Quality Gate passes with at least 80% new-code coverage.

---

## Risks and mitigations

| Risk | Mitigation |
|------|------------|
| SSM raw handling is accidentally "cleaned up" and changes values | Preserve decimal-byte concatenation explicitly in `RawAssembly`; pin existing vectors and converted outputs |
| Stable IDs no longer update the correct legacy UI row | Snapshot an adapter-owned ID/index map and test reordered rows |
| Thread move leaves reconnect policy in Qt | Worker tests assert it only hosts `run()`; portable use-case tests own all thresholds/transitions |
| Cancellation still waits indefinitely in a driver read | Add cancellation/unblock-aware transport results and a bounded teardown test |
| Shared transport signature changes spill into unrelated workflows | Limit changes to logging-used methods and mechanically adapt shared callers without redesign |
| `stop()` failure hides the useful primary error | Fixed error-precedence rule plus an explicit dual-failure test |
| Protocol helpers retain transitive Qt dependencies | Split portable core from any required Qt compatibility target; enforce with the portable closure |
| 5b absorbs the whole `FileActions` split | Snapshot at the desktop boundary; leave definition parsing/production to 5d |
| New portable code misses coverage | Co-located measured tests and the blocking SonarCloud new-code coverage gate |

---

## Deliverable checklist

- [ ] Portable validated `LoggingSession`, stable string channel IDs, and
      locale-independent `LogSample`.
- [ ] Portable expression/raw conversion preserving current SSM and
      MUT/DMA/CDBG semantics.
- [ ] Cancellation-aware typed `LoggingProtocol::start/poll/stop`.
- [ ] Synchronous `LoggingUseCase::run()` owning all workflow policy and
      exactly-once cleanup.
- [ ] Portable SSM, MUT/DMA, and CDBG protocols/helpers with unchanged golden
      vectors.
- [ ] Structured, bounded logging transport operations with normal silence
      distinct from errors.
- [ ] CDBG serial configuration moved to the desktop factory.
- [ ] `LoggingWorker` and QObject `LoggingEngine` moved to platform; backend
      owns no threads or Qt signal surface.
- [ ] Desktop snapshot, ID/index, event, formatting, and legacy UI/CSV adapters.
- [ ] Carry-over start-failure kind tests plus portable use-case, transport,
      teardown, and desktop equivalence tests.
- [ ] Portable closure extended non-vacuously to logging targets.
- [ ] Umbrella build/test/package gates and SonarCloud Quality Gate pass;
      new-code coverage is at least 80%.
