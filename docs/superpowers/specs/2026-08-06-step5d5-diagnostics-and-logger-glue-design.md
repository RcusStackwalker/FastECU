# Step 5d-5 — Diagnostics Tables and Logger-Definition Glue — Design

**Status:** Approved 2026-08-06. Final sub-project of step 5d (see the
[5d umbrella design](2026-07-24-step5d-fileactions-decomposition-design.md),
whose 5d-5 row this document specifies). Depends on 5b (merged, PR #78) and
5d-1 (merged).

**Goal:** drain the last two transitional Qt shims left in the algorithms
layer and convert the logger-definition/logger-conf slice of `FileActions` to
portable use cases. When this lands, step 5d is complete.

## Slice split

The umbrella's 5d-5 row bundles two bodies of work that share nothing — no
common file, no common caller, no common data. They are sequenced as two
independently landable sub-slices, following the 5d-4 / 5d-4b precedent:

| ID | Sub-slice | Core deliverable |
|----|-----------|------------------|
| **5d-5** | Diagnostics table migration | The five NRC/DTC lookup tables move from `QHash` statics on `FileActions` to portable constants beside the parsers; `//src/algorithms/diagnostics:qt_compat` is deleted |
| **5d-5b** | Logger-definition glue | `read_logger_definition_file` and `read_logger_conf` become portable use cases; the packed conversions string is replaced by a typed model and its five consumers are converted |

5d-5 goes first. It is small and mechanical, and it exercises the
closure/allowlist changes before the larger slice depends on them.

## Findings that shaped the scope

Three pieces of the umbrella's stated 5d-5 scope turned out not to exist as
live code. They are recorded here because they shrink the slice materially
and because the umbrella's "dead code is dropped, not ported" rule applies to
each.

- **`save_logger_conf` is dead.** `file_actions.cpp:1050-1112` is entirely
  inside a `/* */` block, so the method declared at `file_actions.h:221` has
  no definition. Its only reference is a commented-out call at
  `file_actions.cpp:996`. Its real-world job is already done by
  `read_logger_conf`'s two write paths. It is deleted, not ported.
- **DTC data from the logger definition file is never captured.**
  `dt_codes_structure dt_codes_struct` (`file_actions.h:133-141`) is declared,
  never populated and never read. The `<dtcodes>` and `<ecuparams>` sections
  of the logger definition file are parsed only to feed `debugLogDtcodes` /
  `debugLogEcuparams`, which are `if constexpr (!kDebugFileActions) return;`
  no-ops. "DTC table ownership" in the umbrella therefore means only the
  static `QHash` tables in `error_codes.h`.
- **`read_logger_conf` is three operations behind one flag.** Its `modify`
  parameter selects between loading a selection and persisting one, and a
  third behavior hides in the ECU-id-not-found branch, which initializes
  defaults and writes a fresh `<ecu>` subtree as a side effect of a call named
  "read".

## 5d-5: diagnostics table migration

**New.** `src/algorithms/diagnostics/dtc_tables.h` / `.cpp` — the five tables
from `error_codes.h` as `const std::unordered_map<int, std::string>&`
accessors backed by function-local statics, so there is no static-init-order
dependency. Values are copied verbatim; a golden test asserts each table's
size and a sample of entries against the current `QHash` contents, so the
transcription is provably lossless.

**Changed.** `nrc_parser.h` and `dtc_parser.h` each gain one overload that
binds the real tables:

```cpp
std::string nrc_description(bytes::ByteView nrc);
std::string dtc_description(std::uint16_t dtc);
```

The existing caller-supplied-table forms stay exactly as they are, along with
their tests. Keeping them is not dead flexibility: synthetic tables are what
make the table-independent behavior (short frame, non-negative response,
unknown code, category selection from the top two bits) cheap to assert
without coupling the assertions to table contents.

**Deleted.** `qt_dtc_parser.*`, `qt_nrc_parser.*`, the
`//src/algorithms/diagnostics:qt_compat` target,
`diagnostic_parsers_qt_compat_test.cpp`,
`src/backend/definitions/error_codes.h`, the five
`static const QHash<int, QString>` declarations at `file_actions.h:171-175`,
and `FileActions::parse_nrc_message` / `parse_dtc_message`.

**Converted callers**, ten sites in two files:

- `src/ui/desktop/dtc_operations.cpp` — four `parse_nrc_message`, two
  `parse_dtc_message`.
- `src/platform/desktop/common/flash/legacy/tcu/flash_tcu_subaru_hitachi_m32r_can_operation.cpp`
  — six `parse_nrc_message`.

Every one is inside a `LOG_E(... + QString)` expression, so each becomes
`QString::fromStdString(nrc_description(...))` over a `bytes::ByteView`
obtained through `qt_bytes.h`.

**Enforcement.** `//src/algorithms/diagnostics` already sits under the
`src/algorithms` portable root, which is registered with `None` (whole subtree
portable), so `dtc_tables` needs no new registration. Deleting `:qt_compat`
only shrinks the Qt surface.

## 5d-5b: logger-definition glue

### Package placement

All new targets go in **`//src/backend/logging`**, not a new package. The
umbrella's "new packages, never co-located with the legacy glob" rule exists
to prevent Bazel glob collisions with `src/backend/definitions/`;
`src/backend/logging` has no legacy glob. It already owns `logging_types.h`,
where `fastecu::logging::Conversion` belongs, and it is where the output is
consumed — `LoggerDefinition` is the typed input that `LoggingChannel` has
been missing since 5b.

### Components

**`logger_definition_model.h`** (portable) — the value types:

```cpp
struct Conversion {
    std::string units, expr, format;
    std::string gauge_min, gauge_max, gauge_step;
};
struct LoggerParameter {
    std::string protocol, id, name, description;
    std::string address, length;
    std::string ecu_byte_index, ecu_bit, target;
    bool enabled;
    std::vector<Conversion> conversions;
};
struct LoggerSwitch {
    std::string protocol, id, name, description;
    std::string address, ecu_byte_index, ecu_bit, target;
};
struct LoggerDefinition {
    std::vector<LoggerParameter> parameters;
    std::vector<LoggerSwitch> switches;
};
```

`protocol` stays a field on each parameter and switch rather than becoming a
grouping key. Every consumer filters with
`log_value_protocol.at(j) == protocol`; regrouping would change that lookup,
and that is step-6 work, not this slice's.

**`logger_definition_parser`** (portable, pugixml) —
`Result<LoggerDefinition> parse_logger_definition(bytes::ByteView)`. Pure: no
file I/O, no config lookup. This is where the packed conversions string dies.

**`logger_conf`** (portable, pugixml) — the three operations currently fused
behind `modify`:

```cpp
Result<std::optional<LoggerSelection>> read_selection(bytes::ByteView, std::string_view ecu_id);
Result<bytes::Bytes>                   write_selection(bytes::ByteView, std::string_view ecu_id,
                                                       const LoggerSelection&);
LoggerSelection                        default_selection(const LoggerDefinition&);
```

with

```cpp
struct LoggerSelection {
    std::string protocol;
    std::vector<std::string> gauge_ids;
    std::vector<std::string> lower_panel_ids;
    std::vector<std::string> switch_ids;
};
```

`read_selection` returns `nullopt` when the ECU id is absent, rather than
initializing anything. `default_selection` is the enabled-entry walk capped at
15 gauges / 12 lower-panel values / 20 switches — pure, no document involved.
`write_selection` handles both updating an existing `<ecu>` element and
appending a new one. The current "ECU id not found, initialize and persist"
branch becomes the service composing `default_selection` with
`write_selection`, so the write is an explicit step rather than a side effect
of a read.

**`logger_definition_service`** (portable) — holds `IFileRepository`,
`IResourceBundle` and `IAtomicFileWriter`. Owns handle resolution and the
read → parse → maybe-write composition:

```cpp
Result<LoggerDefinition> load_definition(std::string_view handle);
Result<LoggerSelection>  load_or_initialize_selection(std::string_view conf_handle,
                                                      std::string_view ecu_id,
                                                      const LoggerDefinition&);
Status                   save_selection(std::string_view conf_handle,
                                        std::string_view ecu_id,
                                        const LoggerSelection&);
```

Handle resolution includes the CDBG fallback currently inline at
`file_actions.cpp:1122-1130`: when no logger definition file is configured and
the selected log protocol is `CDBG`, prefer
`<config_files_directory>/logger_cdbg_example.xml` and fall back to the
bundled `logger_cdbg_example.xml` via `IResourceBundle`. The service resolves
and returns the handle; writing it back into
`configValues->romraider_logger_definition_file` is the legacy adapter's job.

**`legacy_logger_adapter`** (Qt-typed, not portable) — fans `LoggerDefinition`
and `LoggerSelection` out into `FileActions::LogValuesStructure`, including
the new typed `log_value_conversions` member.

### Error handling

Following 5d-4, `FileActions` keeps showing its own `QMessageBox::warning`
dialogs, now driven by the `Result` the use case returns rather than by an
inline `QFile::open` failure. This is what `open_subaru_rom_file` already does
at `file_actions.cpp:1916-1920`. The 5d-4 umbrella row's "dialog relocation to
MainWindow" referred to `QFileDialog` choosers; the logger paths have none, so
all three warnings stay where they are.

`FileActions::read_logger_definition_file` and `read_logger_conf` keep their
signatures and their exact return contracts, including
`read_logger_definition_file` returning `logValues` (not `nullptr`) when the
file cannot be opened, and `read_logger_conf` returning `nullptr` on open
failure and `0` on the no-protocol branch.

### Behavior change, deliberate

`read_logger_conf` currently holds the conf file open `ReadWrite` and calls
`file.resize(0)` **before** `xmlBOM.save()`. If the save fails or the process
dies between the two, the user's logger configuration is destroyed. Going
through `IFileRepository` read plus `IAtomicFileWriter::replace` makes this
read-then-atomic-replace. This is a fix, not a regression, but it means the
fidelity assertion is "byte-identical output", not "identical file-handle
behavior".

## Ownership and mutation story

`FileActions::LogValuesStruct` is one long-lived member of the `FileActions`
`QWidget`. `MainWindow::logValues` (`mainwindow.cpp:443`) and
`LogValues::logValues` (`logvalues.h:28`) are raw aliases to it for the
application's lifetime. It is mutated from four directions, and those are not
the same kind of state:

| Kind | Fields | Written by | 5d-5b treatment |
|---|---|---|---|
| Definition | `log_value_id`/`name`/`description`/`address`/`length`/`units`/`protocol`, all `log_switch_*` descriptors | `read_logger_definition_file`, once at startup | Moves to `LoggerDefinition`; adapter fans out |
| Selection | `dashboard_log_value_id`, `lower_panel_log_value_id`, `lower_panel_switch_id`, `logging_values_protocol` | `read_logger_conf` in both directions, and the UI directly at `logvalues.cpp:212`/`239`/`264` | Moves to `LoggerSelection` |
| Live sample | `log_value`, `log_switch_state` | `logging_value_adapter.cpp:42`, every poll | Stays in `LogValuesStructure`, untouched |
| Both | `log_value_enabled`, `log_switch_enabled` | The XML `enabled` attribute at parse time, then overwritten at runtime by `log_operations_ssm.cpp:348-360` from the ECU's capability response | `LoggerParameter::enabled` holds the definition default only; the runtime override stays in the legacy struct |

The last row is the trap. If the adapter fans a `LoggerDefinition` into
`LogValuesStructure` a second time after logging has started, it silently
resets every runtime capability flag the ECU reported. The contract is
therefore: **the definition fan-out runs exactly once, at load**, and no
selection path ever rewrites the definition fields. Today that holds by
accident, because `read_logger_conf` happens to touch only the three selection
lists. 5d-5b must keep it true deliberately, and carries a test asserting that
a selection round-trip leaves `log_value_enabled` unchanged.

## Consumer conversion

The packed conversions string is decoded in four production sites with
hard-coded stride-7 arithmetic and positional indexes:

| Site | Current | Converted |
|---|---|---|
| `logvalues.cpp:83` | `for (k = 1; k < units.length(); k += 7) addItem(units.at(k))` | `for (const auto& c : conversions) addItem(c.units)` |
| `logvalues.cpp:125` | same stride-7 walk | same |
| `mainwindow.cpp:2160` | `value_unit.at(1)` | `conversions.at(0).units` |
| `mainwindow.cpp:2199` | `.split(",").at(1)` | `conversions.at(0).units` |
| `logging_snapshot_adapter.cpp:46` | `unit_fields.at(1)`/`.at(2)`/`.at(3)` | `c.units` / `c.expr` / `c.format` |

The conversion reaches them through the legacy struct, not by changing what
`read_logger_definition_file` returns. One member changes shape:

```cpp
- QStringList log_value_units;
+ QList<QList<fastecu::logging::Conversion>> log_value_conversions;
```

Parallel-array indexing by row is preserved exactly — dismantling that is
step-6 work. This does cross the umbrella's "`MainWindow` call sites are
untouched until step 6" line, knowingly: the alternative is exporting the
stride-7 format into the new portable layer only to undo it later. The
signatures of `FileActions`'s methods are unchanged; only one field of the
legacy struct changes type, and each consumer changes one to three lines.

`logging_snapshot_adapter.cpp:46`'s `unit_fields.size() < 4` malformed-input
guard becomes a check that `conversions` is non-empty, and
`validate_logger_values`'s `log_value_units` length check
(`file_actions.cpp:640`) becomes structural.

## Dead code removed by 5d-5b

Each verified by checking references, not assumed — per the 5d-4b lesson that
vestigial code here hides behind stale string comparisons as well as behind
unreferenced symbols.

- `save_logger_conf` — `file_actions.h:221` and `file_actions.cpp:1050-1112`.
- `dt_codes_structure` / `dt_codes_struct` — `file_actions.h:133-141`.
- Seven `LogValuesStructure` fields with zero references outside their
  declaration: `log_value_from_byte`, `log_value_format`, `log_value_gauge_min`,
  `log_value_gauge_max`, `log_value_gauge_step`, `log_value_ecu_id`,
  `log_value_type`. These are the dedicated homes for exactly the data the
  packed CSV string absorbed instead — the fields the parser should have
  filled and never did. Deleting them and the packed format is one cleanup.
- `log_values_by_protocol` — cleared and appended at
  `log_operations_ssm.cpp:331`/`338`, never read anywhere. Write-only state.
- `debugLogTransports` / `debugLogDtcodes` / `debugLogEcuparams`
  (`file_actions.cpp:26-119`) — gated on `kDebugFileActions` and reachable
  only from the `QDom` traversal being replaced. They print XML attributes the
  user already has in the file.

## Testing

Characterize before converting, per the step 4 and 5c precedent.

1. Extend `file_actions_parsing_test.cpp` with a golden assertion over the
   current `read_logger_definition_file` output on a fixture logger XML, then
   convert against it.
2. Portable unit tests: parser (well-formed, malformed, multiple conversions
   per parameter, missing attributes falling back to their defaults);
   `read_selection` (ECU found, ECU absent, malformed document);
   `default_selection` (caps at 15 / 12 / 20, respects `enabled`);
   `write_selection` (update existing `<ecu>`, append new `<ecu>`, round-trip).
3. The ownership test from the section above: a selection round-trip leaves
   `log_value_enabled` unchanged.
4. `logging_adapters_test.cpp`'s `append_value_with_units` helper changes
   shape with the typed member.

`model_validation_test.cpp:213` currently seeds `log_value_units` with a
three-field string, `"rpm,x,0"`, which does not match the seven-field stride
the parser emits — it asserts against a format the parser never produces.
Typing the field makes that impossible to write, which is the point.

**Fidelity risk to pin explicitly:** the conf file is written today with
`QDomDocument::save(output, 4)`, a four-space indent. pugixml's default
indent is a tab. `write_selection` must set `pugi::format_indent` with a
four-space string, or every user's logger conf reformats wholesale on first
write.

That is guarded in two stages, because the two halves have different
lifetimes:

- **Transitional.** A byte-identity test comparing `write_selection`'s output
  against a checked-in fixture captured from `QDomDocument::save(output, 4)`.
  This exists to prove the one migration, and it is the only place the old
  writer's output is treated as authoritative. Once 5d-5b has merged, pugixml
  *is* the reference and this assertion pins a writer that no longer exists —
  it would block any later deliberate formatting change for no benefit.
  Dropped as a follow-up (below), replaced by a golden fixture regenerated
  from pugixml's own output plus a `write_selection` → `read_selection`
  round-trip assertion.
- **Permanent.** The four-space `format_indent` setting itself stays. It costs
  one line and spares every existing user a one-time whole-file reflow; the
  follow-up removes the QDom-referenced assertion, not the indent choice.

## Enforcement

`//src/backend/logging:BUILD.bazel` is already listed in the root
`genquery` at `BUILD.bazel:136`, so no root `BUILD.bazel` change is needed.
Only `scripts/check-portable-closure.py` changes: `PORTABLE_ROOTS` gains
`logger_definition_model`, `logger_definition_parser`, `logger_conf` and
`logger_definition_service` under `src/backend/logging`.

Correcting the umbrella's blanket claim that each slice removes its own
allowlist entries: `//src/backend/definitions` has **no**
`serial_compat_allowlist` entry. The list holds only `//src/backend/flash` and
`//src/backend/logging/protocols`
(`scripts/check-serial-compat-allowlist.py:32-33`). 5d-5 and 5d-5b remove
nothing from it, and the plan should not go looking for an entry that does not
exist.

## Gates

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```

Plus `>=80%` new-code coverage and the SonarCloud Quality Gate, as applied to
every slice since 5c. Coverage is budgeted during execution, not treated as a
final gate check.

## Risks

| Risk | Mitigation |
|---|---|
| Re-fanning the definition into `LogValuesStructure` resets runtime capability flags | Explicit once-at-load contract plus the round-trip test above; the adapter has no method that rewrites definition fields |
| pugixml reformats every user's logger conf on first write | Four-space `format_indent`, plus a transitional byte-identity test against a `QDomDocument::save(output, 4)` fixture that the follow-up below replaces with a pugixml-generated golden |
| A "dead" field turns out to be live through a path grep misses | The seven-field and `log_values_by_protocol` removals are each backed by a reference check; the plan re-verifies against a built binary, not only against grep |
| The CDBG resource fallback is exercised only on a machine with no configured logger file | Covered by a service test with a fake `IResourceBundle` and a fake `IFileRepository` that reports the config-dir file absent |
| Consumer conversion crosses into step-6 territory further than intended | Scope is fixed at one struct member and the five listed sites; parallel-array indexing and `logValues` ownership are explicitly out of scope |

## Follow-ups

To be filed as issues when 5d-5b merges, not carried inside it:

- **Drop the `QDomDocument`-referenced byte-identity test.** Once 5d-5b has
  shipped, the migration it proves has happened exactly once and pugixml is
  the reference writer. Replace the QDom-captured fixture with one
  regenerated from pugixml's own output, and add a `write_selection` →
  `read_selection` round-trip assertion so the guard is about the format
  staying stable and deliberate rather than about matching a writer the tree
  no longer contains. Keep the four-space `format_indent`.
