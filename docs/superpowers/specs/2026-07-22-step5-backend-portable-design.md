# Step 5 — Portable Backend Workflows — Umbrella Design

**Status:** Approved 2026-07-22. Supersedes the step 5 bullet list in
`docs/modularization-plan.md`, which this spec decomposes, sequences, and amends.

**Predecessor:** Step 4 (Portable Algorithms — Qt removal) — complete, PR #71.
`src/algorithms` is nine portable, Qt-free targets, each with a sibling
`:qt_compat` shim whose only remaining callers are backend and UI. Step 4
Amendment 1 deferred `Result<T>`, structured errors, typed identifiers,
validated value models, and the parsing/calibration/flash-planning migrations
to this step.

**Goal:** `src/backend` becomes portable — Qt-, thread-, filesystem-, and
dialog-independent — behind a small set of injected ports, so a future Kotlin
application can reuse it unchanged. Behavior is preserved except where a change
is called out as deliberate.

**Why this is an umbrella spec.** Step 5 is materially larger than step 4. Step
4 was a mechanical container-type substitution across nine packages. Step 5 is a
dependency inversion plus god-object decomposition spanning several independent
subsystems: a port/error foundation, ~30 flash operation families, a 3,397-line
`FileActions`, the logging path, and thread-ownership removal. That does not fit
one plan. This document fixes the shared vocabulary once — the port set, error
model, thread model, and `FlashPlan` shape — and sequences the work into four
sub-projects. **Each sub-project gets its own spec → plan → implementation
cycle.** Only 5a is designed in full elsewhere
(`2026-07-22-step5a-port-error-foundation-design.md`); 5b–5d are scoped here and
detailed when reached.

---

## Prerequisite (its own PR, before any 5a work)

**C++23 toolchain bump.** Move `.bazelrc` from `-std=c++20` to `-std=c++23` and
resolve any graph fallout. This gates the error carrier: step 5 uses
`std::expected`, which is C++23. The bump ships as a standalone PR with no
behavior change so its blast radius is isolated from the modularization work.

---

## Sub-project decomposition and sequence

| ID | Sub-project | Depends on | Core deliverable |
|----|-------------|-----------|------------------|
| **5-pre** | C++23 toolchain bump | — | `-std=c++23` across the graph |
| **5a** | Port & error foundation (+ logging proof) | 5-pre | `Result`/`Error` types, the non-transport ports, and the logging path rewired onto them to prove the seam under a real caller |
| **5b** | Logging use-case & thread inversion | 5a | Typed `start` / bounded `poll` / `stop`; stable sample model (channel ID, numeric/raw value, unit; UI owns locale formatting); `logging_worker` thread moves to platform |
| **5c** | Flash preflight/execution seam | 5a | `FlashPlan` build+validate; dialog-free execution; `flash_operation_worker` thread moves to platform; **proving pair** (one K-Line + one CAN family) migrated onto transport ports; `docs/flash-qualification-matrix.md` |
| **5d** | FileActions/MainWindow backend decomposition | 5a, 5c | Split `FileActions` into definition/calibration/checksum/logging/flash use cases; remove `QFileDialog`, `QMessageBox`, filesystem, and `SerialPortActions` from backend |
| **tail** | Per-family flash drain | 5c | ~28 remaining flash families, one to a few per PR, each shipping "experimental", draining the `serial_qt_compat` allowlist |

**Dependency shape:** `5-pre → 5a → {5b, 5c, 5d}`. After 5a, the three are
largely independent — logging, flash, and definition/calibration/checksum are
separate call graphs. The one coupling: 5d's *flash* use-case consumes 5c's
`FlashPlan`, so that slice waits on 5c; 5d's definition, calibration, and
checksum slices do not.

**Scope boundary with step 6.** The `MainWindow` thin-shell rewrite (Qt adapters
for ports, event marshaling to the GUI thread, construction moved into
`apps/desktop`, wrapper removal) stays in **step 6**. Step 5d extracts only the
backend-side use cases out of `FileActions`/`MainWindow`; it does not rewrite the
widgets.

---

## Shared architecture (the fixed vocabulary)

### Error model

```cpp
namespace fastecu {
enum class ErrorKind {
  InvalidConfig,   // invalid configuration or definition
  Timeout,         // bounded read/operation exceeded its deadline
  Disconnected,    // adapter/transport not open or dropped
  BadResponse,     // malformed or negatively-acknowledged ECU response
  Cancelled,       // cooperative cancellation observed
  Unsupported,     // operation not available for this target
  Internal,        // invariant violation / unexpected state
};
struct Error { ErrorKind kind; std::string detail; };
template <class T> using Result = std::expected<T, Error>;
}  // namespace fastecu
```

- **Exceptions never cross a port** or the future native ABI. Backend code may
  use exceptions internally but converts them to `Error` at the boundary.
- `void`-returning operations return `Result<void>`.
- The seven `ErrorKind` values are exactly the plan's "Public Interfaces and
  Error Handling" taxonomy. Do not add kinds without amending this section.
- This is the general infrastructure step 4 Amendment 1 deferred here. It
  extends the spirit of the concrete `ChecksumResult` precedent, not that type
  itself.

### Port set

Interfaces are **backend-owned**, **platform-implemented**, and **injected at
the `apps/desktop` composition root**. `platform → backend` is a permitted
direction; the concrete adapters live under `src/platform/desktop/**` and depend
on the backend interface, never the reverse.

Transport ports already exist in `src/backend/protocol/` and are kept in place:

- `mutdma::IKlineTransport` — byte-stream / K-Line.
- `cdbg::ICanTransport` — raw CAN frames (not ISO-TP).
- `ISsmTransport` — SSM request/response byte stream.

New cross-cutting ports land in a new package `src/backend/ports/`:

- `IClock` — monotonic `now()` and a cancellable `sleep(ms, ICancellationToken&)`;
  replaces `QThread::msleep` / `QElapsedTimer` in backend.
- `ICancellationToken` — `bool cancelled() const`; cooperative, polled in
  transfer and poll loops.
- `IEventSink` — progress, log-line, and prompt events; replaces backend Qt
  signals and every backend `QMessageBox`.
- `IFileRepository` — read/write ROM and definition bytes by handle/path;
  replaces `QFile` and `QFileDialog` in backend.
- `ISettings` — typed get/set; replaces `QSettings` reads in backend.

The transport ports are deliberately not moved into `src/backend/ports/` in step
5: relocating three headers with live callers is churn without payoff. A later
cleanup may consolidate them; this spec cross-references their current location
instead.

### Thread model

Backend owns no threads. The two backend `QThread` workers today —
`src/backend/flash/flash_operation_worker.*` and
`src/backend/logging/logging_worker.*` — move to platform (5c and 5b
respectively). Backend exposes **synchronous, bounded, cancellable** calls;
platform runs them on a Qt worker thread (future Kotlin: a coroutine on a
dispatcher) and marshals `IEventSink` callbacks to the GUI thread.

- Cancellation is cooperative via `ICancellationToken`, polled in loops.
- Every read has an explicit timeout.
- Platform teardown must unblock an active read; a cancelled or torn-down read
  returns `Error{Cancelled}` (or `Error{Disconnected}` on adapter loss), never
  blocks forever and never throws across the port.

### FlashPlan (owned by 5c)

Flashing splits into preflight and execution:

```cpp
Result<FlashPlan> build_plan(/* definition, rom bytes, target */);
Result<void> execute(const FlashPlan&,
                     /* transport(s) */,
                     IClock&, ICancellationToken&, IEventSink&);
```

- `build_plan` validates definition, ROM, block boundaries, and erase regions
  with **no irreversible I/O**. The UI derives its confirmation prompt from the
  returned plan.
- `execute` performs I/O, raises **no dialogs** (prompts become `IEventSink`
  events answered by the UI before or during the call), and preserves each
  family's exact existing wire sequence byte-for-byte.
- Per-family state machines stay separate. There is **no universal flashing
  abstraction** — this follows `docs/protocol-generalization-opportunities.md`,
  which only sanctions sharing pure byte algorithms, framing, validation
  primitives, block planning, and worker plumbing.

### Experimental flash status

Real-hardware bench qualification is **not** a step-5 gate — this project has no
bench access. Migrated flash families ship as **experimental** and are tracked
in a new `docs/flash-qualification-matrix.md` (`family | migrated | proven`
columns). This is a **docs-only** marker: no UI badge, no confirmation dialog
change. A family flips to `proven` only when someone with hardware qualifies it.

---

## Enforcement

**Primary mechanism stays construction, not inspection** (step-4 principle):
removing `QT_DEPS` from a converted backend target takes Qt off the include path
in Bazel's sandbox, so any surviving Qt include fails to compile.

**Secondary check.** Extend `scripts/check-portable-closure.py`
(`//:portable_closure`) to add backend targets to the Qt/JNI-free closure **as
each sub-project converts them**, never all at once:

- 5a adds `src/backend/ports` only. The logging protocols it rewires still
  carry `QVector<LogSample>`, so they cannot enter the portable closure until
  the sample model de-Qt's in 5b.
- 5b adds the logging protocol and use-case targets once the sample model
  de-Qt's and the thread move lands.
- 5c adds each migrated flash family.
- 5d adds the extracted definition/calibration/checksum/logging/flash use cases.

The `//:serial_compat_allowlist` frozen list only ever shrinks: 5c and the tail
remove `serial_port_actions.h` callers as families migrate. `@openssl` remains
an adjudicated exception (step 7 decides its fate); the closure check rejects Qt
and JNI only.

Each extension must be **non-vacuous** — verified to fail when a target is absent
as well as when it is non-conforming, as was done for `//:bazel_openssl_wiring`.

---

## Testing and gates (umbrella-wide)

Run after every migration group:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

- **Golden vectors unchanged** for all mechanically moved or extracted behavior.
- **New portable tests are co-located** as `src/**/*_test.cpp` with `cc_test`
  targets in the owning package (step-4 precedent); `srcs` globs exclude
  `*_test.cpp`; SonarCloud `sonar.tests` already covers `src/**/*_test.cpp`.
- **`docs/coverage-baseline.txt` remains a must-not-change invariant.**
- Portable backend tests run as **Qt-free host `cc_test`s** — the link itself is
  the portability proof.
- Test the error paths the taxonomy names: invalid config/definition, timeout,
  disconnect, malformed/negative response, cancellation, unsupported operation.
- `//:fastecu` stays resolvable by both packaging scripts throughout.
- **Real ECU/adapter behavior is a separate, deferred gate**, not required for
  step-5 completion. Flash families ship "experimental"; see the qualification
  matrix.

---

## Amendments to `docs/modularization-plan.md`

1. Step 5 is delivered as four sub-projects (5a–5d) plus a C++23 prerequisite
   and an incremental per-family flash tail, not one monolithic change.
2. The error carrier is `std::expected<T, Error>` (C++23); a standalone
   toolchain-bump PR precedes 5a.
3. The `MainWindow` thin-shell rewrite remains in step 6; step 5d extracts only
   backend-side use cases.
4. Flash migration in step 5 covers the `FlashPlan`/port seam plus a proving
   pair; the remaining ~28 families drain incrementally afterward.
5. Flash families ship "experimental" tracked in
   `docs/flash-qualification-matrix.md`; bench qualification is not a step-5
   gate (no bench access).
