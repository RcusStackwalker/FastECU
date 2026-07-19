# Step 2 Mechanical Source Organization Design

## Context

This design covers Step 2 of `docs/modularization-plan.md`: mechanically
reorganizing FastECU's production sources and resources without changing
behavior. It targets exact baseline commit
`08e7d1945ff17cf49042160cf0c34d566400d408`, the merged modularization
characterization baseline from PR #46.

Step 2 will be delivered as a merge-ordered sequence of independently green
pull requests. Tests remain under `tests/`; their includes and production
source references change when production files move, but their Bazel labels do
not change.

## Goals

- Create the planned `apps/`, `src/`, and `resources/` ownership layout.
- Separate every flash dialog from its matching `_operation` implementation by
  path ownership.
- Classify protocol algorithms, backend orchestration, and concrete desktop
  transports into their intended layers.
- Preserve behavior, class and file names, public labels, packaging entry
  points, generated Qt header names, and runtime resource identifiers.
- Keep every pull request independently buildable, testable, and mergeable.
- Retain the aggregate root Bazel graph until Step 3.

## Non-goals

Step 2 does not rename classes or files, change APIs or namespaces, decompose
Bazel targets, enforce dependency layering, replace Qt types, remove threading
or dialogs from transitional backend code, add compatibility build systems, or
fix production behavior. Those changes belong to later roadmap steps.

## Chosen Approach

Use a boundary-first hybrid sequence. Establish the destination and resource
layout, move low-coupling algorithm foundations, then move each flash family
as a vertical dialog/operation pair. Finish with the remaining desktop UI,
platform implementations, and a mechanical closeout.

This balances reviewability with early ownership clarity. A layer-at-a-time
series would cut across too many subsystems in each pull request. Mirroring the
legacy directories below `src/` would be mechanically simple but would defer
the classification work and create avoidable second moves in Step 3.

## Ownership Model

Paths document intended ownership during Step 2. They do not yet prove that a
layer is portable or that its dependencies point in the final direction.

### Desktop composition

`apps/desktop/` owns `main.cpp` and, in later phases, dependency composition.
During Step 2 it remains the concrete source of root target `//:fastecu`.

### Desktop UI

`src/ui/desktop/` owns `MainWindow`, dialogs, widgets, Qt Designer forms,
logging presentation, BIU screens, flash dialog classes, and the embedded hex
editor. Each flash class without the `_operation` suffix belongs here because
it owns widget lifecycle, confirmation, and result messages.

### Transitional backend

`src/backend/` owns definition and file workflows, flash `_operation` classes,
flash worker/orchestration, logging engine and protocol coordination,
diagnostic workflows, protocol drivers, and transport interfaces. Existing Qt,
threading, and `SerialPortActions` dependencies are allowed during Step 2.
Removing those dependencies belongs to Steps 4 and 5.

### Algorithms

`src/algorithms/` owns deterministic helpers identifiable by responsibility:
byte/endian utilities, protocol codecs and calculations, expressions, NRC/DTC
parsing, checksums, cipher/key calculations, ROM and calibration calculations,
and flash-planning calculations. Existing Qt types remain unchanged where
present. Placement in this directory expresses destination ownership, not
completed portability.

### Desktop platform

`src/platform/desktop/common/` owns common concrete serial adapters, Qt Remote
Objects, remote utility, WebSocket transport, and OpenSSL-dependent desktop
integration.

`src/platform/desktop/unix/` and `src/platform/desktop/windows/` own
OS-specific J2534 and related implementations. Windows bridge client/protocol
and PE-bitness support belong to the Windows directory.

### Resources

`resources/shared/` owns configurations and kernels.

`resources/desktop/` owns fonts, icons, desktop images, and UI-only hex-editor
assets and translations. QRC aliases preserve every existing runtime resource
identifier despite the physical moves.

## Allowed Changes

Each pull request may contain only:

- `git mv` operations;
- include-path changes;
- root `BUILD.bazel` and `bazel/fastecu_sources.bzl` path changes;
- QRC paths and aliases needed to preserve resource identifiers;
- packaging, CI, coverage, clang-tidy, or helper-script path changes; and
- documentation path updates caused by moved files.

No forwarding headers, symlinks, compatibility copies, or broad include roots
may preserve old paths. Same-directory private includes may remain
basename-only where unambiguous; cross-directory includes use canonical
workspace paths.

## Pull Request Sequence

Every pull request starts from its predecessor after merge and passes the full
Step 2 verification gate before merging.

### PR 1: Destination skeleton and resources

- Create `apps/`, `src/`, and `resources/`.
- Move shared configurations and kernels.
- Move desktop fonts, icons, and images.
- Relocate QRC files and preserve all compiled resource identifiers with
  explicit aliases.
- Move hex-editor assets with the hex-editor UI resource ownership.
- Do not move C++ application sources.

### PR 2: Algorithm foundations

- Move byte/endian helpers, cipher/key calculations, expression evaluation,
  NRC/DTC parsing, checksums, and deterministic calibration/ROM helpers.
- Keep Qt-based implementations unchanged.
- Update all production consumers and tests to canonical paths.

### PR 3: Protocol algorithms

- Move MUT/DMA codecs, memory/freeform encoding, CDBG/Colt protocol encoding,
  and SSM framing/core calculations.
- Leave drivers, orchestration, and concrete serial-backed transports for
  later pull requests.

### PR 4: Shared transitional backend

- Move definition/file workflows, logging engine and protocol orchestration,
  diagnostic workflows, flash helpers/worker, protocol drivers, and transport
  interfaces.
- Preserve current Qt, worker, and transport behavior.

### PRs 5-7: ECU flash families

- PR 5 moves Mitsubishi M32R CAN, Subaru Denso 1N83M 1.5M CAN, Subaru Denso
  1N83M 4M CAN, Subaru Denso MC68HC16Y5-02, Subaru Denso SH7055-02, and Subaru
  Denso SH7058 CAN Diesel.
- PR 6 moves Subaru Denso SH7058 CAN, Subaru Denso SH705x DensoCAN, Subaru
  Denso SH705x K-Line, Subaru Denso SH72531 CAN, Subaru Denso SH72543 CAN
  Diesel, and Subaru Hitachi M32R CAN.
- PR 7 moves Subaru Hitachi M32R K-Line, Subaru Hitachi SH7058 CAN, Subaru
  Hitachi SH72543R CAN, Subaru Mitsubishi M32R K-Line, Subaru Unisia JECS, and
  Subaru Unisia JECS M32R.
- Move each dialog class to `src/ui/desktop/flash/ecu/` and the matching
  `_operation` class to `src/backend/flash/ecu/`.
- Never split a pair across pull requests. A family without an `_operation`
  file moves intact to the UI side because its current implementation owns the
  dialog behavior.

### PR 8: Other flash families

- Move TCU, EEPROM, BDM, JTAG, and boot-mode families as intact pairs into
  corresponding UI and backend subdirectories.
- If the group exceeds 50 moved source/form files, split it at a
  subsystem boundary. Never split a dialog/operation pair.

### PR 9: BIU and remaining desktop UI

- Move BIU screens, `MainWindow`, selection/settings/data-terminal dialogs,
  calibration widgets, logging presentation, and the embedded hex editor.
- Move `main.cpp` to `apps/desktop/`.

### PR 10: Desktop platform

- Move concrete CAN/K-Line/SSM adapters, common serial implementations, remote
  objects, WebSocket/remote utility, J2534, PE-bitness, and bridge support.
- Preserve all standalone target names and existing platform compatibility
  constraints.

### PR 11: Mechanical closeout

- Move any classified stragglers.
- Update packaging, scripts, clang-tidy/coverage inputs, and documentation that
  still names an old path.
- Assert that legacy production locations are empty.
- Retain `bazel/fastecu_sources.bzl`, `//:fastecu_core_common`, the root
  platform libraries, and concrete `//:fastecu`; replacing them is Step 3.

The pull request counts are an expected decomposition, not a reason to make an
oversized change. An unusually coupled family moves with all of its directly
required mechanical path updates. Only subsystem or family boundaries may be
used for additional splits.

## Build and Include Mechanics

The root `BUILD.bazel` remains the application composition package.
`bazel/fastecu_sources.bzl` continues to enumerate application and shared test
sources.

- Application source entries use workspace-relative destinations such as
  `src/backend/...`.
- Lists consumed from `//tests` refer to moved production files with paths such
  as `../src/...`.
- Existing test rule names and labels remain unchanged.
- `//:fastecu`, `//:fastecu_core_common`,
  `//:fastecu_platform_unix`, and `//:fastecu_platform_windows` retain their
  roles.
- Standalone J2534 bridge and PE-bitness target labels remain unchanged.
- Cross-directory C++ includes use canonical paths such as
  `src/algorithms/protocol/bytes.h`.
- Generated Qt headers retain existing basenames, including `ui_*.h` and
  replica headers.

No package-owned production `BUILD.bazel` files are introduced in Step 2.
Their creation and the removal of the central source manifest belong to Step
3.

## Resource and Packaging Mechanics

Physical assets move to `resources/shared` or `resources/desktop`. QRC files
use explicit aliases so the compiled identifiers used by the application stay
identical. Bazel resource rules point at the new QRC and physical paths.

Packaging continues to consume `//:fastecu`. Application names, executable
names, bundle layout, icons, and package entry points remain unchanged. A
resource move is incomplete if the physical path is correct but its runtime
identifier differs.

## Failure Handling

There is no compatibility fallback for a failed move. Fix incorrect include,
manifest, generated-source, QRC, script, or packaging paths before merge. If a
file cannot move independently, move the complete inseparable family in the
same pull request.

Do not retain an old-path shim. Such a shim would hide incomplete ownership and
make the Step 2 completion check unreliable.

## Verification

Run these local gates for every pull request:

```sh
bazel build -k --config=release //:fastecu //tests/...
bazel test -k --config=release //tests/... //:bazel_openssl_wiring
```

Also require:

- all 13 preserved headless GoogleTest labels;
- the complete applicable host test suite, with only existing incompatible
  platform targets skipped;
- a stale-reference scan for every old location moved in the pull request;
- a manifest audit confirming that each moved production file occurs exactly
  where expected;
- `git diff --summary` and rename review confirming no file was silently
  duplicated;
- a semantic-diff audit confirming non-path source lines did not change; and
- unchanged Bazel test labels.

Resource pull requests additionally compare the full set of QRC runtime
identifiers before and after the move. Flash-family pull requests run relevant
characterized checksum, codec, flash-planning, ROM, and pre-I/O tests. Platform
pull requests run serial, J2534, and remote tests on every applicable OS.

CI is the authoritative cross-platform gate and must retain:

- Windows, macOS, and Linux release build/test jobs;
- Windows and macOS package verification;
- pre-commit and coverage/SonarCloud checks; and
- the existing non-blocking clang-tidy reporting path.

Real ECU bench testing is not required for path-only pull requests. Existing
bench checklists remain unchanged and become mandatory if a supposedly
mechanical edit is found to affect runtime behavior.

## Completion Criteria

Step 2 is complete when:

- all production sources live under `apps/desktop` or their intended `src`
  ownership tree;
- shared and desktop assets live under `resources`;
- every flash dialog and matching operation is on its intended side;
- legacy `modules/`, `protocol/`, `logging/`, `serial_port/`,
  `remote_utility/`, `hexedit/`, and root application-source locations contain
  no production files;
- public class names, file names, test labels, application target labels,
  generated Qt header names, runtime resource identifiers, and packaging entry
  points are unchanged;
- the aggregate root Bazel graph builds and tests on Windows, macOS, and Linux;
  and
- the series contains no portability conversion, dependency cleanup, API
  change, or behavior refactor.
