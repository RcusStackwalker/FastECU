# FastECU Modularization and Android-Readiness Plan

## Current State and Destination

Reviewed against revision `a41e3a51` on 2026-09-27. Steps 1-5 and 6a-6j
are implemented. Step 6 remains open because desktop consumers still depend
on Qt-typed parallel-list models and `FileActions`; step 7 has not started.
Implementation completion and hardware qualification are separate statuses.

The portable algorithms and backend workflows already exist: configuration,
definitions, calibration and map editing, checksums, logging, diagnostics,
service functions, and the retained flash families. The remaining work is to
migrate desktop consumers onto those APIs and retire their legacy bridges,
not to extract the same parsers and workflows again.

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

### Remaining structural debt

- `FileActions` remains in the legacy `src/backend/definitions` package,
  distinct from portable `src/backend/definition`. Its logging and
  calibration structures already have standalone headers but
  remain Qt-typed and are exposed through historical `FileActions` aliases.
- Three bridges remain: `LegacyLoggerAdapter`, `LegacyDefinitionAdapter`,
  and `LegacyCalibrationAdapter`. Desktop code
  still consumes their parallel lists and shared mutable state.
- `MainWindow` retains calibration-slot ownership and portions of write
  preflight, checksum correction, and post-read calibration handoff. Logging
  selection, connection orchestration, and log-file handling also retain UI
  coordination; presentation-only coordination need not become a portable port.
- One algorithms-side Qt shim remains:
  `src/algorithms/protocol/qt_compat`, containing byte-container conversions.
  Those conversions are still needed at desktop boundaries. The SSM shim is
  gone.
- Android build configuration, native facade, and smoke fixture do not exist.

The destination remains a reusable, Qt-, JNI-, and OS-independent algorithms
and backend core, with desktop presentation and adapters outside it. Finish
the desktop model and bridge migrations before starting the Android seam.
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
  See the [protocol-sharing boundary](protocol-generalization-opportunities.md)
  and [flash qualification matrix](flash-qualification-matrix.md).
- **Enforce boundaries through the build graph.** Qt reachability is gated by
  [ADR 0016 visibility](adr/0016-enforce-qt-reachability-by-visibility.md).
  Remove transitional entries from `qt_layer` as consumers migrate; never add
  new ones. `implementation_deps` keeps facade headers out of production UI
  compile inputs, enforced by sandboxed Linux/macOS builds. Windows alone
  does not prove that header boundary.
- **Register portable roots explicitly.** `//:portable_closure` is a build-time
  check for reachable `//src/platform` labels, not a Qt/JNI source scan.
  [The registry](../bazel/portable_targets.bzl) names individual targets;
  merely putting a new library in a listed package does not cover it.

## Remaining Roadmap

Steps 6l-6n are ordered consumer migrations. Each slice should land with its
own regression coverage and remove the bridge it makes unnecessary. Reuse
existing portable records and services; extend them only where a consumer
requires a missing capability. Preserve behavior unless a correction is
explicitly evidenced and recorded.

### 6k — Configuration and protocol models

Done: startup, settings persistence, vehicle/protocol selection, and composition
wiring now use the portable `ConfigSession` and catalog records.
`ConfigValuesStructure`, `LegacyConfigAdapter`, and `legacy_config_paths` have
been removed. Startup rejection is an intentional correction, documented in
[Configuration session](design-notes.md#configuration-session), together with
the preserved selection and persistence quirks. Step 6n remains.

### 6l — Logging models

Implemented in two slices: 6l-1 adds the portable `LoggerModel`, support-aware
defaults and explicit selection fallback; 6l-2 migrates desktop composition,
chooser, displays, CSV, snapshots and sample application. `LogValuesStructure`,
`LegacyLoggerAdapter`, logger-specific `FileActions` code and the logging legacy
Qt visibility grant are removed. Definition defaults, operator choices, support
and desktop values have separate ownership.

Automated coverage includes selection persistence/failures, nonfatal definition
loads, capability preservation, stable identities, unresolved IDs, raw-value and
conversion compatibility, immutable snapshots and logging/connection lifecycles.
The chooser's duplicate-label correction and CSV's protocol-scoped identity
correction have reproductions, exact corrected assertions and mutation checks;
see [Logger ownership and stable identities](design-notes.md#logger-ownership-and-stable-identities).
Backend policy stays in `LoggingUseCase::run()`. Platform CI/packaging gates and
hardware bench qualification require their recorded results before release;
automated success alone does not establish hardware qualification.

### 6m — Definition and calibration sessions

Implemented. Portable `CalibrationSession`, `RomOpenUseCase`,
`RomSaveUseCase` and `CalibrationWorkspace` own calibration data through the
composition root. Trees, ROM info, map windows, edits, hex display, save,
write preflight and checksum use sessions directly. The final consumer slice
also reads typed map metadata directly; no read-only legacy projection remains.
`EcuCalDefStructure`, both legacy definition/calibration adapters and their
packages, legacy columns and the two `qt_layer` entries are retired.

Stable session IDs replace fixed raw-pointer slots. UI view state stays keyed
by session; map values decode from the current ROM bytes. Save and write use a
temporary operation image for checksum correction, leaving editable session
bytes unchanged on completion or cancellation. See
[calibration session ownership and operation images](design-notes.md#calibration-session-ownership-and-operation-images)
for lifetime rules and preserved behavior.

The exit requirements remain definition resolution and inheritance, ROM round
trips, map addressing and edits, checksum outcomes, and failed/cancelled reads
leaving no occupied session. Preserve the documented
[calibration corrections and open defects](design-notes.md#calibration).
Full release build/test, formatting, changed-file static analysis and platform
CI/packaging gates require their recorded results. Post-read handoff and
checksum paths still need bench re-verification before release; this migration
establishes no new hardware qualification. `FileActions` and the remaining
legacy package closure belong to step 6n.

### 6n — Desktop closure

Remove `FileActions` once its consumers use the extracted services directly.
Wire those services through desktop composition. Retain the portable kernel
constants/models currently in `src/backend/definitions:models`, moving them
to appropriate portable ownership before retiring the legacy package.
Remove remaining compatibility glue and obsolete checks tied to deleted
files as their callers disappear.

Move the byte-conversion helper out of algorithms into a desktop boundary
package with deliberate UI-facing visibility. Preserve explicit conversions
and their tests; retiring a shim does not mean duplicating conversion code
or forcing Qt-owned buffers out of the UI and platform layers.

**Exit gate:** no Qt-typed legacy package remains under algorithms/backend;
all corresponding transitional `qt_layer` entries are removed, and portable
closure coverage includes the resulting targets. Production UI still cannot
reach serial facade headers. Desktop composition, restart, cancellation,
teardown, and packaging checks pass. Any outstanding hardware qualification
is recorded separately rather than implied by this structural completion.

### 7 — Android seam

Start after 6n. First prove the portable closure can cross-compile with an
Android C++23 toolchain. Validate and pin compatible Bazel Android rules and
NDK versions at this milestone; the original prospective version pins were
not an implemented or verified toolchain. Keep Android dependencies isolated
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

The build includes `//:portable_closure`. Qt restrictions are enforced by
visibility, not the deleted `backend_no_widgets` or serial allowlist scans.
The deleted OpenSSL wiring target is not a gate.

Require the Windows/macOS/Linux CI matrix and Windows/macOS packaging checks.
Coverage is gated through SonarCloud on new code; the former overall coverage
baseline ratchet is gone. Follow the [coding style and testing conventions](coding-style.md)
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
broader cleanup; the [logging debt notes](logging-engine-tech-debt.md) and
[protocol-sharing notes](protocol-generalization-opportunities.md) hold focused
follow-ups. Their historical snapshots can lag code: verify a finding before
scheduling it. For example, logger parsing is already portable, and CDBG
serial setup is already in desktop protocol registration rather than the
portable protocol's `start()`.

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

“Implemented” in this ledger does not supersede any pending bench checklist.
