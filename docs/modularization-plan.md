# FastECU Modularization and Android-Readiness Plan

## Current State and Destination

Reviewed on 2026-09-29. Steps 1-6, including 6n-3, are structurally implemented.
Step 7 has not started.
Implementation completion and hardware qualification are separate statuses.

The portable algorithms and backend workflows already exist: configuration,
definitions, calibration and map editing, checksums, logging, diagnostics,
service functions, and the retained flash families. Desktop consumers use the
portable configuration, logging and calibration records directly.

The current boundary is:

```text
Desktop composition roots ──► UI + desktop adapters
Desktop UI ─────────────────► designed UI-facing adapters + backend + algorithms
Desktop adapters ───────────► backend + algorithms
Future Android native seam ─► backend + algorithms
Backend ────────────────────► algorithms
```

Composition wires UI-owned channels to platform services. A UI dependency on
a platform target is allowed when that target is a deliberately designed
adapter with target-level visibility for the UI; it does not grant access to
platform internals.

### Established foundations

- Bazel is the sole build graph, with package-owned targets, tests, and
  restricted visibility. The project uses C++23. OpenSSL has been removed;
  pugixml is the adopted portable XML dependency. There is no remaining
  aggregate implementation target to retire.
- Portable backend code owns workflow policy, uses injected ports, and owns
  no threads or direct filesystem I/O. Desktop adapters supply those services.
- All retained flash families use portable plans and executors through the
  common workflow/dialog architecture. The unreachable Hitachi M32R JTAG
  stub was removed rather than migrated.
- Desktop composition owns long-lived services and logging registration.
  Flash dispatch, diagnostics, connection/identification, platform selection,
  and UI channels have been separated from their former central wiring.
- The serial facade is platform-only. No GRANDFATHERED UI visibility entry
  remains. Designed UI-facing adapters still exist and are intentional.
- Qt byte-container conversions live under `src/platform/desktop/common/bytes`,
  with target-level visibility for their desktop UI consumers.

### Remaining structural debt

- `MainWindow` retains presentation coordination for write preflight, checksum
  interaction, logging selection, connection orchestration and log-file handling;
  single-consumer presentation flows need not become portable ports.
- Android build configuration, native facade, and smoke fixture do not exist.

The destination remains a reusable, Qt-, JNI-, and OS-independent algorithms
and backend core, with desktop presentation and adapters outside it. Desktop
closure is structurally complete; Android seam work follows.
Android v1 remains MUT/DMA live logging over USB serial, API 29, `arm64-v8a`;
the Kotlin product app and real Android USB implementation are later work.

## Architectural Rules Learned

The [design notes](design-notes.md) preserve the rationale and rejected
alternatives; the [ADRs](adr/README.md) record accepted structural decisions.
The rules below constrain the remaining milestones.

- **Reuse portable policy.** `LoggingUseCase::run()` owns miss counting,
  reconnect cadence, and status transitions. Platform code supplies execution
  context and forwards events. Protocol-level `start`/`poll`/`stop` does not
  mean a future platform should rebuild the use-case loop.
- **Keep errors and outcomes explicit.** Backend ports return `Result<T>` /
  `Status`; exceptions never cross ports or the future native ABI. Ordinary
  non-response is a successful value, distinct from a transport fault or
  timeout error. I/O must be bounded and cancellable, and platform teardown
  must unblock active reads. The returned result is authoritative, while
  events carry notices and progress.
- **Keep lifecycle at the caller.** Flash attempts own transport setup and
  final cleanup; executors receive the appropriate open transport. Preserve
  protocol-required transitions inside the wire sequence and typed
  executor/transport binding. See [ADR 0015](adr/0015-caller-owns-flash-transport-lifetime.md).
- **Keep operator decisions outside executors.** Collect confirmations before
  an attempt or between bounded workflow attempts. An event sink never waits
  for an answer. Preserve cancellation, close/error precedence, and the
  composition lifetime that keeps services alive until their consumers stop.
- **Use the narrowest useful boundary.** Name distinct wire behaviors
  explicitly, such as raw versus echo-checked writes. Keep single-consumer
  presentation flows in the UI. Desktop-specific services may use concrete
  adapters or UI-owned channels; do not invent portable ports for services
  the portable core does not need.
- **Preserve model ownership and compatibility.** Logger definitions, operator
  selections, ECU capabilities, and live samples have different writers.
  Preserve that separation, SSM raw-value assembly, existing file formatting,
  and calibration layout rules while replacing their representations.
- **Prove sharing and corrections.** Share protocol code only after byte-level
  tests establish equivalent behavior. Keep family-specific sequencing,
  timeouts, retry rules, and safety policy explicit. A defect correction needs
  local evidence, a reproducing test, exact corrected expectations, and a
  qualification note; use a mutation check to prove the test detects it.
  See the [protocol-sharing boundary](design-notes.md#where-port-then-factor-shared-code-and-where-it-did-not)
  and [flash qualification matrix](flash-qualification-matrix.md).
- **Enforce boundaries through the build graph.** Qt reachability is gated by
  [ADR 0016 visibility](adr/0016-enforce-qt-reachability-by-visibility.md).
  Remove transitional entries from `qt_layer` as consumers migrate; never add
  new ones. `implementation_deps` keeps facade headers out of production UI
  compile inputs, enforced by sandboxed Linux/macOS builds. Windows alone
  does not prove that header boundary.
- **Portable means location.** Everything under `//src/backend/...` and
  `//src/algorithms/...` is portable. The
  [portable-core gate](../scripts/android-cross-compile.sh) fails if a
  `//src/platform` label is reachable from it and builds all of it for Android
  arm64, so it is not a Qt/JNI source scan.

## Remaining Roadmap

Steps 1-6n are structurally implemented (see the
[completed milestones](#completed-milestones)); Step 7 is the only remaining
milestone. Implementation completion is not hardware qualification: full
release build/test, formatting, changed-file static analysis and platform
CI/packaging results must be recorded before release, and the post-read
handoff and checksum/save/write paths still need bench re-verification. The
migrations establish no new hardware qualification, and automated success
alone does not either.

The desktop-closure exit gate holds: no Qt-typed legacy package remains under
algorithms/backend, all transitional `qt_layer` entries are removed, portable
the portable-core gate covers the resulting portable targets, and production UI
cannot reach serial facade headers. Preserve the documented
[calibration corrections and open defects](design-notes.md#calibration).

### 7 — Android seam

Start after 6n. **Done (spike):** the portable closure cross-compiles for
Android arm64 with C++23. Everything under `//src/backend/...` and
`//src/algorithms/...` (portable gtest binaries included, compile and link
only) builds under `--config=android`, pinned to `rules_android_ndk`
0.1.5 and verified against NDK r30 (30.0.16248370, C++23 via the NDK's libc++).
Run it with `scripts/android-cross-compile.sh` (needs `ANDROID_NDK_HOME`); CI
runs it as the `android-cross-compile` job. The NDK toolchain is registered
only by `--config=android` ([android.bazelrc](../bazel/android.bazelrc)), so
desktop builds need no NDK. Still open: the API level (the plan targets 29;
the spike used the toolchain default) and a hermetic NDK pin for CI. Keep Android dependencies isolated
from desktop targets, adding Kotlin rules only if the fixture requires them.

Introduce the native facade under `src/platform/android/native`: versioned
`fastecu_v1_*` C symbols, opaque handles, fixed-width POD values, caller-owned
buffers, structured errors, and transport callbacks. Wrap the existing
logging use case and cancellation/event contracts so the caller supplies
execution context without duplicating reconnect or polling policy. Keep
STL layouts and exceptions behind the ABI and JNI out of the portable core.

**Exit gate:** host ABI contract tests cover ownership, buffer sizing,
invalid inputs, cancellation, errors, and teardown. An API-29/arm64 no-UI
`android_native_smoke_apk` fixture proves native compilation, exported
symbols, and packaging. Desktop builds remain independent of Android setup.
This proves a native seam, not Android USB behavior or device qualification;
the Kotlin product application remains out of scope.

## Verification and Qualification

### Automated gates

For implementation milestones, run focused package-owned tests while making
changes, then the current repository-wide gates:

```sh
bazel build -k --config=release //...
bazel test -k --config=release //...
bazel run --config=release //:clang_tidy_report_changed
```

The portable-core gate (`scripts/android-cross-compile.sh`, CI job
`android-cross-compile`) is separate from `bazel build //...`. Qt restrictions
are enforced by visibility.

Require the Windows/macOS/Linux CI matrix and Windows/macOS packaging checks.
Coverage is gated through SonarCloud on new code. Follow the [coding style and testing conventions](coding-style.md)
and use package-owned mocks. QtTest suites using Google Mock must propagate
its failures into their exit status. An empty Windows QtTest log is not
proof of a crash; see the [coverage reliability notes](tech-debt.md#p0-make-coverage-results-trustworthy).

Portable tests cover parser/model validation, checksum and ROM outcomes,
scripted successful operations, malformed replies, non-response, timeout,
disconnect, cancellation, and preflight rejection without real hardware.
Preserve golden vectors for mechanically migrated behavior. UI/platform
tests cover wiring, execution context, and lifetime. Add Android ABI and
smoke-build gates only when step 7 creates them.

### Hardware evidence is a separate gate

Use the [flash qualification matrix](flash-qualification-matrix.md) for each
family's supported operations, deliberate corrections, and hardware status.
Use its linked family checklists for verification; a portable executor and
passing automated tests do not make a family hardware-qualified.

Desktop qualification remains tracked in the [logging engine checklist](logging-engine-bench-checklist.md),
[logging composition checklist](logging-composition-bench-checklist.md),
[diagnostics checklist](diagnostics-bench-checklist.md),
[connection checklist](connection-bench-checklist.md),
[platform-selection checklist](platform-selection-bench-checklist.md), and
[checksum-dialog notes](checksum-dialog-bench-notes.md). Keep the
[CDBG checklist](cdbg-can-logging-bench-checklist.md) for its specific wire path.

There is limited real bench evidence: the [bench CLI checklist](bench-cli-checklist.md)
records verified effects of the Colt redirect helpers, with a carrier
verification caveat and no retained raw CAN trace. It does not qualify the
CLI or desktop workflow end to end. Preserve that scope and all unresolved
family-specific gates, including the Hitachi M32R CAN TCU erase-scope gate.
Record ECU, adapter, operation, revision, traces/results, and sign-off before
advancing a hardware status.

### Related work

The [technical debt roadmap](tech-debt.md) owns outstanding defects and
broader cleanup; its "Logging engine follow-ups" section holds the focused logging items.
Those findings can lag code: verify one before scheduling it.

Do not turn every debt item into an Android prerequisite. Evidence-dependent
protocol changes, generic Sonar cleanup, and broader UI features remain
separate work. In particular, the `wrx02` predicate, calibration selection
bounds, DataTerminal delay parsing, and CAN identification changes need their
own focused treatment rather than being hidden in a structural migration.

## Completed Milestones

These identifiers remain stable for existing references. Detailed rationale
lives in the [design notes](design-notes.md); per-family behavior lives in the
[qualification matrix](flash-qualification-matrix.md). Git history retains the
completed specs and implementation plans.

| Step | Implemented outcome |
|---|---|
| Prerequisite | Bazel-only build; qmake removed (`66dd62e`, ADR 0001). |
| 1 | Post-migration characterization and golden baseline (#46). |
| 2 | Mechanical organization into `apps`, `src`, and `resources`. |
| 3 | Package-owned targets and restricted visibility; aggregate source manifest removed. |
| 4 | Portable algorithms; transitional Qt conversions isolated. Later steps drained all but the byte-conversion shim. |
| 5 | Portable ports, logging policy, definition/config/calibration/checksum services, and flash plan/executor migration. Wave 7 removed legacy flash implementations; Qt-typed desktop bridges remain for step 6. |
| 6a | `FileActions` de-widgeted; definition-authoring dialogs moved to UI. |
| 6b | Portable calibration map-edit use case; documented corrections (b)-(h) fixed, (a) remains open. |
| 6c | Desktop composition owns service construction and teardown. |
| 6d | Flash dispatch separated; calibration preflight and handoff intentionally retained for model migration. |
| 6e | Direct/remote selection and OS implementations selected by composition/build wiring. |
| 6f | Logging protocol registration moved to desktop composition. |
| 6g | Diagnostic link boundary, portable DTC sessions and MUT memory helpers; dead tools removed. |
| 6h | Connection adapter and asynchronous SSM identification; legacy UI serial access drained. |
| 6i | Serial facade restricted to platform; transitive headers hidden and obsolete allowlist removed. |
| 6j | UI-owned logging/remote channels; remaining GRANDFATHERED visibility entries removed. |
| 6k | Portable `ConfigSession` and catalog records replace the legacy config structures and adapter; startup rejection is an intentional correction ([Configuration session](design-notes.md#configuration-session)). |
| 6l | Portable `LoggerModel` with separate ownership of definitions, selection, support and desktop values; chooser and CSV identity corrections carry reproductions and mutation checks ([Logger ownership](design-notes.md#logger-ownership-and-stable-identities)). |
| 6m | Portable `CalibrationSession`, `RomOpenUseCase`, `RomSaveUseCase` and `CalibrationWorkspace` with stable session IDs; save and write correct checksums on a temporary operation image ([session ownership](design-notes.md#calibration-session-ownership-and-operation-images)). |
| 6n | Desktop closure: `DefinitionCatalogSession` replaces `FileActions` (6n-1); kernel models move to `src/backend/flash/kernel` (6n-2); the Qt byte helper moves to `src/platform/desktop/common/bytes` (6n-3). |

“Implemented” in this ledger does not supersede any pending bench checklist.
