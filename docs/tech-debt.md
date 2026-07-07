# FastECU technical debt roadmap

This document tracks project-wide technical debt to improve testability,
legibility, and structure. The target state is:

- 80% automated test coverage for maintained, non-generated application code.
- Clear separation between UI, hardware I/O, protocol logic, and data/model code.
- Smaller units that can be tested without a real ECU, J2534 adapter, serial port,
  GUI dialog, or user home-directory configuration.
- A build/test setup that makes regressions visible in CI before bench testing.

## Current snapshot

Observed on 2026-07-06:

- Qt/qmake C++17 desktop application with app code, serial/J2534 backends,
  protocol code, logging, ECU/TCU/EEPROM flash modules, calibration editing,
  config parsing, and packaging assets in one repository.
- Approximate maintained C++ surface, excluding `tests/`, `hexedit/`, and
  generated `moc_`/`ui_`/`qrc_` files: 255 `.cpp`/`.h` files and about 77k lines.
- Tests exist and are valuable, but are concentrated in newer protocol/logging/
  serial work: about 3.4k lines across 59 `.cpp`/`.h` test files.
- `FastECU.pro` manually lists the full application source set; tests are split
  across several qmake project files in `tests/`.
- CI builds the app on Windows/macOS, runs multiple QtTest binaries, and produces
  an llvm-cov report for SonarCloud. There is no visible coverage threshold or
  ratchet in `.github/workflows/pr.yml`.
- Existing focused debt notes should stay linked rather than duplicated:
  `docs/logging-engine-tech-debt.md` and
  `docs/protocol-generalization-opportunities.md`.
- P0 coverage/build hygiene baseline recorded on 2026-07-07:
  `scripts/coverage-local.sh` builds all maintained test binaries under
  `build/coverage`, writes reports under `coverage/`, and
  `docs/coverage-baseline.txt` stores the current llvm-cov summary used by the
  CI ratchet.

## Priorities

### P0: Make coverage actionable

The 80% target needs a reproducible baseline and a ratchet. Current CI produces
coverage, but the workflow does not fail when total or changed-code coverage
drops.

Actions:

- Add a documented local coverage command that builds and runs all maintained
  test binaries with the same exclusions as CI.
- Publish the current total and per-area coverage baseline in this file or in a
  generated report linked from it.
- Add a CI ratchet: first require "no decrease" for covered lines, then increase
  thresholds by area until 80% is realistic.
- Track coverage by ownership area instead of only total percentage:
  protocol/codecs, logging, serial backend, definition parsing, checksum logic,
  calibration/map editing logic, flash operation orchestration, and UI.
- Keep exclusions explicit: generated Qt files, binary assets, `hexedit/` if it
  remains vendored, and platform packaging outputs.

Implemented baseline:

- Local command: `scripts/coverage-local.sh`
- Ratchet command:
  `scripts/check-coverage-ratchet.sh coverage/coverage-summary.txt docs/coverage-baseline.txt`
- Generated report path: `coverage/llvm-cov.report`
- CI job: SonarCloud now runs the local coverage command and fails if total line
  coverage decreases below `docs/coverage-baseline.txt`.
- Exclusions: `tests/`, `hexedit/`, generated Qt `moc_`/`qrc_`/`ui_` files,
  generated `.moc` files, Qt Remote Objects replica headers, system libraries,
  Homebrew Qt, and Xcode SDK paths.
- Current total line coverage: 25.17% (2,399 covered / 9,531 lines).

Area baseline from the same report:

| Area | Lines | Missed | Line coverage |
| --- | ---: | ---: | ---: |
| protocol/codecs | 574 | 37 | 93.55% |
| logging | 392 | 94 | 76.02% |
| serial backend | 3,351 | 2,148 | 35.90% |
| definition parsing | 3,778 | 3,693 | 2.25% |
| checksum logic | 958 | 958 | 0.00% |
| flash operation orchestration | 366 | 196 | 46.45% |
| calibration/map editing logic | 0 | 0 | not covered by maintained tests yet |
| UI | 0 | 0 | not covered by maintained tests yet |

### P0: Keep generated/build artifacts out of the source tree

The working tree root can accumulate qmake outputs: `Makefile`, `*.o`, `moc_*`,
`ui_*`, `qrc_*`, and replica files. They are ignored by git, but they make source
searches noisy and encourage in-source builds.

Actions:

- Document an out-of-source build directory convention.
- Add a small cleanup command or script for ignored qmake artifacts.
- Prefer CI and local instructions that build under `build/` or another ignored
  directory.
- Keep Sonar and coverage filters aligned with `.gitignore`.

Implemented convention:

- Use `build/<purpose>/` for qmake build directories. Examples:
  `build/pr-app`, `build/tests/mut_dma_tests`, and `build/coverage`.
- Use `coverage/` only for generated local or CI coverage outputs.
- Use `scripts/clean-qmake-artifacts.sh` to remove ignored in-source qmake
  artifacts if an old source-tree build left `Makefile`, object files, generated
  Qt files, replica headers, or app bundles in the repository root or `tests/`.
- CI app, package-check, test, Sonar build-wrapper, and coverage commands now
  build under `build/`.

### P1: Separate UI from application logic

`MainWindow` is the central coordinator and currently owns startup, splash
screens, settings/config loading, menu setup, serial/device setup, logging
engine wiring, calibration file lifecycle, ECU operation dispatch, log views,
status bar updates, and many direct dialogs. `mainwindow.h` includes almost all
flash module headers, and `mainwindow.cpp`/`menu_actions.cpp` are among the
largest files in the project.

Risks:

- Most workflows require a live `QMainWindow` or `QApplication` to test.
- Adding a protocol or flash module tends to touch central UI code.
- Business rules are mixed with widget lookup, message boxes, table selection,
  and config state mutation.

Actions:

- Introduce small application services behind `MainWindow`: protocol selection,
  calibration session management, logging session control, flash operation
  dispatch, and settings persistence.
- Move menu command dispatch out of stringly-typed `if (action == "...")` chains
  into typed actions or a command registry.
- Keep `MainWindow` responsible for widgets and signal/slot wiring only.
- Avoid adding new direct file, protocol, or hardware logic to `MainWindow`.

### P1: Split `FileActions`

`FileActions` derives from `QWidget` but also owns config structures, menu
loading, logger definitions, EcuFlash/RomRaider parsing, ROM open/save,
checksum correction, expression parsing/evaluation, NRC/DTC parsing, and direct
`QFileDialog`/`QMessageBox` usage. Tests that only need expression conversion or
logger parsing are forced to link QtWidgets and often construct a `QApplication`.

Actions:

- Extract pure, non-widget classes first:
  `ExpressionEvaluator`, `NrcParser`, `DtcParser`, `RomRaiderParser`,
  `EcuFlashParser`, `LoggerDefinitionParser`, and `ConfigRepository`.
- Make dialogs and message boxes caller-owned UI behavior, not parser behavior.
- Move nested data structures to standalone model headers.
- Replace the parallel `QStringList` model fields with typed records where
  practical; start with logger channels and protocol definitions.
- Remove `using namespace std` from `file_actions.h`.

Implemented baseline:

- `ExpressionEvaluator` now owns ROM map expression tokenization/evaluation as
  pure, headless logic with focused QtTest coverage.
- `NrcParser` and `DtcParser` now own diagnostic message decoding as pure,
  headless logic with focused QtTest coverage.
- `FileActions` keeps compatibility wrappers for existing ROM open paths.
- `file_actions.h` no longer imports `std` into every includer.

### P1: Stabilize data models and bounds

Several core models are represented as parallel `QStringList`s and raw pointer
arrays, for example `FileActions::ConfigValuesStructure`,
`FileActions::LogValuesStructure`, and `FileActions::EcuCalDefStructure`.
Callers rely on matching list lengths and integer indexes.

Risks:

- Invalid definition/config files can create mismatched list lengths.
- UI and protocol code repeatedly indexes into shared mutable structures.
- Test fixtures are verbose because they must populate unrelated lists.

Actions:

- Add validation functions that check list lengths and required fields directly
  after parsing.
- Convert high-churn structures to typed value objects one area at a time:
  `LoggerChannel`, `ProtocolDefinition`, `CalibrationMap`, `RomDefinition`,
  `FlashDevice`.
- Make parsed models immutable where possible after validation.
- Prefer `std::optional`/explicit result types over returning `NULL` or partially
  filled structs.

Implemented baseline:

- Added validation helpers for flash protocol rows, logger parameters, logger
  switches, and calibration map rows.
- Protocol, logger definition, RomRaider definition, and EcuFlash definition
  parsing now reports mismatched parallel-list lengths and missing required row
  identifiers immediately after parsing.
- RomRaider calibration map rows now append `MapDefined` consistently with
  EcuFlash rows.
- Focused QtTest coverage exercises valid rows, mismatched row lengths, and
  missing required identifiers.

### P1: Extract pure protocol and flash-operation helpers

There are 29 flash/eeprom/jtag/bdm operation pairs under `modules/`. The recent
`FlashOperationWorker` split is useful, but operation classes still combine
request construction, response validation, retries, progress reporting, prompts,
serial I/O calls, and ROM mutation.

Actions:

- Follow the boundary in `docs/protocol-generalization-opportunities.md`:
  extract pure helpers for SSM headers/checksums, seed-key/payload transforms,
  CRC, hex formatting, response validation, and block planning.
- Do not force all ECU families into one shared state machine until a concrete
  protocol proves that is safe.
- For each flash family touched, add scripted transport tests around at least:
  connect/handshake failure, read block success, write confirmation cancel,
  erase/write response rejection, stop request, and checksum mismatch.
- Move user prompts behind injectable prompt interfaces, as
  `FlashOperationWorker::PromptFn` already does.

### P1: Narrow serial and hardware interfaces

`SerialPortActions` is a compatibility facade over a threaded backend and exposes
a very large method surface. `SerialPortActionsDirect` and `J2534_*` contain
platform-specific blocking I/O, adapter configuration, protocol framing, and
diagnostics in large classes.

Actions:

- Keep the existing facade for compatibility, but introduce smaller interfaces
  for new code: `KLineTransport`, `CanTransport`, `SsmTransport`,
  `AdapterDiagnostics`, and `PortConfiguration`.
- Avoid passing the full `SerialPortActions*` into pure protocol code.
- Add lifecycle tests for teardown while calls are in flight, since
  `SerialPortActions` has an explicit destructor precondition.
- Move J2534 constants/types and adapter probing away from higher-level serial
  behavior where possible.

### P1: Make checksum and conversion code headless

Checksum modules and definition/parsing code sometimes show `QMessageBox`
directly. This prevents headless tests from exercising success/failure paths
cleanly.

Actions:

- Return structured checksum results: unchanged, corrected, disabled, invalid
  size, unsupported ROM, and parse error.
- Let UI code decide whether to show an information/warning dialog.
- Add golden-vector tests for every checksum family before changing algorithms.
- Treat `error_codes.h` as data; if practical, generate or load it as structured
  data rather than keeping a 3.7k-line hand-maintained header in normal compile
  paths.

Implemented baseline:

- Added `ChecksumResult` as the shared structured checksum result shape.
- `ChecksumEcuSubaruDensoSH7xxx` now exposes a headless
  `calculate_checksum_result()` API and keeps the older `calculate_checksum()`
  wrapper for compatibility.
- Denso SH7xxx checksum correction reports unchanged, corrected, disabled,
  invalid-size, and parse-error outcomes without showing dialogs directly.
- `FileActions` owns the Denso SH7xxx checksum dialogs for existing UI flows.
- Focused QtTest coverage exercises matching checksums, correction, disabled
  checksums, out-of-range ROM data, and malformed checksum table lengths.

### P2: Improve build structure

The app and tests are managed by hand-maintained qmake file lists. Test binaries
duplicate source lists and link heavy GUI/remoteobjects/websockets dependencies
when only small pure functions are under test.

Actions:

- Create shared `.pri` files for source groups, or move toward a build system
  layout that models libraries explicitly.
- Split reusable code into linkable units such as `core`, `protocol`, `serial`,
  `logging`, `flash`, and `ui`.
- Keep GUI-only dependencies out of lower-level units.
- Rename the broad `tests/tests.pro`/`mut_dma_tests` target once it is no longer
  only MUT/DMA-focused.

### P2: Naming and source organization

Some file names and comments reflect earlier architecture. Example:
`log_operations_ssm.cpp` contains MUT/DMA bench utilities, and several headers
define duplicate `STATUS_SUCCESS`/`STATUS_ERROR` macros.

Actions:

- Move misplaced code into names that match current ownership.
- Replace repeated status macros with typed enums or shared result types.
- Prefer local helper namespaces over global free functions and macros.
- Remove stale commented-out code while touching nearby behavior.

## Testing roadmap toward 80%

1. Coverage infrastructure:
   - One local command for all tests plus coverage.
   - CI coverage report with an enforced ratchet.
   - Baseline coverage recorded by area.

2. Pure logic coverage:
   - Expression parsing/evaluation.
   - NRC/DTC parsing.
   - Config, menu, logger, RomRaider, and EcuFlash parsers using fixture files.
   - Checksum modules with golden vectors.
   - Protocol codecs/builders/parsers with byte-level vectors.

3. I/O and orchestration coverage:
   - Serial facade/backend threading and teardown behavior.
   - Scripted K-Line/CAN/SSM transports for protocol sessions.
   - Flash operation workers with fake serial backends and prompt overrides.

4. UI coverage:
   - Thin Qt widget tests for signal wiring and command dispatch.
   - Avoid using UI tests to reach 80%; most coverage should come from extracted
     logic and orchestration.

## Definition of done for new work

- New protocol/math/parser behavior has unit tests or documented bench-only
  justification.
- New logic can be exercised without constructing `MainWindow`.
- No new pure logic depends directly on `QMessageBox`, `QFileDialog`, or a full
  `SerialPortActions*` unless there is a compatibility reason.
- Generated files and build outputs remain ignored and out of review.
- Any new technical debt is added here or to a narrower existing debt document.
