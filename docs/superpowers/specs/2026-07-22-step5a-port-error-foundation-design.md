# Step 5a — Port & Error Foundation (+ Logging Proof) — Design

**Status:** Approved 2026-07-22. First sub-project of step 5; see
[`2026-07-22-step5-backend-portable-design.md`](2026-07-22-step5-backend-portable-design.md)
for the umbrella architecture this inherits.

**Predecessor:** the C++23 toolchain bump (step 5-pre) must land first —
`std::expected` requires C++23.

**Goal:** land the shared vocabulary of step 5 — `Result`/`Error` and the
non-transport ports — and prove it carries a real caller by rewiring the logging
protocol seam onto it. No sample-model redesign, no thread move, no flash work:
those are later sub-projects. 5a exists so that when 30 flash files and a
3,397-line `FileActions` adopt this vocabulary, it has already survived contact
with a live consumer.

---

## Scope

**In scope:**

1. `src/backend/ports/` — new package of backend-owned interface headers:
   `result.h`, `error.h`, `clock.h`, `cancellation.h`, `event_sink.h`,
   `file_repository.h`, `settings.h`. Header-only, portable (no `QT_DEPS`).
2. Portable `cc_test`s for `Result`/`Error` and for exercising the ports through
   in-test fakes.
3. Concrete platform adapters under `src/platform/desktop/**` (`QtClock`,
   `QtCancellationToken`, `QtEventSink`, `QtFileRepository`, `QtSettings`) and
   their injection at the `apps/desktop` composition root.
4. **The logging proof:** convert the `LoggingProtocol` seam
   (`src/backend/logging/logging_protocol.h` and the three implementations —
   `ssm`, `mut_dma`, `cdbg`) to return `fastecu::Result` and to consume
   `IClock` / `ICancellationToken` / `IEventSink` where they currently reach Qt
   for timing, cancellation, and log/prompt emission. `LoggingEngine` /
   `LoggingWorker` call sites are adapted to the new return types — a minimal
   mechanical change, **not** the thread inversion.

**Explicitly deferred to 5b:**

- The `LogSample` model redesign (stable channel ID, numeric/raw value, unit;
  UI owns locale formatting). 5a leaves `LogSample` as-is —
  `{ int logValueIndex; QString displayValue; }` — so `LoggingProtocol` still
  references `QVector<LogSample>` and therefore is **not** yet added to the
  portable closure. 5a's proof is "the seam works under a real caller," not
  "logging is portable."
- Moving `LoggingWorker` off `QThread`.
- Retiring the `LoggingEngine` Qt signal surface.

**Out of scope:** any flash, `FileActions`, or `MainWindow` change.

---

## The foundation

### `result.h` / `error.h`

```cpp
// src/backend/ports/error.h
namespace fastecu {
enum class ErrorKind {
  InvalidConfig, Timeout, Disconnected, BadResponse,
  Cancelled, Unsupported, Internal,
};
struct Error {
  ErrorKind kind;
  std::string detail;  // human-readable context; never the sole control signal
};
const char* to_string(ErrorKind);  // stable identifiers for logs/tests
}  // namespace fastecu
```

```cpp
// src/backend/ports/result.h
#include <expected>
namespace fastecu {
template <class T> using Result = std::expected<T, Error>;
using Status = std::expected<void, Error>;  // void-returning ops
inline std::unexpected<Error> fail(ErrorKind k, std::string d = {}) {
  return std::unexpected(Error{k, std::move(d)});
}
}  // namespace fastecu
```

Callers branch on `kind`, never on `detail` text. `to_string(ErrorKind)` gives
tests and logs a stable spelling. These headers pull in only `<expected>`,
`<string>` — trivially portable, and the whole reason the C++23 bump precedes
this sub-project.

### The ports

All are pure virtual, backend-owned, platform-implemented, injected at
`apps/desktop`. Signatures below are the 5a contract; later sub-projects may add
methods but not change these.

```cpp
// clock.h
class IClock {
 public:
  virtual ~IClock() = default;
  virtual std::uint64_t now_ms() const = 0;                 // monotonic
  virtual Status sleep(int ms, const ICancellationToken&) = 0;  // Cancelled if interrupted
};

// cancellation.h
class ICancellationToken {
 public:
  virtual ~ICancellationToken() = default;
  virtual bool cancelled() const = 0;  // cooperative; polled in loops
};

// event_sink.h — replaces backend Qt signals and every backend QMessageBox
enum class LogLevel { Error, Warning, Info, Debug };
class IEventSink {
 public:
  virtual ~IEventSink() = default;
  virtual void log(LogLevel, std::string_view message) = 0;
  virtual void progress(int done, int total) = 0;
  // Non-blocking notice. Interactive confirmation is modeled as a typed
  // request answered by the UI before/around the backend call, not as a
  // blocking backend prompt — detailed where a consumer first needs it.
  virtual void notice(std::string_view message) = 0;
};

// file_repository.h — replaces QFile / QFileDialog in backend
class IFileRepository {
 public:
  virtual ~IFileRepository() = default;
  virtual Result<std::vector<std::uint8_t>> read(std::string_view handle) = 0;
  virtual Status write(std::string_view handle, std::span<const std::uint8_t>) = 0;
};

// settings.h — replaces QSettings reads in backend
class ISettings {
 public:
  virtual ~ISettings() = default;
  virtual std::optional<std::string> get(std::string_view key) const = 0;
  virtual void set(std::string_view key, std::string_view value) = 0;
};
```

`IFileRepository` and `ISettings` gain concrete adapters in 5a for completeness
and to lock their shape, but their first backend consumer is 5d — 5a does not
convert any `FileActions` code. Defining them now, alongside the ports the
logging proof exercises, means the full port vocabulary lands in one reviewable
package rather than dribbling in.

---

## The logging proof

### Why logging

The logging protocols are the backend workflow closest to already satisfying the
target shape: their transports are already injected ports
(`IKlineTransport`/`ICanTransport`/`ISsmTransport`), and the `LoggingProtocol`
interface already isolates `start`/`poll`/`stop`. What it lacks is exactly what
5a introduces — a structured error carrier and injected clock/cancellation/event
ports instead of ad-hoc `bool + QString*` returns and direct Qt timing. That
makes it the cheapest honest test of the vocabulary.

### Interface change

```cpp
// before
virtual bool start(QString* errorOut) = 0;
virtual PollResult poll(int timeoutMs) = 0;   // Status{Ok,NoResponse,TransportError}

// after
virtual fastecu::Status start() = 0;
virtual fastecu::Result<PollData> poll(int timeoutMs) = 0;
```

`PollData` carries the *successful-exchange* outcome and keeps `LogSample`
untouched for 5b:

```cpp
struct PollData {
  bool responded;                 // false == car silent this cycle (was NoResponse)
  QVector<LogSample> samples;     // valid when responded; unchanged shape (5b de-Qt's it)
};
```

**Mapping the old `PollResult::Status` — this is the equivalence contract:**

| old | new |
|-----|-----|
| `Ok` + samples | `Result` holding `PollData{responded=true, samples}` |
| `NoResponse` | `Result` holding `PollData{responded=false, {}}` (a value, not an error) |
| `TransportError` + `errorMessage` | `fail(ErrorKind::Disconnected \| Timeout \| BadResponse, errorMessage)` |

Car-silence stays a normal value because the worker's miss-threshold /
reconnect logic treats it as recoverable, not as session failure. Only genuine
transport faults become `Error`. Which `ErrorKind` each existing `TransportError`
site maps to is decided per site during implementation and pinned by a test.

### Port injection into the three implementations

Where the protocol implementations currently sleep, check elapsed time, poll for
cancellation, or emit a log/prompt via Qt, they take `IClock` /
`ICancellationToken` / `IEventSink` by reference (constructor-injected, supplied
by the platform factory). The blocking reads remain transport-timeout-bounded as
today; cancellation is additionally polled cooperatively so platform teardown
can unblock a session promptly.

### `LoggingEngine` / `LoggingWorker` fallout

These stay `QObject`/`QThread` in 5a (thread inversion is 5b). They are adapted
mechanically to the new signatures: `start()` checks `Status.has_value()` instead
of a `bool`; the `poll` loop branches on `Result` — error ⇒ the existing
transport-error path, `responded==false` ⇒ the existing no-response path,
`responded==true` ⇒ emit samples as today. The `LOG_E/W/I/D` signal surface is
unchanged in 5a; the platform `IEventSink` adapter forwards to it.

---

## Enforcement

Extend `scripts/check-portable-closure.py` (`//:portable_closure`) to add
**`src/backend/ports`** to the Qt/JNI-free closure. Verify non-vacuous: inject a
`QT_DEPS` into the ports target, observe the check fail, then restore — the same
discipline used for `//:bazel_openssl_wiring` and the step-4 closure.

The logging protocol targets are **not** added to the closure in 5a — they still
carry `QVector<LogSample>`. They join in 5b when the sample model de-Qt's. The
spec is explicit about this so a reviewer does not read the unchanged logging
Qt-dependency as an incomplete conversion.

`//:serial_compat_allowlist` is untouched by 5a (no flash work).

---

## Testing

**Equivalence-proof discipline (step-4 precedent):**

1. The existing logging tests (`test_ssm_logging_protocol`, the MUT/DMA and CDBG
   protocol tests) keep running, adapted to the new return types, asserting the
   **same vectors and the same status mapping** per the table above. A passing
   adapted test on identical inputs is the behavior-unchanged proof.
2. Add **portable `cc_test`s** for `Result`/`Error` (construction, `fail`,
   `to_string` stability) and for each port driven through an in-test fake
   (`FakeClock`, `FakeCancellationToken`, `RecordingEventSink`,
   `InMemoryFileRepository`, `InMemorySettings`) — no `QT_DEPS`. The link with no
   Qt is itself the portability proof for the foundation.
3. New tests co-located as `src/backend/ports/*_test.cpp`; the package `srcs`
   glob excludes `*_test.cpp`.

**Gates:**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Standing invariants: `docs/coverage-baseline.txt` unchanged; `//:fastecu`
resolvable by both packaging scripts; test count only increases; the cancellable
`IClock::sleep` and cooperative cancellation are exercised by `FakeClock`, not by
real elapsed time, to keep the suite fast and deterministic.

---

## Risks

| Risk | Mitigation |
|------|------------|
| `std::expected` unavailable → build breaks | 5-pre C++23 bump is a hard predecessor; 5a does not start until it lands |
| Silent behavior drift when remapping `PollResult::Status` → `Result` | The mapping table is the contract; adapted existing tests must stay green on identical vectors |
| Reviewer reads unchanged logging Qt deps as incomplete | Spec states logging joins the portable closure in 5b, not 5a |
| `*_test.cpp` swept into the shipping `ports` library | Add `exclude=["*_test.cpp"]` when the package glob is created, before the first test file |
| Ports over-designed ahead of consumers | Signatures frozen to what the logging proof exercises plus the minimum for `IFileRepository`/`ISettings` shape; later sub-projects add methods, not reshape |

---

## Deliverable checklist

- [ ] `src/backend/ports/` with the seven headers, portable target, `*_test.cpp`
      excluded from `srcs`.
- [ ] Platform adapters + `apps/desktop` injection.
- [ ] `LoggingProtocol` seam on `fastecu::Result` + injected ports; three impls
      converted; engine/worker call sites adapted.
- [ ] Existing logging tests adapted and green; new portable foundation tests.
- [ ] `//:portable_closure` covers `src/backend/ports`, verified non-vacuous.
- [ ] Umbrella gates pass; standing invariants intact.
