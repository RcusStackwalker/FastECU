# Native protocol and vehicle catalog — design

Date: 2026-10-04. Status: approved; amended during planning (see "Amendments from planning").

## Problem

`resources/shared/config/protocols.cfg` (63 `<protocol>` and 65 `<car_model>`
entries) is compiled into the Qt resource bundle, copied into
`<config root>/<version>/config/` when absent, and parsed at startup and again
on every flash and EEPROM read. The file looks like user configuration but is
not usable as such, and parts of it are unsafe to edit:

- **The code already owns the facts.** Every flash family validates the cfg
  `mcu` against its own `constexpr` table, every kernel-uploading family does
  the same for `kernel_addr`, and the plans enforce their own operation
  support. Protocol names are a closed set: flash routing (`kRoutes` in
  `flash_workflow.cpp`) and checksum routing (`kRoutes` in
  `checksum/dispatch.cpp`) select behavior by name, so an added or renamed
  protocol reaches nothing.
- **One edit silently weakens a write.** `checksum` `yes` → `no` makes
  `apply_checksum_correction` return `NoModuleForProtocol`, and because the
  flag is `no` no dialog is shown: the uncorrected image is written. The flag
  is otherwise derivable — across all 63 protocols, `yes` holds exactly when a
  checksum route exists.
- **Edits do not survive.** Provisioning is per version and migrates only
  `fastecu.cfg`, so an edited `protocols.cfg` is replaced at every upgrade;
  within one version string an existing copy shadows bundled fixes.
- **The saved selection is a row index.** `fastecu.cfg` `protocol_id` indexes
  `<car_models>`; it migrates while `protocols.cfg` does not, so a reordered
  list silently selects a different vehicle and flash protocol.
- **Three readers.** `ConfigSession` at startup, `flash_workflow.cpp`
  `resolveProtocol` per flash, and `build_eeprom_read_plan` each parse the
  file; a mid-session edit makes the UI and the flash disagree.
- **Defects nothing checks.** A kernel name that differs from the bundled
  file only by case, two vehicles referencing protocols renamed upstream, 14
  protocols no vehicle reaches (MUT/DMA logging among them), kernel names that
  are not bundled, and one EEPROM entry with a likely-wrong kernel address.
- **Dead fields.** `ecu_id_ascii`, `ecu_id_addr`, `ecu_id_length`,
  `cal_id_ascii`, `cal_id_addr`, `cal_id_length` have no consumer.

## Decisions

| Question | Decision |
| --- | --- |
| Runtime override of protocols or vehicles | None. The data is code; changes go through pull requests. |
| Known data defects | Fixed in this migration, each as its own commit. |
| Protocols no vehicle reaches | Offer every protocol with a working plan; delete the two that support no operation. |
| Saved selection on upgrade | Reset: the old `protocol_id` is ignored and the operator chooses a vehicle once. |
| Ownership of hardware facts | The catalog and the flash plans both declare them; tests prove agreement (approach A). |
| `sub_ecu_eeprom_denso_sh7058_densocan` kernel address | Corrected to `0xFFFF3000`, with a VERIFY item for first bench use. |

Rejected alternatives, recorded for the ADR: making the plans the single owner
of hardware facts (approach B — a large hardware-facing refactor with no
safety gain over A once the data is compiled in; a candidate for a later
spec), and making the shared table the single owner with plans reading it
(approach C — plan validation would compare a value with itself, removing the
second, independent declaration on the hardware path). Migrating the saved row
through the previous version's `protocols.cfg`, and mapping rows through a
frozen order, were rejected in favor of the reset.

## Goals and non-goals

Goals:

- No `protocols.cfg` in the bundle or in a newly provisioned config directory.
- Protocol and vehicle data are portable `constexpr` data in `src/backend`,
  passed to consumers as a value; no consumer reads it from disk.
- Consistency the file left unchecked is enforced at compile time or by tests.
- Every protocol in the catalog is reachable from a vehicle.
- A selection is stored by a stable vehicle id.
- `bazel test //...`, the Android cross-compile gate, `prek` and Gazelle pass.

Non-goals:

- Kernels. They stay bundled, provisioned to disk and read by name.
- Deduplicating the plans' own catalogs against the shared catalog
  (approach B).
- `fastecu.cfg` defaults other than removing `protocol_id`, and
  `logger.cfg`.
- Typing `checksum::ChecksumSelection::checksum_flag`, `make`, or the
  transport lists beyond what this design states.

## Data model

A new portable target in `src/backend/config` (no Qt, no I/O, no pugixml).

`ProtocolEntry` keeps its name to limit churn; its fields become:

| Field | Type | Notes |
| --- | --- | --- |
| `name` | `std::string_view` | Unique key. |
| `alias` | `std::string_view` | Empty when none (today's "No alias" default). Every shipped alias is a single token. |
| `ecu`, `mcu`, `mode`, `description` | `std::string_view` | As today. |
| `checksum` | `ChecksumSupport` | `Corrected` (`yes`: run the module), `Missing` (`n/a`: warn before a write), `None` (`no`: no checksum, no warning). |
| `read`, `test_write`, `write` | `bool` | |
| `flash_transport`, `log_transport` | `std::string_view` | Comma lists, split by the UI as today. |
| `log_protocol` | `std::string_view` | |
| `kernel` | `std::string_view` | Empty when the family uploads no kernel. |
| `kernel_load_address` | `std::optional<std::uint32_t>` | Replaces the `kernel_addr` string and its runtime hex parse. |

The six `ecu_id_*` and `cal_id_*` fields are dropped. One adapter,
`checksum_flag(ChecksumSupport)`, yields `"yes"`/`"n/a"`/`"no"` for
`ChecksumSelection`, the only consumer still taking the string.

`VehicleEntry` replaces `CarModelEntry` and `ResolvedCarModel`:

| Field | Type | Notes |
| --- | --- | --- |
| `id` | `std::string_view` | Stable, unique, never reused. |
| `make`, `model`, `version`, `type`, `kw`, `hp`, `fuel`, `year` | `std::string_view` | As today. `make` stays a string: checksum routing matches it. |
| `protocol` | `std::string_view` | Name of an existing `ProtocolEntry`. |

Vehicle ids are generated once, by the same one-off script that generates the
table, as `slug(make, model, version, year) + "--" + slug(protocol)`, where
`slug` lowercases and collapses each run of characters outside `[a-z0-9]` to
one `-`, trimming leading and trailing `-`; for example
`subaru-impreza-2-0-wrx-sti-2008--sub-ecu-eeprom-denso-sh7058-can`. The rule is
applied after the data fixes below and is unique across all 75 rows. After
generation an id is a literal: editing a vehicle's display text never changes
it.

`Catalog` holds `std::span<const ProtocolEntry>` and
`std::span<const VehicleEntry>` and provides:

- `find_protocol(name) -> const ProtocolEntry*`
- `protocol_of(const VehicleEntry&) -> const ProtocolEntry&` (total: the
  consistency check guarantees a match)
- `find_vehicle(id) -> const VehicleEntry*`
- `last_vehicle_for_protocol(name) -> std::optional<std::size_t>` — the
  last-match rule ROM open uses today
- `first_vehicle_for_alias(flash_method) -> const VehicleEntry*` — the
  first-match rule alias resolution uses today; an empty alias never matches,
  so an empty flash method resolves to nothing (today's "No alias" default
  had the same effect)

`builtin_catalog()` returns the built-in catalog. Its data lives in an internal
header as `inline constexpr` arrays, included by the catalog's `.cpp` and its
test. `constexpr bool catalog_is_consistent(protocols, vehicles)` is
`static_assert`ed on the built-in data; synthetic test catalogs `ASSERT` it.
It requires: unique protocol names; unique vehicle ids matching `[a-z0-9-]+`;
every vehicle's protocol exists; every protocol has at least one vehicle; no
`kernel_load_address` without a `kernel`.

## Consumers and data flow

- **Composition.** `DesktopComposition` passes `config::builtin_catalog()` to
  `ConfigSession`. Nothing else names the built-in catalog; consumers reach it
  through the session or through a flash request.
- **`ConfigSession`.**
  - `initialize` no longer loads protocols or vehicles; its "Unable to load
    protocols", "Unable to load vehicles" and "No vehicles defined" failures
    are removed.
  - `AppConfig::selected_protocol_id` becomes `selected_vehicle_id`, persisted
    as a `vehicle_id` setting. `protocol_id` is ignored on read and no longer
    written.
  - An absent or unknown id means no selection — there is no fallback to row
    0. `selected_vehicle()` returns `nullptr` then.
  - `select_row(row)` stays for the dialogs (rows index the current catalog
    and are never persisted); it stores the vehicle's id and sets
    `selected_log_protocol` as today.
  - `select_by_protocol_name` and alias resolution keep their rules through
    the `Catalog` lookups.
  - `vehicles()` returns `std::span<const VehicleEntry>`.
- **Startup gate.** `ensure_vehicle_selected(ConfigSession&, chooser)` in
  `apps/desktop` runs after a successful composition and before `MainWindow`
  is built. With a selection it returns at once. Without one it calls the
  chooser, which `main.cpp` wires to a modal `VehicleSelect` with no parent
  (the dialog already tolerates no selection). An accepted row is selected
  and saved, so an in-app restart does not ask again; a cancel ends startup
  with exit code 0 (the operator's choice, not a failure), no `MainWindow`
  and no ECU I/O. `MainWindow`'s invariant that
  a vehicle is always selected, and its 27 call sites, are unchanged.
- **Flash.** `FlashWorkflowRequest` replaces `std::string protocol` and
  `std::string mcu` with `config::ProtocolEntry protocol`, filled from the
  selected vehicle by the request's builder. `resolveProtocol`,
  `resolveKernel` and `resolveKernelBytes` read `request.protocol.kernel` and
  `request.protocol.kernel_load_address` instead of the file; `paths` remains
  for the kernel directory. Family validation is unchanged.
- **EEPROM.** `build_eeprom_read_plan` takes a `const config::ProtocolEntry&`
  in place of a protocol name, so it stops re-reading both catalogs and drops
  its "no car model references protocol" failure.
- **ROM open and UI.** `rom_open` reads `checksum`, `mcu` and `alias` through
  the new types; `config_fields` uses `std::string_view` and `bool` member
  pointers; the vehicle and protocol dialogs iterate `catalog.vehicles()`;
  the calibration coordinator compares `ChecksumSupport` values instead of
  `"n/a"`.

## Removals

- `resources/shared/config/protocols.cfg`, its `config.qrc` entry, and its
  `exports_files` in `resources/shared/BUILD.bazel`.
- `resources/shared/protocols_cfg_*_test.cpp` (three tests pinning file
  strings), superseded by the capability-agreement test below.
- `src/backend/config` `protocol_catalog`, `car_model_catalog` and
  `protocols_document` with their tests; pugixml remains for `app_config`.
- `protocol_field_or_placeholder`, `kMissingProtocolField`, and the
  unresolved-vehicle cases in session and UI tests.
- The `DesktopCompositionTest` cases for the removed startup failures.
- `protocol_id` from the bundled `fastecu.cfg`, so a fresh install starts
  unselected; `app_config_test`'s exact-content pin is updated.

Provisioning code is unchanged: it copies whatever the `config` bundle holds.
A stale `protocols.cfg` can exist only in a directory created by an earlier
build with the same version string; it is ignored and left in place.

## Data fixes

Each is a separate commit after the faithful table lands.

1. `sub_tcu_denso_sh7055_can` kernel `ssmk_tcu_can_SH7055_35.bin` →
   `ssmk_tcu_can_sh7055_35.bin`, the bundled name. Today the read fails on
   case-sensitive filesystems before any ECU I/O.
2. The two Subaru Legacy 2.0 A/T 1990 vehicles referencing
   `sub_ecu_unisia_jecs_92` and `_97` → `sub_ecu_unisia_jecs_m3779x` and
   `_m3775x`. Upstream `90f11ae9` renamed the protocols without updating the
   vehicles. This makes the read-only Unisia Jecs path selectable.
3. Year `20011` → `2011` on the SH7059 Denso CAN Diesel vehicle; trailing
   space removed from `2.0 5MT ` on three vehicles.
4. `sub_ecu_unisia_jecs_m3779x` and `_m3775x`: `kernel_addr 0x00000000` with
   no kernel → no kernel load address. Their plan already requires no kernel.
5. `sub_ecu_denso_sh72543_can_diesel`: kernel `ssmk_can_tp_sh72543d_euro6.bin`
   (not bundled) and `kernel_addr 0xFFF80000` → none. The family talks to the
   ECU's on-board kernel and rejects a plan carrying one, as the
   [flash qualification matrix](../../flash-qualification-matrix.md) records.
6. `sub_ecu_eeprom_denso_sh7058_densocan` kernel load address `0xFFFF6004` →
   `0xFFFF3000`. The same kernel file is loaded at `0xFFFF3000` by all three
   main-flash DensoCAN SH7058 entries, and `0xFFFF6004` is the SH7055
   address. The EEPROM preflight accepts both (the SH7058 kernel RAM region is
   `0xFFFF3000`–`0xFFFFC000`), so no guard distinguishes them. VERIFY on first
   bench use.
7. Delete `sub_ecu_denso_mc68hc16y5_04` and `_04_ecutek` (no supported
   operation), with their flash route row and the MC68 plan's branch that
   rejects them; the unbundled `ssmk_hc16.bin` reference goes with them.
8. Append ten vehicles, after all existing rows so first-match alias
   resolution is unchanged (`fxt02` still resolves to
   `sub_ecu_denso_sh7055_02`). Each follows the existing unknown-vehicle
   convention — model `Unknown`, version `unk ECU`, type/kw/hp/year `Unk`,
   fuel `Petrol` — with make `Subaru` except where noted:
   - `mitsu_ecu_m32r_kline_mut_dma` (make `Mitsubishi`)
   - `sub_ecu_denso_sh7055_02_ecutek`
   - `sub_ecu_denso_sh7055_04_cobb`, `sub_ecu_denso_sh7058_cobb`,
     `sub_ecu_denso_sh7058_can_cobb`
   - `sub_ecu_eeprom_denso_sh7055_kline`, `sub_ecu_eeprom_denso_sh7058_kline`,
     `sub_ecu_eeprom_denso_sh7055_densocan`,
     `sub_ecu_eeprom_denso_sh7058_densocan`
   - `sub_ecu_eeprom_denso_sh7058_can_diesel` (fuel `Diesel`)

Recorded and deliberately not changed: the Unisia `m3779x`/`m3775x` entries
declare `log_transport SSM` and `log_protocol SSM1`, where every other entry
names a physical transport. There is no evidence of the correct value.

Result: 61 protocols, 75 vehicles.

## User-visible behavior changes

- Startup asks for a vehicle once after upgrading, and on a fresh install;
  cancelling quits.
- Newly selectable: MUT/DMA logging, Unisia Jecs M3779x/M3775x reads, five
  EEPROM reads, two test-write-only Cobb paths
  (`sh7055_04_cobb`, `sh7058_cobb`), and two full-write paths
  (`sh7058_can_cobb`, `sh7055_02_ecutek`). All remain `experimental` in the
  qualification matrix.
- A ROM whose definition names `sti04_cobb`, `sti05_cobb` or `subarucan_cobb`
  now selects its Cobb vehicle; today it selects nothing.
- The SH7055 TCU read works on case-sensitive filesystems.
- A `protocols.cfg` in the config directory has no effect.

## Testing

Compile time — the `static_assert` on `catalog_is_consistent` for the built-in
catalog.

Catalog package (portable gtest):

- Lookup behavior: `find_protocol`, `find_vehicle`, last match by protocol,
  first match by alias. Pinned alias outcomes: `fxt02` →
  `sub_ecu_denso_sh7055_02`, `wrx02` → the first `sub_ecu_denso_mc68hc16y5_02`
  vehicle, `subarucand` → `sub_ecu_denso_sh7058_can_diesel`.
- One test per data fix, asserting the corrected value, so undoing a fix fails
  by name.

Cross-package consistency:

- **Capabilities agree with the plans** — in the platform flash package, which
  owns `FlashWorkflowFactory`. For every built-in protocol and each of Read,
  TestWrite and Write, build the workflow from the catalog entry, with the
  real bundled kernels copied to a temporary kernel directory:
  - an operation the entry marks unsupported fails with
    `ErrorKind::Unsupported`;
  - for a supported Read, the workflow's first step is not a failure, proving
    the plan accepts the entry's `mcu`, kernel and load address;
  - for a supported Write or TestWrite given an image of the size the family
    requires, the first step is not a failure;
  - every protocol with any capability has a route, and
    `mitsu_ecu_m32r_kline_mut_dma` has none.

  The UI never offers an unsupported operation, so the error kind of such a
  rejection is observable only here. Where a workflow rejects one with a kind
  other than `Unsupported`, the implementation plan aligns that family's kind;
  no rejection is removed or weakened.
- **Checksum flag agrees with checksum routing** — in `tests/`. For every
  built-in vehicle, `checksum == Corrected` exactly when
  `checksum::has_route(vehicle.make, protocol.name)` is true. `has_route` is a
  new backend function exposing route existence without ROM bytes.
- **Kernel names agree with the bundle** — in `tests/`. Every non-empty
  `kernel` equals a file name in `resources/shared/kernels` by exact string
  comparison against a directory listing, so a case-insensitive filesystem
  cannot hide a mismatch; every bundled kernel is referenced by some protocol.

Migrated tests: the session fixture and the flash workflow, EEPROM plan,
MainWindow, dialog, coordinator and ROM-open tests replace XML strings with
small synthetic `Catalog` values that satisfy `catalog_is_consistent`. Tests of
removed behavior are deleted.

New behavior tests:

- Session: an absent or unknown `vehicle_id` yields no selection; `protocol_id`
  is ignored; `select_row` persists the vehicle id.
- Startup gate, with a fake chooser: an existing selection does not call the
  chooser; an accepted row selects and saves; a cancel returns "do not start".

## Rollout

Ordered so each step is reviewable and verifiable on its own:

1. Generate the catalog from the current file with a one-off, uncommitted
   script, faithfully (the six dead fields dropped, nothing else changed),
   plus a temporary parity test comparing it field by field with the
   still-present file.
2. Move every consumer to the catalog and introduce `vehicle_id` and the
   startup gate.
3. Apply the data fixes, one commit each, each with its pinning test; add the
   consistency tests.
4. Delete the file, the loaders, the parity test and the pinning tests.

Every step passes `bazel test --config=release //...`, the Android
cross-compile gate, `prek run --all-files` and
`python3 scripts/gazelle_check.py`. No address-window guard or plan
validation is relaxed: the only plan changes remove the two MC68 `_04` names
the MC68 plan already rejected (data fix 7) and, if needed, align a rejection's
error kind to `Unsupported`.

## Documentation

- ADR 0019, "Protocol and vehicle data are compiled into the backend":
  decision as above; consequences — upstream `protocols.cfg` changes are
  ported by hand, saved selections reset once, kernels are unaffected.
  The [ADR index](../../adr/README.md) gains the entry, and its stale
  "next new ADR is 0018" becomes 0020.
- [Flash qualification matrix](../../flash-qualification-matrix.md): the
  VERIFY item for data fix 6; notes that the newly reachable variants are
  experimental; rules that say "the exact `protocols.cfg` name" name the
  catalog instead.
- [Design notes](../../design-notes.md): the startup-rejection paragraph loses
  the protocols failures; a short catalog section records the model, the
  startup gate and the reset.
- [Connection bench checklist](../../connection-bench-checklist.md) and
  [Subaru TCU Hitachi M32R CAN bench checklist](../../subaru-tcu-hitachi-m32r-can-bench-checklist.md):
  references to `protocols.cfg` point at the catalog source.

## Amendments from planning

Writing the [implementation plan](../plans/2026-10-04-native-protocol-catalog.md)
against the code changed these details. The plan follows the amended text.

1. **Type names.** `ProtocolSpec` and `VehicleSpec`, not `ProtocolEntry` and
   `VehicleEntry`. The old loader types keep their names until their last
   consumer moves, so the migration can land one consumer group per task
   instead of in one change.
2. **A vehicle's protocol is a pointer.** `VehicleSpec::protocol` is a
   `const ProtocolSpec*` resolved at compile time by `protocol_in()`, not a
   name. About 40 UI call sites read protocol fields from a vehicle alone,
   and a name would have forced each to take the catalog too.
   `protocol_of()` is dropped; `find_vehicle(id)` returns the row index the
   session and choosers work in.
3. **What is checked at compile time.** The `static_assert` covers only the
   reference checks (every vehicle has a protocol, every protocol a vehicle,
   no kernel address without a kernel), which stay inside MSVC's
   constant-evaluation step limit. Duplicate names and ids, id spelling and
   alias spelling are checked by `catalog_problems()` in a test that runs in
   every `bazel test //...`.
4. **Capability check.** A supported Write or TestWrite asserts only that
   the workflow does not reject it as `Unsupported`: the image size each
   family accepts is family-specific. EEPROM entries are checked by building
   the EEPROM plan directly, because the EEPROM workflow builds its plan only
   after its prompts.
5. **Rollout order.** The data fixes land before the consumers move, because
   consumers rely on every vehicle having a protocol; the temporary parity
   test lists each fix as an expected difference from the file.
   `ConfigPaths::protocols_file` is removed with the file.
6. **The session stores a reference.** `ConfigSession` takes and keeps
   `const Catalog&` and reads it in `initialize()`, so a test can swap the
   catalog before initializing.

## Follow-ups (out of scope)

- Embed kernels and verify a per-protocol SHA-256 before upload.
- Approach B: one owner for hardware facts.
- Type `ChecksumSelection::checksum_flag` and `make`.
- Bundled `fastecu.cfg` defaults that are one machine's state (`serial_port`,
  `primary_definition_base`).
- `logger_mut_dma_example.xml` is referenced by the README but not bundled.
- The Unisia `m377x` `log_transport` value.
