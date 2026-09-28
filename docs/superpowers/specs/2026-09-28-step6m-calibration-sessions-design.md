# Step 6m — Definition and calibration sessions

## Scope

Replace the desktop's `EcuCalDefStructure` parallel-list model and its fixed
raw-pointer slot array with portable, explicitly owned calibration sessions.
Migrate ROM open, definition lookup, map display and editing, save, write
preflight, checksum correction, and the post-read handoff onto them.
Completion removes `EcuCalDefStructure`, `LegacyDefinitionAdapter`,
`LegacyCalibrationAdapter`, `legacy_definition_columns.h`, and the
`//src/backend/calibration/legacy` and `//src/backend/definition/legacy`
entries from the `qt_layer` ratchet.

`FileActions` itself, the rest of `src/backend/definitions`, the kernel
models, and the byte-conversion shim remain for step 6n. No wire behavior,
definition XML schema, ROM file format, or hardware support changes belong in
this step. Corrections are out of scope unless separately evidenced; every
legacy quirk named below is preserved and pinned by a test.

The step lands as three stacked pull requests (`gh stack`):

| Slice | Content | Legacy model after the slice |
|---|---|---|
| 6m-1 | Portable session, workspace, and ROM-open use case; composition wiring | Unchanged; still used by every consumer |
| 6m-2 | Ownership and identity move to the workspace; open, ECU-read adoption and close go through it | One legacy view per session, built once at open, owned beside it |
| 6m-3 | Tree, map windows and every mutation onto the session; legacy model, adapters, and ratchet entries retired | Deleted |

## Decisions

- **ROM bytes are the only truth for map values.** The session holds no
  decoded-value cache. A view decodes a map on demand; an edit writes bytes
  only and the view re-decodes. The legacy `"v1,v2,...,"` text is a
  presentation format rebuilt in the UI where still needed.
- **The desktop composition owns the workspace.** `DesktopComposition`
  constructs a `CalibrationWorkspace` and passes it to `MainWindow` through
  `MainWindowServices`, following `LoggerModel`. The composition outlives the
  window, as for every other long-lived service.
- **Sessions have stable identities.** A `SessionId` is never reused within a
  workspace. UI rows and map windows carry it instead of a slot position.
- **A session may lack a definition.** "Continue without definition file" is a
  modeled state (`std::optional` definition), not placeholder list entries.

## 6m-1 — Portable session model

Add a new package `src/backend/calibration/session` with explicit `srcs` and
co-located tests. Register every new `cc_library` by name in
`bazel/portable_targets.bzl`. No Qt, threads, or direct filesystem access.

### `CalibrationSession`

A movable value owning:

- `SessionId id`.
- `RomSource source`: display filename, full path, and origin (file or ECU
  read).
- `std::vector<std::uint8_t> rom`: the image after flash-method padding.
- `std::optional<ResolvedDefinition> definition`: format, definition ID, and
  `definition::RomDefinition`. A definition whose size validation failed is
  retained as its header only, with no maps (see preserved behavior).
- `RomProtocolInfo protocol`: flash method after alias resolution, checksum
  module label, MCU type, kernel path, kernel start address, ROM ID, and the
  file-size label.
- `bool dirty`. The legacy `SyncedWithEcu` and `OemEcuFile` flags have no
  reader and are not carried over.

Interface: read-only accessors for all of the above;
`Result<MapCellValues> decode_map(std::size_t map_index) const`, delegating to
the existing `calibration::compute_map_cell_values` logic for one map and
failing with `ErrorKind` on an out-of-range index or a session without a
definition; `Status write_bytes(std::uint64_t offset, bytes::ByteView data)`,
rejecting any write that does not lie wholly within `rom` and setting `dirty`
on success; and a setter for `RomProtocolInfo`. 6m-1 adds `write_bytes` with
tests but no production caller.

`compute_map_cell_values` gains a single-map entry point shared with the
existing whole-definition function, so both produce identical text. Existing
golden tests for the whole-definition function must still pass unchanged.

### `CalibrationWorkspace`

Owns sessions in open order. Operations:

- `Result<SessionId> open_file(std::string_view path)`
- `Result<SessionId> adopt_read_image(ReadImage)` — bytes, ROM ID, and the
  protocol selected at read time.
- `Status close(SessionId)`
- `CalibrationSession *find(SessionId)` and a `const` overload; `nullptr` for
  an unknown or closed ID.
- `std::vector<SessionId> ids() const` in open order. Sessions are held by
  `std::unique_ptr`, so pointers from `find` stay valid until that session
  closes.

A session is inserted only after its open or adoption succeeds. A failed open,
a failed read, and a cancelled read leave the workspace unchanged. The legacy
capacity of 100 is not preserved as a limit.

### `RomOpenUseCase`

Moves the logic of `FileActions::open_subaru_rom_file` onto typed inputs and
outputs. Dependencies: `definition::DefinitionService`, `IFileRepository`,
`IFileSystem`, `IEventSink`, the portable `config::ConfigSession`, and a new
narrow port, `IDefinitionCatalogs` (`catalog(DefinitionFormat)`, plus
`indexed_source(DefinitionFormat, id)` so a definition that became
unreadable after indexing still reaches the legacy load-failure notice).
`FileActions` implements that port with its existing `build_definition_catalog`,
so catalog sources, including definitions authored this session, are
unchanged; step 6n replaces the implementation. Sequence, in the legacy
order:

1. Read the ROM (`calibration::read_rom`), or for an ECU image back it up to
   `<calibration_files_directory>/read.bin` (`calibration::backup_rom`). The
   caller names the image, as `MainWindow` already does with
   `flash::read_image_filename`. An empty name is rejected; the legacy
   timestamp fallback was unreachable and would need a clock port.
2. Match against the primary then secondary catalog with the legacy settings
   precedence: `primary_definition_base`, the two `use_*_definitions` flags,
   and the EcuFlash directory / RomRaider file list, via
   `DefinitionService::match_rom` and `load`. When matching fails, the
   previous ROM ID (for an ECU read, the one the ECU reported) is still looked
   up in the catalog and loaded, as legacy did.
3. Strip `0x` address prefixes and resolve the flash-method alias against
   the vehicle catalog.
4. Select the vehicle by protocol name in `ConfigSession`.
5. Derive the checksum-module label and MCU type from the selected vehicle.
6. Record the file-size label from the unpadded length, then apply
   flash-method padding.
7. Validate ROM size against the resolved definition.

The result reports the session contents, whether a definition was found, and
whether the vehicle selection changed.

### Preserved behavior pinned by 6m-1 tests

- Opening a ROM changes the selected vehicle when its flash method names a
  catalog protocol, and changes nothing otherwise.
- A size-validation failure still opens the ROM, with the definition's maps
  emptied (legacy `NameList.clear()`), and emits the existing notice.
- A definition file that no longer exists opens the ROM without a
  definition, logs the load failure, and shows the existing
  "Unable to open ECU definition file" notice. The legacy "no resolved
  definition" warning becomes unreachable, because the use case holds the
  definition it loaded.
- The file-size label is `<unpadded length / 1024>kb`.
- The checksum-module label: `checksum<method minus its first three
  characters>` for `yes`, `Not implemented yet` for `n/a`, `No checksums` for
  `no`.
- An empty filename for a disk open is an error, as is an empty ECU-image
  filename or image.
- `x_axis_data` / `y_axis_data` keep their `" "` default for an absent axis.

6m-1 wires the workspace into `DesktopComposition`. `MainWindowServices`
gains the field in 6m-2, where `MainWindow` first uses it. No consumer calls
the workspace yet, and `FileActions` keeps its legacy open path.

## 6m-2 — Ownership and identity

The original split (read-only consumers in 6m-2, mutation in 6m-3) does not
survive the code. Map windows re-render from the text the edit paths patch, the
selectable and switch handlers write bytes directly, and `HexEdit` and the map
windows keep their struct for their whole lifetime. A per-call projection would
therefore be a long-lived second byte store. 6m-2 instead moves ownership and
identity only, and leaves every reader and writer on one legacy view.

- `MainWindowServices` carries the `CalibrationWorkspace`. Remove
  `std::array<EcuCalDefStructure *, 100> ecuCalDef` and `ecuCalDefIndex` from
  `MainWindow`. It owns, per open session and in files-tree order, the
  `SessionId` and one `std::unique_ptr<EcuCalDefStructure>` legacy view.
- The legacy view is built once, when the session opens, by a UI-side
  projection that reproduces what `FileActions::open_subaru_rom_file` left in
  the slot (golden-equivalence tested against it). Until 6m-3 the view is the
  only byte store the UI reads or writes. Nothing reads the session's bytes
  after the projection, and 6m-3 reverses that ownership.
- Calibration-file tree rows and map sub-window names carry the `SessionId`.
  Every lookup is by ID, and the close-time renumbering is removed. A window
  or row whose session has closed finds nothing and does nothing.
- File open and the post-read handoff call the workspace.
  `adopt_read_image` is called only after a successful read, so the
  pre-created slot and its failure-path `delete` disappear.
  `prompt_for_missing_definition` runs when the new session has no definition.
  The dialog stays in the UI. Create and import keep their current behavior:
  they write a definition file and do not re-open the ROM, which remains
  definition-less until reopened. "Continue without" or rejecting the dialog
  keeps the definition-less session and shows the legacy placeholder ROM info
  (XML ID `UnknownID`; empty internal ID address, internal ID string, and ECU
  ID; make from the selected vehicle; definition file blank) only on that
  path.
- Closing a ROM closes its session and frees its legacy view, which legacy
  leaked.
- `HexEdit` takes the image bytes and file name by value instead of keeping
  the struct.

## 6m-3 — Rendering, mutation and retirement

- **Tree and view state.** `CalibrationTreeWidget` builds from the session's
  definition and protocol info. Which map windows are open and which
  categories are expanded (legacy `VisibleList`, `CategoryExpandedList`,
  `RomInfoExpanded`) move to UI view state keyed by `SessionId`.
- **Map windows.** `CalibrationMaps` and `set_maptablewidget_items` render
  from `decode_map`. A UI helper converts the typed result into cell text,
  preserving current formatting, precision, and min/max cell coloring.
- **Switch and selectable handlers** write through `write_bytes`, keeping the
  `wrx02` address adjustment unchanged (defect (a) stays open).
- `DefHeaderStrings` / `DefHeaderNames` move to the authoring dialog as UI
  constants.
- **Map edits.** `map_edit_adapter` stops patching the text columns and calls
  `write_bytes`; the view re-decodes. The selectable-map path (legacy direct
  write into `MapData`) writes the selection's byte value. A characterization
  test pins the displayed result before the change.
- **Save.** A portable `save_rom(const CalibrationSession&, path)` preserves
  the existing Subaru save behavior. A failed write keeps its error-level log
  and operator notice, and success clears `dirty`.
- **Write preflight and checksum.** Protocol-info refresh from the selected
  vehicle moves onto `RomProtocolInfo`. Checksum correction keeps its legacy
  semantics: the corrected image is what gets written or saved, and the open
  ROM's bytes are restored afterwards, whether the operator cancelled or the
  write or save completed (`start_ecu_operations` and both save paths restore
  their pre-correction copy). A test pins this. The `n/a` warning and
  confirmation stay in the UI.
- **Retirement.** Delete `EcuCalDefStructure`, both legacy adapters and their
  packages, `legacy_definition_columns.h`, the ROM open/save and
  definition-column code in `FileActions`, the legacy view projection, and
  tests covering only deleted code. Remove the two `qt_layer` entries.
  `checksum_selection.h` and `map_edit.h` comments naming the legacy struct are
  updated.

**Exit gate.** No consumer requires `EcuCalDefStructure`,
`LegacyDefinitionAdapter`, or `LegacyCalibrationAdapter`, and neither ratchet
entry remains. Tests cover definition resolution and inheritance, ROM open and
save round trips, map addressing and edits (including striding and every
storage type already covered by `map_edit_test`), checksum outcomes, and
failed or cancelled reads leaving no session.

## Errors

Backend operations return `Result<T>` / `Status`, checked with `has_value()`.
Operator-facing notices keep their current text and remain emitted through
`IEventSink`. No new `ErrorKind` values. `find` returning `nullptr` for a
stale ID is an expected outcome, and a UI path holding a closed session's ID
does nothing.

## Testing

- Portable gtests (`fastecu_portable_gtest`) for the session, workspace, and
  open use case, using package-owned fakes of `IFileRepository` and
  `IFileSystem`, with synthetic ROM images. No real ROM bytes.
- Golden equivalence: for the existing definition fixtures, the session's
  per-map decode equals the legacy `MapData` / `XScaleData` / `YScaleData`
  output before the legacy path is deleted.
- `mainwindow_test`: open, close, and reopen routing by `SessionId`. A map
  window for a closed session is inert, and a failed or cancelled ECU read
  adds no tree row.
- Each slice passes `bazel build -k --config=release //...`,
  `bazel test -k --config=release //...`, `//:clang_tidy_report_changed`, and
  the platform CI matrix.

## Hardware

No slice changes a wire sequence. The ECU write path's preflight moves but
keeps its behavior, so the flash qualification matrix is unchanged. The
post-read handoff and checksum paths are recorded as needing re-verification
on the bench checklist before release, not as newly qualified.

## Documentation

On completion, update the 6m section of the modularization plan, the
"Split `FileActions`" and "Replace parallel-list data models" entries in the
tech-debt roadmap, and add a design note on calibration session ownership and
bytes-as-truth. Delete this spec and its plan in the final slice, following
earlier steps.
