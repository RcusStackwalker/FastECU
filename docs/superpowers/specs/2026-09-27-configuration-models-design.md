# Step 6k — Portable configuration session

## Scope

Replace desktop configuration and protocol parallel lists with one portable
`fastecu::config::ConfigSession`, owned by `DesktopComposition`. Migrate the
configuration inputs of retained logging, definition, and calibration code
without changing their workflows. Completion removes `ConfigValuesStructure`,
`LegacyConfigAdapter`, and `legacy_config_paths` together; no synchronized
compatibility copy remains.

Logging models, calibration session ownership, and final `FileActions` removal
remain in steps 6l–6n. No wire behavior, hardware support, XML schema, software
version, or packaging layout changes belong in this slice.

## Portable ownership and interface

Add `config_session.h/.cpp` and focused tests under `src/backend/config`.
The session reuses `AppConfig`, `ConfigPaths`, the provisioning and catalog
loaders, and `ResolvedCarModel`. Inject `IFileSystem`, `IResourceBundle`,
`IFileRepository`, and `IEventSink`; introduce no Qt, threads, direct filesystem
access, or desktop platform detection.

The public interface provides initialization and saving with explicit `Status`
results, mutable/const settings access, effective paths by value, read-only
vehicle records, checked selected-row access, checked selection by row, and
selection by protocol name reporting whether a match exists. Access before
successful initialization cannot index an empty catalog. An invalid explicit
row selection fails without changing settings.

`AppConfig.selected_protocol_id` remains the authoritative saved row identifier.
Do not maintain a second selected index or cached make/model/MCU/checksum/
capabilities/description. Derive these from the selected `ResolvedCarModel`.
Vehicle records remain in file order, with numeric IDs equal to their original
zero-based positions. Choosers sort their presentation without reordering the
catalog or changing IDs.

Store provisioned paths separately from settings. Effective paths start from
the provisioned paths and take current configurable calibration and datalog
directories from settings. Editing either directory must not relocate config,
kernel, definition-resource, or syslog files. Definition search directories
and file lists remain settings, distinct from provisioned resource locations.

## Loading, defaults, and persistence

Composition supplies the application root and existing application metadata:
name/title `FastECU`, version `0.1.0-beta.5`. Preserve Unix/macOS
`~/.config/FastECU/` and Windows `~/AppData/Local/FastECU/`, including custom
root injection used by tests.

Initialization provisions directories, loads settings, loads both catalogs,
resolves vehicles, and validates the selected ID before exposing consumers.
Preserve prior-version migration, bundled-resource copying, and syslog pruning.
Reject an empty vehicle catalog. Invalid saved IDs select row zero, including
negative, malformed, overflowing, and out-of-range values.

Apply compiled-in defaults in the session: window dimensions `default`, toolbar
icon size `32`, serial port `ttyUSB0`, primary definition base `ecuflash`, both
definition enablement flags `disabled`, and provisioned calibration/datalog
directories. Other absent settings retain their existing empty defaults.
`AppConfig` itself keeps its empty-value parser contract. Nonempty loaded scalar
values override defaults; loaded file lists retain their existing semantics.

Preserve trailing-slash normalization for calibration, EcuFlash definition,
and datalog directories, including existing acceptance of a trailing backslash.
Successful saving updates the in-memory settings with the normalized result.
A failed save retains the user's edits and returns an error including path and
reason. Settings reports that error to the operator.

The existing loader rewrites configuration and discards the write result.
The session must observe normalization-write failures without changing the
parser's returned empty-value/unnormalized-value contract. Implementation must
provide an observable load/write path rather than relying on the current
fire-and-forget rewrite. Successful loading followed by a failed normalization
write is nonfatal and emits a warning; it must not become a startup rejection.

Preserve the known XML mismatch: the writer emits `logfiles_directory`, while
the reader recognizes `datalog_files_directory`. This setting consequently
does not round-trip. Tests must pin the exception rather than fix it incidentally.

## Selection behavior

Startup restores the saved flash transport, log transport, and logging protocol;
restoring the selected row does not replace those choices with protocol defaults.
An explicit vehicle or protocol selection changes the saved row and logging
protocol using the existing rules. Toolbar transport selection behavior stays
unchanged.

Protocol-name selection uses the last matching vehicle row, as the existing
ROM-open scan does. An unmatched ROM protocol leaves all settings unchanged and
reports no match. The protocol chooser remains vehicle-backed, including shared
protocol entries, rather than becoming a separate list of protocol definitions.

Unresolved protocol references remain `std::nullopt` in vehicle records. At
legacy/display boundaries, missing protocol fields use the existing single-space
placeholder. Missing read/test-write/write capabilities remain unavailable.
Do not synthesize a protocol record to make an unresolved reference look valid.

Vehicle and protocol dialogs hold tentative choices locally. Acceptance applies
the choice to the shared session; cancellation leaves it unchanged. Settings
continues to edit live settings and save on close, retaining its destruction
fallback. Do not change Settings into a transactional accept/cancel dialog.

## Desktop composition and startup

`DesktopComposition` owns the session before its consumers and initializes it
before constructing `MainWindow`. It passes the same session through
`MainWindowServices` and into `FileActions`. Required configuration failures
prevent consumer startup; startup rejection performs no ECU I/O.

Provisioning failures and required configuration/catalog load failures show the
failing path and reason, then exit cleanly before creating `MainWindow`.
Diagnostics must have a visible destination before a window or syslogger thread
exists. The desktop startup path presents the failure; portable code returns
status and emits events rather than displaying dialogs. Nonfatal warnings must
also remain visible before the window exists.

Construction and teardown must handle failed initialization without dereferencing
uncreated services or threads. Restart creates a fresh composition/session and
destroys dependent services before their configuration and I/O dependencies.
Keep serial facade headers isolated from production UI code.

This changes the old failure behavior intentionally: `LegacyConfigAdapter`
ignored provisioning/load errors and startup could proceed into empty lists.
Document this correction in `docs/design-notes.md`, add reproducing tests, and
mutation-check the startup rejection guard to prove the tests fail when broken.

## Retained legacy workflows

Migrate startup, Settings, vehicle/protocol dialogs, menus, logging configuration
and snapshots, definition lookup/authoring, and calibration configuration inputs
as one consumer slice. Lower-level adapters receive only needed settings or
paths, not the entire session. Remove the calibration adapter's duplicate catalog
loading, protocol-binding cache, and selection copies; ROM-triggered selection
goes through the shared session.

Move the eight existing EcuFlash/RomRaider definition-index lists into a dedicated
`FileActions`-owned legacy structure: calibration IDs, calibration-ID addresses,
ECU IDs, and filenames for each definition format. Preserve index contents,
ordering, lookup, and authoring semantics. These indexes are not application
settings and do not belong in `ConfigSession`.

Retire old configuration validation tied to parallel-list lengths, the legacy
configuration package and its `qt_layer` entry, and unused build dependencies.
Register the portable session target explicitly in `portable_targets.bzl` and
grant only the composition visibility needed in addition to legitimate consumers.

## Delivery and verification

1. **6k-1: Portable session.** Implement the session with tests first. Transfer
   meaningful bridge regressions before removing their old tests: defaults,
   custom roots, provisioning/migration, normalized paths, file round trips and
   the datalog exception, invalid IDs, shared protocols, unresolved references,
   unmatched ROM selection, and error/warning distinctions.
2. **6k-2: Consumer migration and retirement.** Switch all consumers together,
   delete the old model and adapters without an interim synchronized mirror, and
   update `docs/modularization-plan.md` and superseded configuration notes.
3. Add desktop tests for dialog acceptance/cancellation, live Settings edits and
   persistence failures, startup rejection and visible diagnostics, shared session
   ownership, ROM-triggered selection, logging snapshots, restart, and teardown.
   Failed startup must produce zero ECU I/O. Mutation-check the intentional
   startup correction and restore production code afterward.
4. Run focused tests and then:

   ```sh
   bazel build -k --config=release //...
   bazel test -k --config=release //...
   bazel run --config=release //:clang_tidy_report_changed
   ```

   Windows/macOS/Linux CI and existing packaging checks are required release
   gates. Preserve portable-closure enforcement and serial-header isolation.
   Report any CI unavailable locally as pending, not passing. Hardware
   qualification remains a separate gate.

## Review status

This written design records the supplied plan against the current implementation.
Self-review checked scope, ownership, compatibility, failure semantics, and test
coverage. Written-spec review precedes the detailed implementation plan and its
review, as requested. Implementation has not started.
