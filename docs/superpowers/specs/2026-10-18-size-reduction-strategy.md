# Size-reduction strategy

Status: accepted direction. Phase 1 is done (−1,437 NLOC); phase 3's Colt
cancellation ownership change is merged. Explicit CAN and mixed-CAN migration
is implemented under [ADR 0025](../../adr/0025-enforce-cancellation-in-a-guarded-transport.md#per-executor-migration-requirements),
with retained family polls and measured production/test support growth in the
[CAN results](../../reference/can-cancellation-migration-results.md).
Final delivery validation is recorded there. Separate compression assessment
and the Colt bench exit gate remain outstanding. This status update authorizes
no other strategy direction.
Baseline: `136ad0fa` (master, 2026-10-18), 150,494 NLOC (70,746 production,
79,748 test) by `python3 scripts/nloc_loop.py measure`.

## Goal

FastECU must be maintainable by one person working with coding agents. The
metric stays Lizard NLOC as defined in [ADR 0023](../../adr/0023-hill-climb-nloc-with-kind-specific-guards.md),
but the target is the smallest code that keeps every flash family and its wire
behavior. 100k NLOC is a checkpoint, not the finish line.

An earlier revision of this document treated scope, test cases, the remote
serial schema and every legacy quirk as fixed, and asked only how to
deduplicate within them. That framing could not allocate the 50k gap. This
revision changes the constraints instead.

## Decisions

| Decision | Replaces |
| --- | --- |
| Keep all flash families. No family is deleted for lack of hardware. | — |
| FastECU is a hard fork. No compatibility with upstream clients, remote peers or the `.rep` schema. | Preserving deployed remote slots |
| Wire behavior is the contract: bytes, ordering, timeouts and delays, retry counts, address windows, and which failures abort. | Byte-identical legacy behavior everywhere |
| Non-wire deviations may be normalized across families: log text and log points, error wording, progress granularity, helper structure. | Recording them as "transcribed, unverified" |
| Cancellation refusal moves to a guarded transport; typed errors and family continuation policies retain their existing owners ([ADR 0025](../../adr/0025-enforce-cancellation-in-a-guarded-transport.md)). | Bundled cancellation and error normalization in every executor |
| Wire transcripts and plan goldens are the test contract ([ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md)). | ADR 0023's case-count and kill-set guards |
| QHexEdit is first-party code. It stays in `src/ui/desktop/hexedit/qhexedit`, counts toward NLOC and meets the same standards as the rest of the tree. | Moving it to `third_party/` to exclude it from the count |
| SonarCloud, FuzzTest and Mull stay and become merge gates once CI time is acceptable. Other tools stay only if they have recently caught a real defect. | All tooling as fixed cost |

## Where the size is

Measured on the baseline:

| Pool | Production | Test |
| --- | ---: | ---: |
| Flash executors (`src/backend/flash/{ecu,eeprom}/*_executor*`) | 14,153 | 22,550 |
| All of `src/backend/flash/{ecu,eeprom}` | 20,249 | 30,697 |
| Desktop serial (`src/platform/desktop/common/serial`) | 4,117 | 1,761 |
| Desktop flash transports (`.../common/transport`) | 1,510 | 2,214 |
| `mainwindow_test.cpp` | — | 4,725 |
| Remote mode (remote backend, `remote_utility`, websocket, `qtrohelper`, `remote_peer`) | 1,019 | 33 |
| QHexEdit (`src/ui/desktop/hexedit/qhexedit`) | 1,895 | 232 |

The 30 executor suites hold 682 test cases. By name, 193 of them test generic
behavior: 120 cancellation, 45 timeout or empty reply, 30 disconnect, 39 error
propagation (themes overlap). Production flash code mentions `cancel` about
1,100 times (case-sensitive), 60–126 times in each large executor. Test density
ranges from 0.7× production NLOC (Hitachi) to 3.5× (1N83M), so it reflects how
much attention a family received rather than its risk. The shared
[CanExecutorConformance](../../../src/backend/flash/ecu/testing/can_executor_conformance.h)
harness is used by 9 of the 30 suites.

## Phases

Each phase lands as normal PRs, measured before and after on the formatted
tree. A pool is an upper bound on what a phase can touch, not a forecast; the
"expected" column is what each phase must demonstrate in its first PR before
rolling out.

| Phase | Pool | Expected |
| --- | ---: | --- |
| 1. Delete remote mode | ~1.1k + remote branches in callers | Done: −1,437 (1,248 production, 189 test) |
| 2. QHexEdit to first-party standards, and tooling | 2.1k | Measured per PR; may grow tests |
| 3. Colt cancellation ownership, then separate test compression | 1.9k Colt executor + tests | Report initial support cost; assess compression separately |
| 4. Roll out per family | ~35k remaining executor code and tests | Explicit CAN/mixed-CAN ownership implemented; measured growth, no compression claim |
| 5. Thin main-window tests | 4.7k | Measured per coordinator |
| 6. Android | Adds code | Kept small by reusing the Qt UI and portable drivers |

### 1. Delete remote mode

Delete the remote serial backend and its smoke test, `remote_utility`, the
websocket I/O device, `qtrohelper.hpp`, `serial_port_actions.rep`,
`RemotePeer`, the remote-mode branches in `MainWindow` and `AdapterConnection`,
and the Qt Remote Objects and WebSockets modules from the Bazel Qt
configuration (`bazel/qt`, `third_party/qt`, the Windows add-ons patch).

`SerialPortActions` remains: it marshals calls onto the serial I/O thread,
which the direct backend still needs. Its `QRemoteObjectReplica::State` signal
had no direct-mode emitter, so it was removed without a replacement. With one
production backend left,
the 44 getter/setter pairs on `SerialBackend` no longer have a remote schema to
match; narrowing them to cohesive configuration values is follow-up work
within the serial pool. Keep the observed behavior of the direct backend's
setters, including the parity setter that updates stored state before an open
port rejects it, unless a separate change decides otherwise.

### 2. QHexEdit to first-party standards, and tooling

QHexEdit was imported from upstream and has since been modified (#683, #690).
It is treated as our code: it is not moved under `third_party/`, and it is
brought up to the [coding style](../../coding-style.md) and test standards
that apply to the rest of `src/ui/desktop`: clang-tidy clean, naming per the
style guide, comment density matching the surrounding code (the upstream
Doxygen blocks go), and no coverage exemption, so changed lines fall under the
changed-line and SonarCloud new-code coverage gates. There is no whole-file
coverage target. The `hexedit`, `searchdialog` and `optionsdialog` wrappers
meet the same bar.

API that no FastECU caller uses is deleted (unused `Q_PROPERTY` accessors, the
`QHEXEDIT_EXPORTS`/`IMPORTS` macros, example-app leftovers), as are the
translations and icons under `resources/desktop/hexedit/` that nothing loads.
Call sites are verified before each cut.

Phase 2 lands as separate PRs in this order:

1. Retire the NLOC loop (below).
2. Delete unused QHexEdit API and resources.
3. Remove the coverage exemption in `scripts/coverage-local.sh` and test what
   remains.
4. Style pass on what remains.

Deleting first avoids testing or restyling code that is about to go; removing
the exemption before the style pass puts that refactor under coverage.

Tooling audit, from the 30 days before the baseline:

| Tool | Activity | Evidence of defects caught | Proposed |
| --- | --- | --- | --- |
| Bazel build and tests (Linux, macOS, Windows) | Every PR; 33 failed jobs | Build and test failures | Keep |
| clang-tidy | Every PR; 5 failed jobs | Lint findings blocking merge | Keep |
| Sanitizers and fuzz seeds (ASan + UBSan) | Every PR; 1 failed job | One failure | Keep |
| Portable core (Android build) | Every PR; 1 failed job | One failure | Keep; Android ships the backend |
| prek (format, buildifier, ruff, pragma-once, lychee, ...) | Every PR; 2 failed jobs | Formatting and links | Keep |
| Gazelle | Every PR; none failed | Generates BUILD files | Keep |
| SonarCloud | Every PR; 32 failed jobs | Duplication gate, currently non-blocking | Keep; make it a merge gate |
| CodeQL | 137 runs, none failed | 9 alerts fixed all-time, 0 open | Weekly only (#751) |
| Mull mutation testing | Advisory on PRs; loop guard | Loop guard is superseded by ADR 0024 | Keep; make "no new surviving mutants on changed lines" a merge gate |
| FuzzTest workflow | 2 scheduled runs, none failed | None recorded | Keep; make it a merge gate |
| jscpd, Lizard `nloc` hook, `nloc_loop.py`, `hill-climbing-nloc` skill | Manual | Loop-specific | Keep `measure`; retire the loop guards and ledger with ADR 0023 |

SonarCloud, FuzzTest and Mull become blocking merge gates once their CI time
is acceptable. This is a separate track that phase 3 does not wait on. CI
performance work (caching, sharding, narrowing each run to the changed
packages) comes first, and each tool's switch to blocking is its
own PR that records the CI time it adds.

Retiring the NLOC loop deletes the `hill-climbing-nloc` skill, the loop
subcommands, guards and ledger in `nloc_loop.py` (keeping `measure`), the
`jscpd` hook and `.jscpd.json`, and the loop section of
[tooling](../../tooling.md); ADR 0023 becomes superseded by ADR 0024. A GitHub
issue tracks resurrecting the loop later with SonarCloud duplicate detection as
its candidate source, and links the deleting PR so the old loop can be
recovered from history.

For the remaining tools, a failed job is not a defect found; a cut proposal
rests on the absence of any finding another tool would not also report. Each
cut is its own PR that states this evidence. Cutting a tool also deletes its configuration, scripts
and documentation.

### 3. Colt cancellation ownership, then test compression

The [Colt cancellation contract](../../reference/flash-contracts.md#colt-cancellation-ownership)
replaces the original bundled cancellation/transcript pilot. The
[lessons from PRs #756 and #768](../../reference/guarded-transport-pilot-lessons.md)
record the failed size goals, lost abort protection, and harness costs.

The [Colt implementation results](../../reference/colt-cancellation-centralization-results.md)
record the merged ownership change, retained tests, measurements and delivery CI.
Assess test compression in a separate change. Cancellation
ownership may initially add measured support cost; it must not be reported as a
size reduction. The flash contract owns the implemented guard boundaries;
the results retain the boundary inventory, preservation evidence and measurements.
The ownership change requires no transcript
framework, error normalization, or cancellation-deferral mechanism.

### 4. Review before per-family rollout

The expanded [ADR 0025](../../adr/0025-enforce-cancellation-in-a-guarded-transport.md#per-executor-migration-requirements)
sets the requirements for explicit per-executor migration of CAN and mixed-CAN
families. It replaces the blanket Colt compression and bench prerequisites;
those Colt tasks remain outstanding. The explicit CAN/mixed-CAN rollout is
implemented; the [migration results](../../reference/can-cancellation-migration-results.md)
record its audits, preserved checkpoints and measured growth. Review the
recorded net/cumulative cost
before extrapolating savings. Each migration must inventory cancellation
boundaries and retain family abort/continue decisions and wire behavior under the
[sharing decision](../../design-notes.md#share-only-proven-protocol-equivalence).
K-Line decorators, cancellation deferral and removal of UdsClient's checks
remain outside this rollout. Hardware status stays with the
[qualification matrix](../../flash-qualification-matrix.md).

### 5. Thin main-window tests

[mainwindow_test.cpp](../../../src/ui/desktop/widgets/mainwindow_test.cpp)
holds 4,725 NLOC. Where a coordinator owns a rule, test the rule there and
keep window tests only for wiring, visible output, signals and lifetime. Phase 1
already removes the remote-mode cases.

### 6. Android

The Android app has full parity with desktop and works with a wired OpenPort
2.0 and with Bluetooth STN-class adapters (STN11xx/22xx command set). Plain
ELM327 chips are not supported: they cannot set arbitrary K-Line baud rates or
ISO-TP flow control. Parity must not mean a second UI, so:

1. **Spike the toolchain first.** Build the existing Qt Widgets application for
   Android (arm64-v8a, API 29) through Bazel and `rules_qt`. If that fails,
   stop and reopen the UI decision before writing Android code.
2. **Portable adapter drivers.** Move the OpenPort protocol driver out of
   [src/platform/desktop/unix/j2534](../../../src/platform/desktop/unix/j2534)
   into the portable backend without Qt types, and add an STN driver. Both run
   over a platform byte pipe and implement the transport ports the phase 3
   guarded transport uses. macOS and Linux switch to the portable OpenPort
   driver; Windows keeps the vendor J2534 DLL and its 32-bit bridge.
3. **Android platform layer.** Byte pipes for USB serial and BLE, and
   permissions. JNI only where Qt does not cover the platform API.
4. **Parity.** Android runs the same Qt UI. Touch-optimize only the screens
   that prove painful in use.

The versioned C ABI and Kotlin application in the
[modularization plan](../../modularization-plan.md) are dropped.

## Not changed

- Bazel as the build graph, layer visibility, and the portable-core gate. The
  backend stays Qt-free so it is host-testable, even though Android runs Qt.
- Windows support through the vendor J2534 DLL.
- [The caller owns flash transport lifetime](../../adr/0015-caller-owns-flash-transport-lifetime.md).
- Hardware qualification: replaying a transcript is not hardware evidence.
