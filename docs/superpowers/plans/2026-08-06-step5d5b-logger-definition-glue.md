# Step 5d-5b Logger-Definition Glue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert `FileActions::read_logger_definition_file` and `read_logger_conf` into portable use cases in `//src/backend/logging`, replace the packed stride-7 conversions string with a typed model, and delete the dead logger code the conversion exposes.

**Architecture:** Four new portable targets in `//src/backend/logging` — a value model, a pugixml parser, a pure conf-operations module, and a service that owns the ports and composes read → parse → maybe-write. A Qt-typed `legacy_logger_adapter` fans the portable results out into the existing `FileActions::LogValuesStructure`, so `FileActions`'s method signatures and return contracts do not change. One field of that struct changes type (`QStringList log_value_units` → `QList<QList<Conversion>> log_value_conversions`), and its five production consumers change with it.

**Tech Stack:** C++23 (`std::expected`, `std::format`, ranges), Bazel 9.1.1, pugixml 1.15, GoogleTest for portable tests, QtTest for the legacy `FileActions` suites, Qt 6.8.3 only at the adapter and UI boundary.

## Global Constraints

- Every header needs `#pragma once` (enforced by prek).
- Backend/algorithms code uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` from `src/algorithms/protocol/bytes.h`; `QByteArray` is a boundary type only, converted explicitly via `src/algorithms/protocol/qt_bytes.h` (ADR 0004). Note `bytes::ByteView` **is** `std::span<const std::uint8_t>` — the same type `src/backend/definition`'s parsers take.
- Backend operations return `fastecu::Result<T>` (`std::expected<T, Error>`) with one of the seven existing `ErrorKind` values. **Do not add an `ErrorKind` value** — that requires amending the step-5 design doc. Parse and config failures are `ErrorKind::InvalidConfig`.
- **Exceptions never cross a port.**
- Prefer `std::string_view` by value over `const char*` / `const std::string&` (ADR 0009); gmock matchers for property assertions (ADR 0010); `std::format` for message construction (ADR 0011); ranges/views over index loops (ADR 0012).
- New portable targets must be registered in **both** places or `//:portable_closure` fails: the `genquery` in the root `BUILD.bazel` (already lists `//src/backend/logging:BUILD.bazel` at line 136 — **no root change needed**) and `PORTABLE_ROOTS` in `scripts/check-portable-closure.py` (line 33, the `src/backend/logging` entry — **this one does change**). A `PORTABLE_ROOTS` name is a **required** target: the guard fails with `required portable target 'X' is missing` if the name has no target. Register a name only in the task that creates its target — never ahead of time.
- Bazel `deps` lists must be in **alphabetical order**. Buildifier enforces it; run `prek` before committing.
- Tests are package-owned and co-located. Use `fastecu_portable_gtest` (Qt-free closure) from `bazel/gtest_targets.bzl` for the portable targets, `fastecu_gtest` where `QT_DEPS` are needed, `fastecu_qttest` from `bazel/qt_targets.bzl` for the QtTest-style `FileActions` suites.
- Mocks/fakes are package-owned in a `testing/` subpackage. The fakes this plan needs already exist in `//src/backend/ports/testing`: `in_memory_file_repository`, `in_memory_atomic_file_writer`, `in_memory_resource_bundle`, `in_memory_file_system`. **Do not write new fakes for these ports.**
- `//src/backend/definitions` has **no** `serial_compat_allowlist` entry (that list holds only `//src/backend/flash` and `//src/backend/logging/protocols`). Do not go looking for one to remove.
- Test fixtures in this repo are **inline raw string literals**, not checked-in data files. There is no `testdata/` convention and no `data =` fixture pattern to follow. Follow `src/backend/definitions/file_actions_parsing_test.cpp`.
- Commit directly to the working branch; do not open PRs unless asked.
- Full gate before each commit that touches production code:
  ```bash
  bazel build -k --config=release //:fastecu //tests/...
  bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
              //:serial_compat_allowlist //:portable_closure
  ```
  Plus `prek run --all-files`.
- Coverage: `>=80%` on new code, budgeted during execution rather than checked at the end. `scripts/coverage-local.sh` produces the report.

## Decisions locked before execution

Two behaviours in the current code are not modelled by the design doc. Both were confirmed against the tree and ruled on; the rulings are binding.

**1. There are two different selection defaults, and both must survive.**

`read_logger_definition_file` seeds the three selection lists at parse time with the **first N entries regardless of `enabled`** (`file_actions.cpp:1193-1200` for parameters, `1263-1266` for switches):

| List | Cap | Filter |
|---|---|---|
| `dashboard_log_value_id` | 15 | none — first 15 parameters |
| `lower_panel_log_value_id` | 12 | none — first 12 parameters |
| `lower_panel_switch_id` | 20 | none — first 20 switches |

`read_logger_conf`'s ECU-not-found branch (`file_actions.cpp:972-994`) seeds the same three lists with the **enabled-only walk** at the same caps. The design doc models only the second one and calls it `default_selection`.

**Ruling: model both explicitly.** `initial_selection(const LoggerDefinition&)` is the first-N walk; `default_selection(const LoggerDefinition&)` is the enabled-only walk. Both pure, both tested. Do not collapse them and do not drop either — `read_logger_conf(modify=false)` clears the lists at `file_actions.cpp:840-842` before re-seeding, so the definition-time seeding is only observable in the window between definition load and the first `read_logger_conf` call, but the UI reads the lists in that window.

**2. The parallel arrays misalign today; the typed model fixes that.**

`log_value_address` and `log_value_length` are appended only inside the `<address>` branch (`file_actions.cpp:1205-1210`), and `log_value_units` only inside the `<conversions>` branch (`file_actions.cpp:1211-1237`). A `<parameter>` missing either child leaves those lists **shorter than `log_value_id`**, silently skewing every subsequent row's parallel-array indexing. `validate_logger_values` (`file_actions.cpp:640`) then logs a length mismatch, which is the only signal a user gets.

**Ruling: fix it, characterize first.** Task 1 pins the current misaligned output in a test so the change is visible in the diff. The typed `LoggerParameter` aligns by construction — a parameter with no `<address>` gets empty `address`/`length` strings, one with no `<conversions>` gets an empty `conversions` vector, and the row count always matches. Task 9 removes the now-impossible length check. This is a deliberate behaviour change and gets its own commit message paragraph.

## File Structure

**Created — portable, all in `src/backend/logging/`:**

| File | Responsibility |
|---|---|
| `logger_definition_model.h` | Value types only: `Conversion`, `LoggerParameter`, `LoggerSwitch`, `LoggerDefinition`, `LoggerSelection`. No functions. |
| `logger_definition_parser.h` / `.cpp` | `parse_logger_definition(bytes::ByteView, std::string_view source)` — pure pugixml parse, no I/O. |
| `logger_definition_parser_test.cpp` | Well-formed, malformed, multiple conversions, missing attributes falling back to defaults, missing `<address>` / `<conversions>` children. |
| `logger_conf.h` / `.cpp` | `read_selection`, `write_selection`, `default_selection`, `initial_selection`. Pure; no I/O. |
| `logger_conf_test.cpp` | ECU found / absent / malformed; both caps walks; update-existing and append-new writes; the four-space byte-identity assertion. |
| `logger_definition_service.h` / `.cpp` | Holds `IFileRepository`, `IResourceBundle`, `IAtomicFileWriter`. Handle resolution incl. the CDBG fallback, and the read → parse → maybe-write composition. |
| `logger_definition_service_test.cpp` | Composition and the CDBG fallback, driven by the existing in-memory fakes. |

**Created — Qt-typed, in `src/backend/logging/` (same package as the portable targets, simply not registered portable):**

| File | Responsibility |
|---|---|
| `legacy_logger_adapter.h` / `.cpp` | Fans `LoggerDefinition` / `LoggerSelection` out into `LogValuesStructure`. Not portable. |
| `legacy_logger_adapter_test.cpp` | Fan-out fidelity, and the once-at-load contract (a selection round-trip leaves `log_value_enabled` untouched). |

**Why not `src/platform/desktop/common/logging/`:** `file_actions.cpp` (backend) calls the adapter, and `backend → platform` is a banned layering edge. `//src/backend/definition:legacy_definition_adapter` is the precedent — a Qt-typed `qt_cc_library` living in a backend package beside portable targets, depending on fine-grained `//src/backend/definitions:config_values` / `:ecu_cal_def` targets rather than the whole `definitions` god target. This plan follows it exactly.

**Created — the struct extraction that keeps the adapter out of a dependency cycle:**

| File | Responsibility |
|---|---|
| `src/backend/definitions/log_values.h` | `LogValuesStructure`, lifted out of `file_actions.h` into its own header and fine-grained Bazel target, exactly as `config_values.h` and `ecu_cal_def.h` already are. Without it the adapter can only reach the struct through the whole `//src/backend/definitions` target — which Task 7 makes depend on the adapter, closing a cycle. |

**Modified:**

| File | Change |
|---|---|
| `scripts/check-portable-closure.py:33-38` | `PORTABLE_ROOTS`'s `src/backend/logging` set gains one name per task, in the task that creates the target (2, 3, 4, 5). The adapter is **not** among them — it is Qt-typed by design. |
| `src/backend/logging/BUILD.bazel` | Four new `cc_library` targets, one `qt_cc_library` adapter, and their tests. |
| `src/backend/definitions/BUILD.bazel` | New `log_values` target; the `definitions` target gains the logger deps. |
| `src/backend/definitions/file_actions.h` | `LogValuesStructure` becomes a `using` alias of the extracted type; `log_value_units` → `log_value_conversions`; delete 7 dead fields, `log_values_by_protocol`, `dt_codes_structure`, `save_logger_conf`'s declaration. |
| `src/backend/definitions/file_actions.cpp` | Both logger methods delegate; delete the commented-out `save_logger_conf` body and the three `debugLog*` helpers. |
| `src/backend/definitions/file_actions_parsing_test.cpp` | Characterization tests (Task 1), then updated for the typed member (Task 9). |
| `src/backend/definitions/model_validation_test.cpp:213` | `log_value_units << "rpm,x,0"` becomes a typed seed. |
| `src/ui/desktop/logvalues.cpp:81,125` | Stride-7 walk → range-for over `conversions`. |
| `src/ui/desktop/mainwindow.cpp:2160,2199` | `.at(1)` → `conversions.at(0).units`. |
| `src/ui/desktop/log_operations_ssm.cpp:331,338` | Delete the `log_values_by_protocol` writes. |
| `src/platform/desktop/common/logging/logging_snapshot_adapter.cpp:46` | Positional fields → `c.units` / `c.expr` / `c.format`; the `size() < 4` guard becomes an empty check. |
| `src/platform/desktop/common/logging/logging_adapters_test.cpp:44` | `append_value_with_units` helper changes shape. |

**Deleted:** `FileActions::save_logger_conf` (`file_actions.h:212`, `file_actions.cpp:1050-1112` — already inside a `/* */` block), `dt_codes_structure` / `dt_codes_struct` (`file_actions.h:133-141`), `debugLogTransports` / `debugLogDtcodes` / `debugLogEcuparams` (`file_actions.cpp:26-119`), and eight `LogValuesStructure` fields: `log_value_from_byte`, `log_value_format`, `log_value_gauge_min`, `log_value_gauge_max`, `log_value_gauge_step`, `log_value_ecu_id`, `log_value_type`, `log_values_by_protocol`.

---

### Task 1: Characterize the current behaviour

No production change. This task pins what the tree does today so Tasks 7-9's diffs show exactly what changed. Per the step-4 and 5c precedent: characterize before converting.

**Files:**
- Modify: `src/backend/definitions/file_actions_parsing_test.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: the golden conf bytes that Task 4's byte-identity test asserts against.

- [ ] **Step 1: Pin the definition-time selection seeding**

Add to `file_actions_parsing_test.cpp`, following the existing `logger_definition_reads_parameter_and_switch` test's shape:

```cpp
    // Characterization (5d-5b Task 1): read_logger_definition_file seeds the
    // three selection lists with the FIRST N entries, ignoring `enabled`.
    // This is a different default from read_logger_conf's enabled-only walk.
    void logger_definition_seeds_selection_lists_ignoring_enabled()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString params;
        for (int i = 0; i < 20; i++)
        {
            params += QString(R"(<parameter id="P%1" name="N%1" desc="D%1" length="1" enabled="0">
    <address>0x10</address><conversions><conversion units="u" expr="x" format="0"/></conversions>
  </parameter>)").arg(i);
        }
        const QString path = writeTextFile(
            dir, "logger-seed.xml",
            R"(<logger><protocols><protocol id="SSM"><parameters>)" + params +
                R"(</parameters></protocol></protocols></logger>)");
        QVERIFY(!path.isEmpty());

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        actions.ConfigValuesStruct.romraider_logger_definition_file = path;

        FileActions::LogValuesStructure *values = actions.read_logger_definition_file();
        // Every parameter is enabled="0", yet the lists are seeded to their caps.
        QCOMPARE(values->dashboard_log_value_id.size(), 15);
        QCOMPARE(values->lower_panel_log_value_id.size(), 12);
        QCOMPARE(values->dashboard_log_value_id.at(0), QString("P0"));
        QCOMPARE(values->dashboard_log_value_id.at(14), QString("P14"));
        QCOMPARE(values->lower_panel_log_value_id.at(11), QString("P11"));
    }
```

- [ ] **Step 2: Pin the parallel-array misalignment**

```cpp
    // Characterization (5d-5b Task 1): a <parameter> with no <conversions>
    // child appends nothing to log_value_units, so the list runs SHORT of
    // log_value_id and every later row's parallel index is skewed by one.
    // Task 3's typed model aligns these by construction; this test is
    // rewritten in Task 9 to assert the aligned behaviour.
    void logger_definition_misaligns_rows_missing_optional_children()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTextFile(
            dir, "logger-skew.xml",
            R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" name="First" desc="d" length="1">
    <address>0x10</address>
  </parameter>
  <parameter id="P2" name="Second" desc="d" length="2">
    <address>0x20</address>
    <conversions><conversion units="rpm" expr="x" format="0.00"/></conversions>
  </parameter>
</parameters></protocol></protocols></logger>)");
        QVERIFY(!path.isEmpty());

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        actions.ConfigValuesStruct.romraider_logger_definition_file = path;

        FileActions::LogValuesStructure *values = actions.read_logger_definition_file();
        QCOMPARE(values->log_value_id.size(), 2);
        // The skew: one units entry for two rows, and it belongs to row 1.
        QCOMPARE(values->log_value_units.size(), 1);
        QCOMPARE(values->log_value_units.at(0),
                 QString("conversion 0,rpm,x,0.00,No gauge_min,No gauge_max,No gauge_step"));
        // Reading "row 0's units" therefore yields row 1's data.
        QVERIFY(!FileActions::validate_logger_values(*values));
    }
```

- [ ] **Step 3: Capture the QDom conf-writer golden**

`read_logger_conf` writes with `QDomDocument::save(output, 4)`. Task 4's `write_selection` must reproduce that byte-for-byte. Add a test that drives the ECU-not-found branch and prints the bytes, so they can be pasted into Task 4:

```cpp
    // Characterization (5d-5b Task 1): captures the exact bytes QDom writes,
    // which Task 4's write_selection must reproduce. The expected string below
    // was captured by running this test once with the QCOMPARE commented out
    // and qDebug()-ing `written`.
    void logger_conf_writes_four_space_indented_xml()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString conf = writeTextFile(
            dir, "logger.cfg", "<config><logger></logger></config>");
        QVERIFY(!conf.isEmpty());

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        actions.ConfigValuesStruct.logger_file = conf;
        FileActions::LogValuesStructure values;
        values.log_value_protocol << "SSM" << "SSM";
        values.log_value_id << "P1" << "P2";
        values.log_value_enabled << "1" << "0";
        values.log_switch_id << "S1";
        values.log_switch_enabled << "1";

        QVERIFY(actions.read_logger_conf(&values, "ECUID1", false) != nullptr);

        QFile file(conf);
        QVERIFY(file.open(QFile::ReadOnly | QFile::Text));
        const QString written = QString::fromUtf8(file.readAll());
        file.close();

        QVERIFY(written.contains(QStringLiteral("<ecu id=\"ECUID1\">")));
        QVERIFY(written.contains(QStringLiteral("<protocol id=\"SSM\">")));
        // Four-space indent, not a tab -- this is the fidelity risk.
        QVERIFY(written.contains(QStringLiteral("\n    <logger>")) ||
                written.contains(QStringLiteral("\n        <ecu")));
        QVERIFY(!written.contains(QChar('\t')));
        // Only the enabled parameter and switch are seeded here (enabled-only walk).
        QVERIFY(written.contains(QStringLiteral("<parameter id=\"P1\"")));
        QVERIFY(!written.contains(QStringLiteral("<parameter id=\"P2\"")));
    }
```

- [ ] **Step 4: Run the suite and record the captured bytes**

Run: `bazel test --config=release //src/backend/definitions:test_file_actions_parsing --test_output=all`
Expected: PASS. Copy the full `written` string from the test output into the task report — Task 4 needs it verbatim as its expected fixture. If any assertion fails, the captured shape is different from what this plan assumed: record the actual bytes and report it, do not adjust the production code.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definitions/file_actions_parsing_test.cpp
git commit -m "test: characterize logger definition and conf behaviour before 5d-5b

Pins three things the conversion must either preserve or deliberately
change: the first-N selection seeding that ignores enabled, the parallel
array skew when a <parameter> omits <conversions>, and the four-space
QDom indent the new pugixml writer has to reproduce."
```

---

### Task 2: The portable value model

**Files:**
- Create: `src/backend/logging/logger_definition_model.h`
- Modify: `src/backend/logging/BUILD.bazel`
- Modify: `scripts/check-portable-closure.py:33-38`

**Interfaces:**
- Consumes: nothing.
- Produces: `fastecu::logging::Conversion`, `LoggerParameter`, `LoggerSwitch`, `LoggerDefinition`, `LoggerSelection` — every later task depends on these exact field names.

- [ ] **Step 1: Write the model header**

Create `src/backend/logging/logger_definition_model.h`:

```cpp
#pragma once

#include <string>
#include <vector>

namespace fastecu::logging
{

// One <conversion> element. All fields stay strings: the expression is
// evaluated downstream by the expression evaluator, and the gauge bounds are
// presentation values the UI parses on demand.
struct Conversion
{
    std::string units;
    std::string expr;
    std::string format;
    std::string gauge_min;
    std::string gauge_max;
    std::string gauge_step;

    bool operator==(const Conversion&) const = default;
};

// One <parameter>. `protocol` stays a field rather than becoming a grouping
// key because every consumer filters with `log_value_protocol.at(j) ==
// protocol`; regrouping is step-6 work.
struct LoggerParameter
{
    std::string protocol;
    std::string id;
    std::string name;
    std::string description;
    std::string address;
    std::string length;
    std::string ecu_byte_index;
    std::string ecu_bit;
    std::string target;
    bool enabled{false};
    std::vector<Conversion> conversions;

    bool operator==(const LoggerParameter&) const = default;
};

struct LoggerSwitch
{
    std::string protocol;
    std::string id;
    std::string name;
    std::string description;
    std::string address;
    std::string ecu_byte_index;
    std::string ecu_bit;
    std::string target;
    // Always false out of the parser -- the logger definition XML carries no
    // switch `enabled` attribute, and the legacy parser hardcoded "0"
    // (file_actions.cpp:1261). It is overwritten at runtime from the ECU's
    // capability response, and Task 8 populates it from that live state so
    // default_selection can filter on it the way file_actions.cpp:990 does.
    bool enabled{false};

    bool operator==(const LoggerSwitch&) const = default;
};

struct LoggerDefinition
{
    std::vector<LoggerParameter> parameters;
    std::vector<LoggerSwitch> switches;

    bool operator==(const LoggerDefinition&) const = default;
};

// The user's per-ECU choice of what to display. Distinct from the definition:
// see the ownership table in the 5d-5 design doc.
struct LoggerSelection
{
    std::string protocol;
    std::vector<std::string> gauge_ids;
    std::vector<std::string> lower_panel_ids;
    std::vector<std::string> switch_ids;

    bool operator==(const LoggerSelection&) const = default;
};

} // namespace fastecu::logging
```

- [ ] **Step 2: Add the Bazel target**

In `src/backend/logging/BUILD.bazel`, add alongside the existing `logging_types` target (keep targets and their `deps` alphabetically ordered — buildifier will tell you if not):

```python
cc_library(
    name = "logger_definition_model",
    hdrs = ["logger_definition_model.h"],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 3: Register the portable roots**

In `scripts/check-portable-closure.py`, the `src/backend/logging` entry at line 33 currently reads:

```python
    ROOT / "src/backend/logging": {
        "logging_types",
        "logging_session",
        "logging_conversion",
        "logging_use_case",
    },
```

Add **only** `logger_definition_model` — the target that exists after this task:

```python
    ROOT / "src/backend/logging": {
        "logging_types",
        "logging_session",
        "logging_conversion",
        "logging_use_case",
        "logger_definition_model",
    },
```

**Register exactly one name per task, in the task that creates the target.** A `PORTABLE_ROOTS` entry is a *required* target, not a permission: `scripts/check-portable-closure.py` fails with `required portable target 'X' is missing` when a listed name has no target. Pre-registering the later names breaks `//:portable_closure` — and therefore the full gate — for every task until the last one lands. Tasks 3, 4 and 5 each add their own name.

The root `BUILD.bazel` genquery already lists `//src/backend/logging:BUILD.bazel` (line 136) — do **not** edit the root `BUILD.bazel`.

- [ ] **Step 4: Verify the closure guard still passes**

Run: `bazel test --config=release //:portable_closure`
Expected: PASS. A header-only target with no deps cannot pull Qt in; this step proves the registration is right before three more targets depend on it.

- [ ] **Step 5: Commit**

```bash
git add src/backend/logging/logger_definition_model.h \
        src/backend/logging/BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat: add the portable logger definition value model

Types only, plus the PORTABLE_ROOTS registration for this one target."
```

---

### Task 3: The portable logger-definition parser

**Files:**
- Create: `src/backend/logging/logger_definition_parser.h`, `logger_definition_parser.cpp`
- Test: `src/backend/logging/logger_definition_parser_test.cpp`
- Modify: `src/backend/logging/BUILD.bazel`

**Interfaces:**
- Consumes: `logger_definition_model.h` from Task 2.
- Produces: `Result<LoggerDefinition> parse_logger_definition(bytes::ByteView xml, std::string_view source)`.

**The attribute defaults are load-bearing.** The legacy parser substitutes literal placeholder strings when an attribute is absent, and downstream code compares against them. Reproduce them exactly:

| Element | Attribute | Default when absent |
|---|---|---|
| `<parameter>` | `id` | `"No id"` |
| `<parameter>` | `name` | `"No name"` |
| `<parameter>` | `desc` | `"No desc"` |
| `<parameter>` | `ecubyteindex` | `"No byte index"` |
| `<parameter>` | `ecubit` | `"No ecu bit"` |
| `<parameter>` | `target` | `"No target"` |
| `<parameter>` | `enabled` | `"0"` |
| `<parameter>` | `length` | `"1"` |
| `<conversion>` | `units` | `"#"` |
| `<conversion>` | `expr` | `"x"` |
| `<conversion>` | `format` | `"0.00"` |
| `<conversion>` | `gauge_min` | `"No gauge_min"` |
| `<conversion>` | `gauge_max` | `"No gauge_max"` |
| `<conversion>` | `gauge_step` | `"No gauge_step"` |
| `<switch>` | `id` | `"No id"` |
| `<switch>` | `name` | `"No name"` |
| `<switch>` | `desc` | `"No desc"` |
| `<switch>` | `byte` | `"No address"` |
| `<switch>` | `ecubyteindex` | `"No ecu byte index"` |
| `<switch>` | `bit` | `"No ecu bit"` |
| `<switch>` | `target` | `"No target"` |
| `<protocol>` | `id` | `"No protocol id"` |

Note the asymmetry, which is not a typo: `<parameter>` uses `ecubyteindex`/`ecubit`, `<switch>` uses `ecubyteindex`/`bit`, and the switch address attribute is `byte`. `enabled` is `"1"` for true and anything else for false.

- [ ] **Step 1: Write the failing test**

Create `src/backend/logging/logger_definition_parser_test.cpp`:

```cpp
#include "src/backend/logging/logger_definition_parser.h"

#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::logging::parse_logger_definition;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::SizeIs;

bytes::ByteView view(std::string_view text)
{
    return {reinterpret_cast<const bytes::Byte *>(text.data()), text.size()};
}

constexpr std::string_view kWellFormed = R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" name="Engine Speed" desc="RPM" length="2"
             ecubyteindex="3" ecubit="4" target="1" enabled="1">
    <address>0x1234</address>
    <conversions>
      <conversion units="rpm" expr="x*0.25" format="0.00"
                  gauge_min="0" gauge_max="8000" gauge_step="500"/>
      <conversion units="rps" expr="x/240" format="0.0"
                  gauge_min="0" gauge_max="133" gauge_step="10"/>
    </conversions>
  </parameter>
</parameters><switches>
  <switch id="S1" name="Test Switch" desc="flag" byte="0x20"
          ecubyteindex="7" bit="1" target="2"/>
</switches></protocol></protocols></logger>)";

TEST(LoggerDefinitionParser, ParsesParametersSwitchesAndConversions)
{
    const auto result = parse_logger_definition(view(kWellFormed), "test.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    ASSERT_THAT(result->parameters, SizeIs(1));
    const auto& p = result->parameters.at(0);
    EXPECT_EQ(p.protocol, "SSM");
    EXPECT_EQ(p.id, "P1");
    EXPECT_EQ(p.name, "Engine Speed");
    EXPECT_EQ(p.description, "RPM");
    EXPECT_EQ(p.address, "0x1234");
    EXPECT_EQ(p.length, "2");
    EXPECT_EQ(p.ecu_byte_index, "3");
    EXPECT_EQ(p.ecu_bit, "4");
    EXPECT_EQ(p.target, "1");
    EXPECT_TRUE(p.enabled);

    ASSERT_THAT(p.conversions, SizeIs(2));
    EXPECT_EQ(p.conversions.at(0).units, "rpm");
    EXPECT_EQ(p.conversions.at(0).expr, "x*0.25");
    EXPECT_EQ(p.conversions.at(0).format, "0.00");
    EXPECT_EQ(p.conversions.at(0).gauge_min, "0");
    EXPECT_EQ(p.conversions.at(0).gauge_max, "8000");
    EXPECT_EQ(p.conversions.at(0).gauge_step, "500");
    EXPECT_EQ(p.conversions.at(1).units, "rps");
    EXPECT_EQ(p.conversions.at(1).expr, "x/240");

    ASSERT_THAT(result->switches, SizeIs(1));
    const auto& s = result->switches.at(0);
    EXPECT_EQ(s.protocol, "SSM");
    EXPECT_EQ(s.id, "S1");
    EXPECT_EQ(s.name, "Test Switch");
    EXPECT_EQ(s.description, "flag");
    EXPECT_EQ(s.address, "0x20");
    EXPECT_EQ(s.ecu_byte_index, "7");
    EXPECT_EQ(s.ecu_bit, "1");
    EXPECT_EQ(s.target, "2");
}

TEST(LoggerDefinitionParser, SubstitutesLegacyPlaceholderDefaults)
{
    constexpr std::string_view kBare =
        R"(<logger><protocols><protocol><parameters>
  <parameter><address>0x1</address>
    <conversions><conversion/></conversions>
  </parameter>
</parameters><switches><switch/></switches></protocol></protocols></logger>)";

    const auto result = parse_logger_definition(view(kBare), "test.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;

    const auto& p = result->parameters.at(0);
    EXPECT_EQ(p.protocol, "No protocol id");
    EXPECT_EQ(p.id, "No id");
    EXPECT_EQ(p.name, "No name");
    EXPECT_EQ(p.description, "No desc");
    EXPECT_EQ(p.ecu_byte_index, "No byte index");
    EXPECT_EQ(p.ecu_bit, "No ecu bit");
    EXPECT_EQ(p.target, "No target");
    EXPECT_EQ(p.length, "1");
    EXPECT_FALSE(p.enabled);

    const auto& c = p.conversions.at(0);
    EXPECT_EQ(c.units, "#");
    EXPECT_EQ(c.expr, "x");
    EXPECT_EQ(c.format, "0.00");
    EXPECT_EQ(c.gauge_min, "No gauge_min");
    EXPECT_EQ(c.gauge_max, "No gauge_max");
    EXPECT_EQ(c.gauge_step, "No gauge_step");

    const auto& s = result->switches.at(0);
    EXPECT_EQ(s.id, "No id");
    EXPECT_EQ(s.address, "No address");
    EXPECT_EQ(s.ecu_byte_index, "No ecu byte index");
    EXPECT_EQ(s.ecu_bit, "No ecu bit");
}

// The fix ruled on in "Decisions locked": rows stay aligned even when a
// <parameter> omits <address> or <conversions>. The legacy parser skewed the
// parallel arrays here; see file_actions_parsing_test's characterization.
TEST(LoggerDefinitionParser, RowsStayAlignedWhenOptionalChildrenAreMissing)
{
    constexpr std::string_view kSparse =
        R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1"><address>0x10</address></parameter>
  <parameter id="P2">
    <conversions><conversion units="rpm"/></conversions>
  </parameter>
  <parameter id="P3"/>
</parameters></protocol></protocols></logger>)";

    const auto result = parse_logger_definition(view(kSparse), "test.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_THAT(result->parameters, SizeIs(3));

    EXPECT_EQ(result->parameters.at(0).address, "0x10");
    EXPECT_THAT(result->parameters.at(0).conversions, IsEmpty());
    EXPECT_EQ(result->parameters.at(1).address, "");
    EXPECT_THAT(result->parameters.at(1).conversions, SizeIs(1));
    EXPECT_EQ(result->parameters.at(2).address, "");
    EXPECT_THAT(result->parameters.at(2).conversions, IsEmpty());
}

TEST(LoggerDefinitionParser, CollectsParametersAcrossMultipleProtocols)
{
    constexpr std::string_view kTwo =
        R"(<logger><protocols>
  <protocol id="SSM"><parameters><parameter id="P1"/></parameters></protocol>
  <protocol id="CDBG"><parameters><parameter id="P2"/></parameters></protocol>
</protocols></logger>)";

    const auto result = parse_logger_definition(view(kTwo), "test.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_THAT(result->parameters,
                ElementsAre(Field(&fastecu::logging::LoggerParameter::protocol, "SSM"),
                            Field(&fastecu::logging::LoggerParameter::protocol, "CDBG")));
}

TEST(LoggerDefinitionParser, RejectsMalformedXml)
{
    const auto result = parse_logger_definition(view("<logger><protocols>"), "broken.xml");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("broken.xml"));
}

TEST(LoggerDefinitionParser, RejectsWrongRootElement)
{
    const auto result = parse_logger_definition(view("<config/>"), "wrong.xml");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(LoggerDefinitionParser, AcceptsAnEmptyButWellFormedDocument)
{
    const auto result = parse_logger_definition(view("<logger/>"), "empty.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_THAT(result->parameters, IsEmpty());
    EXPECT_THAT(result->switches, IsEmpty());
}

} // namespace
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bazel test --config=release //src/backend/logging:logger_definition_parser_test`
Expected: FAIL — the target does not exist yet (`no such target`). Add the BUILD entries in Step 3, then it fails to compile on the missing header, which is the real red.

- [ ] **Step 3: Write the header**

Create `src/backend/logging/logger_definition_parser.h`:

```cpp
#pragma once

#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

// Parses a RomRaider-style logger definition document. Pure: no file I/O and
// no configuration lookup -- the caller supplies the bytes and a `source`
// label used only to build error messages.
//
// Missing optional attributes fall back to the legacy placeholder strings
// ("No id", "No desc", "#", ...) that downstream comparisons still rely on.
// Rows are always aligned: a <parameter> without <address> or <conversions>
// yields empty fields, never a short vector.
Result<LoggerDefinition> parse_logger_definition(
    bytes::ByteView xml, std::string_view source);

} // namespace fastecu::logging
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/logging/logger_definition_parser.cpp`:

```cpp
#include "src/backend/logging/logger_definition_parser.h"

#include <format>
#include <string>

#include <pugixml.hpp>

namespace fastecu::logging
{
namespace
{

std::string attribute_or(pugi::xml_node node, const char *name, const char *fallback)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? attribute.value() : fallback;
}

Conversion parse_conversion(pugi::xml_node node)
{
    return Conversion{
        .units = attribute_or(node, "units", "#"),
        .expr = attribute_or(node, "expr", "x"),
        .format = attribute_or(node, "format", "0.00"),
        .gauge_min = attribute_or(node, "gauge_min", "No gauge_min"),
        .gauge_max = attribute_or(node, "gauge_max", "No gauge_max"),
        .gauge_step = attribute_or(node, "gauge_step", "No gauge_step"),
    };
}

LoggerParameter parse_parameter(pugi::xml_node node, std::string_view protocol)
{
    LoggerParameter parameter{
        .protocol = std::string(protocol),
        .id = attribute_or(node, "id", "No id"),
        .name = attribute_or(node, "name", "No name"),
        .description = attribute_or(node, "desc", "No desc"),
        .address = {},
        .length = {},
        .ecu_byte_index = attribute_or(node, "ecubyteindex", "No byte index"),
        .ecu_bit = attribute_or(node, "ecubit", "No ecu bit"),
        .target = attribute_or(node, "target", "No target"),
        .enabled = attribute_or(node, "enabled", "0") == "1",
        .conversions = {},
    };

    if (const pugi::xml_node address = node.child("address"))
    {
        parameter.address = address.child_value();
        parameter.length = attribute_or(node, "length", "1");
    }
    for (pugi::xml_node conversion : node.child("conversions").children("conversion"))
    {
        parameter.conversions.push_back(parse_conversion(conversion));
    }
    return parameter;
}

LoggerSwitch parse_switch(pugi::xml_node node, std::string_view protocol)
{
    return LoggerSwitch{
        .protocol = std::string(protocol),
        .id = attribute_or(node, "id", "No id"),
        .name = attribute_or(node, "name", "No name"),
        .description = attribute_or(node, "desc", "No desc"),
        .address = attribute_or(node, "byte", "No address"),
        .ecu_byte_index = attribute_or(node, "ecubyteindex", "No ecu byte index"),
        .ecu_bit = attribute_or(node, "bit", "No ecu bit"),
        .target = attribute_or(node, "target", "No target"),
        // Legacy parity: file_actions.cpp:1261 appends "0" unconditionally.
        // The definition file has no switch enabled attribute.
        .enabled = false,
    };
}

} // namespace

Result<LoggerDefinition> parse_logger_definition(
    bytes::ByteView xml, std::string_view source)
{
    pugi::xml_document document;
    const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size());
    if (!parsed)
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("{}: {} at offset {}", source, parsed.description(), parsed.offset));
    }

    const pugi::xml_node root = document.child("logger");
    if (!root)
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("{}: expected root element <logger>", source));
    }

    LoggerDefinition definition;
    for (pugi::xml_node protocol : root.child("protocols").children("protocol"))
    {
        const std::string protocol_id = attribute_or(protocol, "id", "No protocol id");
        for (pugi::xml_node parameter : protocol.child("parameters").children("parameter"))
        {
            definition.parameters.push_back(parse_parameter(parameter, protocol_id));
        }
        for (pugi::xml_node paramswitch : protocol.child("switches").children("switch"))
        {
            definition.switches.push_back(parse_switch(paramswitch, protocol_id));
        }
    }
    return definition;
}

} // namespace fastecu::logging
```

- [ ] **Step 5: Add the Bazel targets**

In `src/backend/logging/BUILD.bazel` (alphabetical deps):

```python
cc_library(
    name = "logger_definition_parser",
    srcs = ["logger_definition_parser.cpp"],
    hdrs = ["logger_definition_parser.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//src/algorithms/protocol",
        "//src/backend/logging:logger_definition_model",
        "//src/backend/ports",
        "@pugixml",
    ],
)

fastecu_portable_gtest(
    name = "logger_definition_parser_test",
    srcs = ["logger_definition_parser_test.cpp"],
    deps = [":logger_definition_parser"],
)
```

Then add `"logger_definition_parser"` to the `src/backend/logging` set in `scripts/check-portable-closure.py` — one name per task, in the task that creates the target.

Check the file's existing `load(...)` line already imports `fastecu_portable_gtest`; add it if not. Confirm the exact label for the bytes header and for pugixml against a package that already uses them — `src/backend/definition/BUILD.bazel` is the reference for `@pugixml`, and `grep -rn "protocol:bytes" src/*/BUILD.bazel` for the bytes label. **Use the labels the tree actually has, not the ones written above, if they differ.**

- [ ] **Step 6: Run tests to verify they pass**

Run: `bazel test --config=release //src/backend/logging:logger_definition_parser_test`
Expected: PASS, all seven tests.

Then: `bazel test --config=release //:portable_closure`
Expected: PASS — proves pugixml and the bytes header are Qt-free.

- [ ] **Step 7: Commit**

```bash
git add src/backend/logging/logger_definition_parser.h \
        src/backend/logging/logger_definition_parser.cpp \
        src/backend/logging/logger_definition_parser_test.cpp \
        src/backend/logging/BUILD.bazel
git commit -m "feat: add the portable logger definition parser

pugixml replaces the QDom walk. Legacy placeholder defaults are
reproduced exactly; rows are aligned by construction, which is the
deliberate fix for the parallel-array skew characterized in Task 1."
```

---

### Task 4: The portable conf operations

**Files:**
- Create: `src/backend/logging/logger_conf.h`, `logger_conf.cpp`
- Test: `src/backend/logging/logger_conf_test.cpp`
- Modify: `src/backend/logging/BUILD.bazel`

**Interfaces:**
- Consumes: `LoggerDefinition`, `LoggerSelection` from Task 2.
- Produces: `read_selection`, `write_selection`, `default_selection`, `initial_selection`.

**The caps, verbatim from the legacy code:**

| Function | Source | Gauges | Lower panel | Switches | Filter |
|---|---|---|---|---|---|
| `initial_selection` | `file_actions.cpp:1193-1200,1263-1266` | 15 | 12 | 20 | none |
| `default_selection` | `file_actions.cpp:972-994` | 15 | 12 | 20 | `enabled` only |

`default_selection`'s `protocol` is `definition.parameters.at(0).protocol` (legacy: `log_value_protocol.at(0)`); when there are no parameters it is empty and the caller treats that as the no-protocol case. `initial_selection` sets `protocol` the same way.

**The four-space indent is the fidelity risk.** pugixml's default indent is a tab; `QDomDocument::save(output, 4)` uses four spaces. `write_selection` must pass `"    "` to `save`.

- [ ] **Step 1: Write the failing test**

Create `src/backend/logging/logger_conf_test.cpp`:

```cpp
#include "src/backend/logging/logger_conf.h"

#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::logging::default_selection;
using fastecu::logging::initial_selection;
using fastecu::logging::LoggerDefinition;
using fastecu::logging::LoggerParameter;
using fastecu::logging::LoggerSelection;
using fastecu::logging::LoggerSwitch;
using fastecu::logging::read_selection;
using fastecu::logging::write_selection;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::SizeIs;

bytes::ByteView view(std::string_view text)
{
    return {reinterpret_cast<const bytes::Byte *>(text.data()), text.size()};
}

std::string text_of(bytes::ByteView data)
{
    return std::string(reinterpret_cast<const char *>(data.data()), data.size());
}

LoggerDefinition make_definition(int parameters, int switches, bool all_enabled)
{
    LoggerDefinition definition;
    for (int i = 0; i < parameters; i++)
    {
        LoggerParameter p;
        p.protocol = "SSM";
        p.id = std::format("P{}", i);
        p.enabled = all_enabled || (i % 2 == 0);
        definition.parameters.push_back(std::move(p));
    }
    for (int i = 0; i < switches; i++)
    {
        LoggerSwitch s;
        s.protocol = "SSM";
        s.id = std::format("S{}", i);
        definition.switches.push_back(std::move(s));
    }
    return definition;
}

constexpr std::string_view kConfWithEcu = R"(<config>
    <logger>
        <ecu id="ECUID1">
            <protocol id="SSM">
                <parameters>
                    <gauges>
                        <parameter id="P1" name=""/>
                        <parameter id="P2" name=""/>
                    </gauges>
                    <lower_panel>
                        <parameter id="P3" name=""/>
                    </lower_panel>
                </parameters>
                <switches>
                    <switch id="S1" name=""/>
                </switches>
            </protocol>
        </ecu>
    </logger>
</config>
)";

TEST(ReadSelection, ReturnsTheSelectionWhenTheEcuIsPresent)
{
    const auto result = read_selection(view(kConfWithEcu), "ECUID1", "conf.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ((*result)->protocol, "SSM");
    EXPECT_THAT((*result)->gauge_ids, ElementsAre("P1", "P2"));
    EXPECT_THAT((*result)->lower_panel_ids, ElementsAre("P3"));
    EXPECT_THAT((*result)->switch_ids, ElementsAre("S1"));
}

TEST(ReadSelection, ReturnsNulloptWhenTheEcuIsAbsent)
{
    const auto result = read_selection(view(kConfWithEcu), "OTHER", "conf.xml");
    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_FALSE(result->has_value());
}

TEST(ReadSelection, RejectsMalformedXml)
{
    const auto result = read_selection(view("<config><logger>"), "ECUID1", "broken.xml");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("broken.xml"));
}

TEST(InitialSelection, TakesTheFirstEntriesIgnoringEnabled)
{
    // Every parameter disabled: initial_selection still fills to the caps.
    LoggerDefinition definition = make_definition(30, 30, false);
    for (auto& p : definition.parameters)
    {
        p.enabled = false;
    }

    const LoggerSelection selection = initial_selection(definition);
    EXPECT_EQ(selection.protocol, "SSM");
    EXPECT_THAT(selection.gauge_ids, SizeIs(15));
    EXPECT_THAT(selection.lower_panel_ids, SizeIs(12));
    EXPECT_THAT(selection.switch_ids, SizeIs(20));
    EXPECT_EQ(selection.gauge_ids.front(), "P0");
    EXPECT_EQ(selection.gauge_ids.back(), "P14");
    EXPECT_EQ(selection.lower_panel_ids.back(), "P11");
    EXPECT_EQ(selection.switch_ids.back(), "S19");
}

TEST(DefaultSelection, WalksOnlyEnabledEntriesAndRespectsTheCaps)
{
    // make_definition enables even indices only when all_enabled is false.
    const LoggerDefinition definition = make_definition(60, 5, false);

    const LoggerSelection selection = default_selection(definition);
    EXPECT_THAT(selection.gauge_ids, SizeIs(15));
    EXPECT_THAT(selection.lower_panel_ids, SizeIs(12));
    EXPECT_EQ(selection.gauge_ids.at(0), "P0");
    EXPECT_EQ(selection.gauge_ids.at(1), "P2");
    EXPECT_EQ(selection.gauge_ids.back(), "P28");
}

TEST(DefaultSelection, YieldsAnEmptyProtocolForAnEmptyDefinition)
{
    const LoggerSelection selection = default_selection(LoggerDefinition{});
    EXPECT_THAT(selection.protocol, IsEmpty());
    EXPECT_THAT(selection.gauge_ids, IsEmpty());
}

TEST(WriteSelection, UpdatesAnExistingEcuElement)
{
    LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"X1", "X2"};
    selection.lower_panel_ids = {"X3"};
    selection.switch_ids = {"X4"};

    const auto written = write_selection(view(kConfWithEcu), "ECUID1", selection, "conf.xml");
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    const auto reread = read_selection(*written, "ECUID1", "conf.xml");
    ASSERT_TRUE(reread.has_value() && reread->has_value());
    EXPECT_THAT((*reread)->gauge_ids, ElementsAre("X1", "X2"));
    EXPECT_THAT((*reread)->lower_panel_ids, ElementsAre("X3"));
    EXPECT_THAT((*reread)->switch_ids, ElementsAre("X4"));
    // Exactly one <ecu> -- an update must not append a duplicate.
    EXPECT_EQ(text_of(*written).find("ECUID1"), text_of(*written).rfind("ECUID1"));
}

TEST(WriteSelection, AppendsANewEcuElementAndKeepsTheExistingOne)
{
    LoggerSelection selection;
    selection.protocol = "CDBG";
    selection.gauge_ids = {"Y1"};

    const auto written = write_selection(view(kConfWithEcu), "ECUID2", selection, "conf.xml");
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    const auto original = read_selection(*written, "ECUID1", "conf.xml");
    ASSERT_TRUE(original.has_value() && original->has_value());
    EXPECT_THAT((*original)->gauge_ids, ElementsAre("P1", "P2"));

    const auto added = read_selection(*written, "ECUID2", "conf.xml");
    ASSERT_TRUE(added.has_value() && added->has_value());
    EXPECT_EQ((*added)->protocol, "CDBG");
    EXPECT_THAT((*added)->gauge_ids, ElementsAre("Y1"));
}

// TRANSITIONAL (5d-5b): pins pugixml's output against the bytes
// QDomDocument::save(output, 4) produced, captured by Task 1's
// logger_conf_writes_four_space_indented_xml. This is the only place the old
// writer is treated as authoritative. The follow-up issue replaces it with a
// pugixml-generated golden plus the round-trip assertion above; the
// four-space indent choice itself stays.
TEST(WriteSelection, ReproducesTheFourSpaceQDomIndent)
{
    LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"P1"};
    selection.switch_ids = {"S1"};

    const auto written = write_selection(
        view("<config><logger/></config>"), "ECUID1", selection, "conf.xml");
    ASSERT_TRUE(written.has_value()) << written.error().detail;

    const std::string xml = text_of(*written);
    EXPECT_THAT(xml, Not(HasSubstr("\t")));
    EXPECT_THAT(xml, HasSubstr("\n    <logger>"));
    EXPECT_THAT(xml, HasSubstr("\n        <ecu id=\"ECUID1\">"));
    EXPECT_THAT(xml, HasSubstr("\n            <protocol id=\"SSM\">"));
}

} // namespace
```

**Strengthen the last test before you run it.** As written above it is four
`HasSubstr` checks plus a `Not(HasSubstr("\t"))` that can never fail against a
space-indented document — Task 1's reviewer flagged the same weakness in the
characterization test it descends from. Task 1's report
(`.superpowers/sdd/2026-08-06-step5d5b-logger-definition-glue/task-1-report.md`)
contains the **complete 537-byte golden** that `QDomDocument::save(output, 4)`
produced, verbatim. Read it and replace the substring checks with a single
`EXPECT_EQ(xml, kQDomGolden)` against a raw-string constant holding those exact
bytes, so the committed test — not just a report file — is what pins the
fidelity risk.

The golden was captured from a different input than this test's, so build the
test's fixture to match the one Task 1 used (its report documents the
`LogValuesStructure` it seeded and the starting conf document). If reproducing
that input exactly through `write_selection`'s signature is not possible,
say so in your report and keep the strongest assertion you can — a full
`EXPECT_EQ` against a golden you capture from **this** test's own pugixml
output is not acceptable, because it would pin nothing about QDom.

- [ ] **Step 2: Run test to verify it fails**

Run: `bazel test --config=release //src/backend/logging:logger_conf_test`
Expected: FAIL — target missing, then compile failure on the missing header.

- [ ] **Step 3: Write the header**

Create `src/backend/logging/logger_conf.h`:

```cpp
#pragma once

#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

// The three operations the legacy read_logger_conf fused behind its `modify`
// flag, split apart. All pure: the caller supplies and stores the bytes.

// Returns nullopt when `ecu_id` has no <ecu> element -- it does not
// initialize anything. Composing a default and writing it is the service's
// job, so the write is an explicit step rather than a side effect of a read.
Result<std::optional<LoggerSelection>> read_selection(
    bytes::ByteView conf, std::string_view ecu_id, std::string_view source);

// Updates `ecu_id`'s <ecu> element in place, or appends one if absent, and
// returns the whole re-serialized document. Four-space indented to match what
// QDomDocument::save(output, 4) wrote, so an existing conf file does not
// reflow wholesale on first write.
Result<bytes::Bytes> write_selection(
    bytes::ByteView conf,
    std::string_view ecu_id,
    const LoggerSelection& selection,
    std::string_view source);

// The first-N walk read_logger_definition_file performs at parse time:
// 15 gauges / 12 lower-panel / 20 switches, ignoring `enabled`.
LoggerSelection initial_selection(const LoggerDefinition& definition);

// The enabled-only walk read_logger_conf performs when the ECU id is absent,
// at the same caps.
LoggerSelection default_selection(const LoggerDefinition& definition);

} // namespace fastecu::logging
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/logging/logger_conf.cpp`:

```cpp
#include "src/backend/logging/logger_conf.h"

#include <format>
#include <sstream>
#include <string>

#include <pugixml.hpp>

namespace fastecu::logging
{
namespace
{

// QDomDocument::save(output, 4) wrote four spaces per level. pugixml defaults
// to a tab; matching the old indent spares every existing user a one-time
// whole-file reflow on the first write.
constexpr const char *kIndent = "    ";

constexpr std::size_t kGaugeCap = 15;
constexpr std::size_t kLowerPanelCap = 12;
constexpr std::size_t kSwitchCap = 20;

Status load(pugi::xml_document& document, bytes::ByteView conf, std::string_view source)
{
    const pugi::xml_parse_result parsed = document.load_buffer(conf.data(), conf.size());
    if (!parsed)
    {
        return fail(
            ErrorKind::InvalidConfig,
            std::format("{}: {} at offset {}", source, parsed.description(), parsed.offset));
    }
    return {};
}

pugi::xml_node find_ecu(pugi::xml_node logger, std::string_view ecu_id)
{
    for (pugi::xml_node ecu : logger.children("ecu"))
    {
        if (ecu.attribute("id").value() == ecu_id)
        {
            return ecu;
        }
    }
    return {};
}

void append_ids(
    pugi::xml_node parent,
    const char *element_name,
    const std::vector<std::string>& ids)
{
    for (const std::string& id : ids)
    {
        pugi::xml_node node = parent.append_child(element_name);
        node.append_attribute("id") = id.c_str();
        node.append_attribute("name") = "";
    }
}

std::vector<std::string> collect_ids(pugi::xml_node parent, const char *element_name)
{
    std::vector<std::string> ids;
    for (pugi::xml_node node : parent.children(element_name))
    {
        ids.emplace_back(node.attribute("id").as_string("No id"));
    }
    return ids;
}

LoggerSelection walk(const LoggerDefinition& definition, bool enabled_only)
{
    LoggerSelection selection;
    if (!definition.parameters.empty())
    {
        selection.protocol = definition.parameters.front().protocol;
    }
    for (const LoggerParameter& parameter : definition.parameters)
    {
        if (enabled_only && !parameter.enabled)
        {
            continue;
        }
        if (selection.gauge_ids.size() < kGaugeCap)
        {
            selection.gauge_ids.push_back(parameter.id);
        }
        if (selection.lower_panel_ids.size() < kLowerPanelCap)
        {
            selection.lower_panel_ids.push_back(parameter.id);
        }
    }
    for (const LoggerSwitch& paramswitch : definition.switches)
    {
        // file_actions.cpp:990 gates this walk on log_switch_enabled == "1";
        // file_actions.cpp:1263-1266's first-N walk does not.
        if (enabled_only && !paramswitch.enabled)
        {
            continue;
        }
        if (selection.switch_ids.size() < kSwitchCap)
        {
            selection.switch_ids.push_back(paramswitch.id);
        }
    }
    return selection;
}

} // namespace

Result<std::optional<LoggerSelection>> read_selection(
    bytes::ByteView conf, std::string_view ecu_id, std::string_view source)
{
    pugi::xml_document document;
    if (auto loaded = load(document, conf, source); !loaded)
    {
        return std::unexpected(loaded.error());
    }

    const pugi::xml_node ecu = find_ecu(document.child("config").child("logger"), ecu_id);
    if (!ecu)
    {
        return std::optional<LoggerSelection>{};
    }

    LoggerSelection selection;
    const pugi::xml_node protocol = ecu.child("protocol");
    selection.protocol = protocol.attribute("id").as_string("No id");
    const pugi::xml_node parameters = protocol.child("parameters");
    selection.gauge_ids = collect_ids(parameters.child("gauges"), "parameter");
    selection.lower_panel_ids = collect_ids(parameters.child("lower_panel"), "parameter");
    selection.switch_ids = collect_ids(protocol.child("switches"), "switch");
    return std::optional<LoggerSelection>{std::move(selection)};
}

Result<bytes::Bytes> write_selection(
    bytes::ByteView conf,
    std::string_view ecu_id,
    const LoggerSelection& selection,
    std::string_view source)
{
    pugi::xml_document document;
    if (auto loaded = load(document, conf, source); !loaded)
    {
        return std::unexpected(loaded.error());
    }

    pugi::xml_node config = document.child("config");
    if (!config)
    {
        config = document.append_child("config");
    }
    pugi::xml_node logger = config.child("logger");
    if (!logger)
    {
        logger = config.append_child("logger");
    }

    // Rebuild rather than patch attributes by index. The legacy writer walked
    // existing elements and set their `id` positionally, which silently
    // dropped ids when the selection was longer than the stored subtree.
    pugi::xml_node ecu = find_ecu(logger, ecu_id);
    if (ecu)
    {
        logger.remove_child(ecu);
    }
    ecu = logger.append_child("ecu");
    ecu.append_attribute("id") = std::string(ecu_id).c_str();

    pugi::xml_node protocol = ecu.append_child("protocol");
    protocol.append_attribute("id") = selection.protocol.c_str();
    pugi::xml_node parameters = protocol.append_child("parameters");
    append_ids(parameters.append_child("gauges"), "parameter", selection.gauge_ids);
    append_ids(parameters.append_child("lower_panel"), "parameter", selection.lower_panel_ids);
    append_ids(protocol.append_child("switches"), "switch", selection.switch_ids);

    std::ostringstream output;
    document.save(output, kIndent, pugi::format_default, pugi::encoding_utf8);
    const std::string xml = std::move(output).str();
    return bytes::Bytes(xml.begin(), xml.end());
}

LoggerSelection initial_selection(const LoggerDefinition& definition)
{
    return walk(definition, /*enabled_only=*/false);
}

LoggerSelection default_selection(const LoggerDefinition& definition)
{
    return walk(definition, /*enabled_only=*/true);
}

} // namespace fastecu::logging
```

- [ ] **Step 5: Add the Bazel targets**

```python
cc_library(
    name = "logger_conf",
    srcs = ["logger_conf.cpp"],
    hdrs = ["logger_conf.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//src/algorithms/protocol",
        "//src/backend/logging:logger_definition_model",
        "//src/backend/ports",
        "@pugixml",
    ],
)

fastecu_portable_gtest(
    name = "logger_conf_test",
    srcs = ["logger_conf_test.cpp"],
    deps = [":logger_conf"],
)
```

Then add `"logger_conf"` to the `src/backend/logging` set in `scripts/check-portable-closure.py`.

- [ ] **Step 6: Run tests to verify they pass**

Run: `bazel test --config=release //src/backend/logging:logger_conf_test`
Expected: PASS, all nine tests.

If `ReproducesTheFourSpaceQDomIndent` fails on the exact nesting, compare against Task 1's captured bytes and fix the **test's** expectation to match those bytes — then confirm the indent string is still `"    "`. Do not change `kIndent`.

- [ ] **Step 7: Commit**

```bash
git add src/backend/logging/logger_conf.h src/backend/logging/logger_conf.cpp \
        src/backend/logging/logger_conf_test.cpp src/backend/logging/BUILD.bazel
git commit -m "feat: add the portable logger conf operations

Splits read_logger_conf's three fused behaviours apart: read_selection
returns nullopt rather than initializing, initial_selection and
default_selection are the two distinct seeding walks the legacy code
performed in different places, and write_selection rebuilds the <ecu>
subtree instead of patching attributes positionally.

Four-space format_indent matches QDomDocument::save(output, 4) so
existing conf files do not reflow on first write."
```

---

### Task 5: The logger definition service

**Files:**
- Create: `src/backend/logging/logger_definition_service.h`, `logger_definition_service.cpp`
- Test: `src/backend/logging/logger_definition_service_test.cpp`
- Modify: `src/backend/logging/BUILD.bazel`

**Interfaces:**
- Consumes: `parse_logger_definition` (Task 3); `read_selection`, `write_selection`, `default_selection` (Task 4); `IFileRepository`, `IResourceBundle`, `IAtomicFileWriter` from `//src/backend/ports`.
- Produces: `LoggerDefinitionService` with `resolve_definition_handle`, `load_definition`, `load_or_initialize_selection`, `save_selection`.

**The CDBG fallback**, currently inline at `file_actions.cpp:1122-1130`: when the configured logger definition handle is empty **and** the selected log protocol is `"CDBG"`, prefer `<config_files_directory>logger_cdbg_example.xml` if it exists, else the bundled resource `":/config/logger_cdbg_example.xml"`. The service resolves and returns the handle; writing it back into `configValues->romraider_logger_definition_file` stays the legacy adapter's job (Task 7).

- [ ] **Step 1: Write the failing test**

Create `src/backend/logging/logger_definition_service_test.cpp`:

```cpp
#include "src/backend/logging/logger_definition_service.h"

#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"

namespace
{

using fastecu::logging::LoggerDefinitionService;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::SizeIs;

constexpr std::string_view kDefinition =
    R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" enabled="1"><address>0x1</address></parameter>
  <parameter id="P2" enabled="0"><address>0x2</address></parameter>
</parameters><switches><switch id="S1"/></switches></protocol></protocols></logger>)";

constexpr std::string_view kConfWithEcu =
    R"(<config><logger><ecu id="ECUID1"><protocol id="SSM"><parameters>
  <gauges><parameter id="P2" name=""/></gauges>
  <lower_panel><parameter id="P2" name=""/></lower_panel>
</parameters><switches><switch id="S1" name=""/></switches></protocol></ecu></logger></config>)";

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {text.begin(), text.end()};
}

class LoggerDefinitionServiceTest : public ::testing::Test
{
  protected:
    fastecu::InMemoryFileRepository repository_;
    fastecu::InMemoryResourceBundle bundle_;
    fastecu::InMemoryAtomicFileWriter writer_;

    LoggerDefinitionService service()
    {
        return LoggerDefinitionService(repository_, bundle_, writer_);
    }
};

TEST_F(LoggerDefinitionServiceTest, LoadsAndParsesTheConfiguredHandle)
{
    repository_.files["logger.xml"] = bytes_of(kDefinition);

    const auto definition = service().load_definition("logger.xml");
    ASSERT_TRUE(definition.has_value()) << definition.error().detail;
    EXPECT_THAT(definition->parameters, SizeIs(2));
    EXPECT_THAT(definition->switches, SizeIs(1));
}

TEST_F(LoggerDefinitionServiceTest, PropagatesAReadFailure)
{
    const auto definition = service().load_definition("missing.xml");
    ASSERT_FALSE(definition.has_value());
    EXPECT_EQ(definition.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST_F(LoggerDefinitionServiceTest, ResolvesTheConfiguredHandleUnchanged)
{
    const auto handle = service().resolve_definition_handle(
        "configured.xml", "CDBG", "/home/u/.FastECU/");
    ASSERT_TRUE(handle.has_value()) << handle.error().detail;
    EXPECT_EQ(*handle, "configured.xml");
}

TEST_F(LoggerDefinitionServiceTest, PrefersTheConfigDirCdbgExampleWhenPresent)
{
    repository_.files["/home/u/.FastECU/logger_cdbg_example.xml"] =
        bytes_of(kDefinition);

    const auto handle = service().resolve_definition_handle("", "CDBG", "/home/u/.FastECU/");
    ASSERT_TRUE(handle.has_value()) << handle.error().detail;
    EXPECT_EQ(*handle, "/home/u/.FastECU/logger_cdbg_example.xml");
}

TEST_F(LoggerDefinitionServiceTest, FallsBackToTheBundledCdbgExample)
{
    // Config-dir file absent; the bundled resource is the only source.
    const auto handle = service().resolve_definition_handle("", "CDBG", "/home/u/.FastECU/");
    ASSERT_TRUE(handle.has_value()) << handle.error().detail;
    EXPECT_EQ(*handle, ":/config/logger_cdbg_example.xml");
}

TEST_F(LoggerDefinitionServiceTest, LeavesTheHandleEmptyForNonCdbgProtocols)
{
    const auto handle = service().resolve_definition_handle("", "SSM", "/home/u/.FastECU/");
    ASSERT_TRUE(handle.has_value()) << handle.error().detail;
    EXPECT_THAT(*handle, ::testing::IsEmpty());
}

TEST_F(LoggerDefinitionServiceTest, LoadsAnExistingSelectionWithoutWriting)
{
    repository_.files["logger.cfg"] = bytes_of(kConfWithEcu);
    const auto definition = fastecu::logging::LoggerDefinition{};

    const auto selection =
        service().load_or_initialize_selection("logger.cfg", "ECUID1", definition);
    ASSERT_TRUE(selection.has_value()) << selection.error().detail;
    EXPECT_THAT(selection->gauge_ids, ElementsAre("P2"));
    EXPECT_TRUE(writer_.replace_calls.empty()) << "reading must not write";
}

TEST_F(LoggerDefinitionServiceTest, InitializesAndPersistsWhenTheEcuIsAbsent)
{
    repository_.files["logger.cfg"] = bytes_of("<config><logger/></config>");
    const auto parsed = fastecu::logging::parse_logger_definition(
        bytes::ByteView(reinterpret_cast<const bytes::Byte *>(kDefinition.data()),
                        kDefinition.size()),
        "logger.xml");
    ASSERT_TRUE(parsed.has_value());

    const auto selection =
        service().load_or_initialize_selection("logger.cfg", "NEWECU", *parsed);
    ASSERT_TRUE(selection.has_value()) << selection.error().detail;
    // Enabled-only walk: P1 is enabled, P2 is not.
    EXPECT_THAT(selection->gauge_ids, ElementsAre("P1"));
    ASSERT_THAT(writer_.replace_calls, SizeIs(1)) << "the default must be persisted";
    EXPECT_EQ(writer_.replace_calls.at(0).handle, "logger.cfg");
}

TEST_F(LoggerDefinitionServiceTest, SaveSelectionReplacesTheFileAtomically)
{
    repository_.files["logger.cfg"] = bytes_of(kConfWithEcu);
    fastecu::logging::LoggerSelection selection;
    selection.protocol = "SSM";
    selection.gauge_ids = {"P9"};

    const auto status = service().save_selection("logger.cfg", "ECUID1", selection);
    ASSERT_TRUE(status.has_value()) << status.error().detail;
    ASSERT_THAT(writer_.replace_calls, SizeIs(1));
    const auto& written = writer_.replace_calls.at(0).data;
    EXPECT_THAT(std::string(written.begin(), written.end()), HasSubstr("P9"));
}

} // namespace
```

The fake APIs above were checked against the tree: all three live in namespace `fastecu` (not `fastecu::testing`), `InMemoryFileRepository` exposes a public `files` map plus `read_errors` / `read_handles` / `read_count()`, and `InMemoryAtomicFileWriter` records a public `replace_calls` vector of `ReplaceCall{handle, data}`. A handle absent from `files` makes `read` return `InvalidConfig` with detail `"no such handle"`, which is exactly what the CDBG-fallback test needs. **Read the three headers before writing the test anyway, and if anything differs, the headers govern — adapt the test, never extend a fake.**

- [ ] **Step 2: Run test to verify it fails**

Run: `bazel test --config=release //src/backend/logging:logger_definition_service_test`
Expected: FAIL — missing target, then missing header.

- [ ] **Step 3: Write the header**

Create `src/backend/logging/logger_definition_service.h`:

```cpp
#pragma once

#include <string>
#include <string_view>

#include "src/backend/logging/logger_conf.h"
#include "src/backend/logging/logger_definition_model.h"
#include "src/backend/logging/logger_definition_parser.h"
#include "src/backend/ports/atomic_file_writer.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/resource_bundle.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

// Owns handle resolution and the read -> parse -> maybe-write composition for
// the logger definition and conf files. Portable: every I/O path goes through
// an injected port.
class LoggerDefinitionService
{
  public:
    LoggerDefinitionService(IFileRepository&, IResourceBundle&, IAtomicFileWriter&);

    // Returns `configured_handle` unchanged when it is non-empty. When it is
    // empty and `log_protocol` is "CDBG", prefers
    // <config_files_directory>logger_cdbg_example.xml and falls back to the
    // bundled resource. Otherwise returns an empty handle -- the caller
    // reports "no logger definition file selected".
    Result<std::string> resolve_definition_handle(
        std::string_view configured_handle,
        std::string_view log_protocol,
        std::string_view config_files_directory);

    Result<LoggerDefinition> load_definition(std::string_view handle);

    // Reads the conf file's selection for `ecu_id`. When that ECU has no
    // entry, composes default_selection(definition) and persists it, so the
    // write is an explicit step rather than a side effect of a read.
    Result<LoggerSelection> load_or_initialize_selection(
        std::string_view conf_handle,
        std::string_view ecu_id,
        const LoggerDefinition& definition);

    Status save_selection(
        std::string_view conf_handle,
        std::string_view ecu_id,
        const LoggerSelection& selection);

  private:
    IFileRepository& repository_;
    IResourceBundle& bundle_;
    IAtomicFileWriter& writer_;
};

} // namespace fastecu::logging
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/logging/logger_definition_service.cpp`:

```cpp
#include "src/backend/logging/logger_definition_service.h"

#include <format>

namespace fastecu::logging
{
namespace
{

constexpr std::string_view kCdbgProtocol = "CDBG";
constexpr std::string_view kCdbgExampleName = "logger_cdbg_example.xml";
constexpr std::string_view kBundledCdbgExample = ":/config/logger_cdbg_example.xml";

} // namespace

LoggerDefinitionService::LoggerDefinitionService(
    IFileRepository& repository, IResourceBundle& bundle, IAtomicFileWriter& writer)
    : repository_(repository), bundle_(bundle), writer_(writer)
{
}

Result<std::string> LoggerDefinitionService::resolve_definition_handle(
    std::string_view configured_handle,
    std::string_view log_protocol,
    std::string_view config_files_directory)
{
    if (!configured_handle.empty())
    {
        return std::string(configured_handle);
    }
    if (log_protocol != kCdbgProtocol)
    {
        return std::string{};
    }

    const std::string user_copy = std::format("{}{}", config_files_directory, kCdbgExampleName);
    if (repository_.read(user_copy).has_value())
    {
        return user_copy;
    }
    return std::string(kBundledCdbgExample);
}

Result<LoggerDefinition> LoggerDefinitionService::load_definition(std::string_view handle)
{
    auto contents = repository_.read(handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    return parse_logger_definition(*contents, handle);
}

Result<LoggerSelection> LoggerDefinitionService::load_or_initialize_selection(
    std::string_view conf_handle,
    std::string_view ecu_id,
    const LoggerDefinition& definition)
{
    auto contents = repository_.read(conf_handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }

    auto stored = read_selection(*contents, ecu_id, conf_handle);
    if (!stored)
    {
        return std::unexpected(stored.error());
    }
    if (stored->has_value())
    {
        return std::move(**stored);
    }

    LoggerSelection selection = default_selection(definition);
    auto written = write_selection(*contents, ecu_id, selection, conf_handle);
    if (!written)
    {
        return std::unexpected(written.error());
    }
    if (auto replaced = writer_.replace(conf_handle, *written); !replaced)
    {
        return std::unexpected(replaced.error());
    }
    return selection;
}

Status LoggerDefinitionService::save_selection(
    std::string_view conf_handle,
    std::string_view ecu_id,
    const LoggerSelection& selection)
{
    auto contents = repository_.read(conf_handle);
    if (!contents)
    {
        return std::unexpected(contents.error());
    }
    auto written = write_selection(*contents, ecu_id, selection, conf_handle);
    if (!written)
    {
        return std::unexpected(written.error());
    }
    return writer_.replace(conf_handle, *written);
}

} // namespace fastecu::logging
```

Note `bundle_` is held but unused by the code above: the bundled-resource case returns the `":/config/..."` handle for the repository to resolve, matching how the legacy code passes that string to `QFile`. If reading the bundled example through `IResourceBundle` turns out to be required (the repository fake rejects `:/` handles), read it via `bundle_.read("config", kCdbgExampleName)` in `load_definition` instead and say so in the report — do not silently drop the member.

- [ ] **Step 5: Add the Bazel targets**

```python
cc_library(
    name = "logger_definition_service",
    srcs = ["logger_definition_service.cpp"],
    hdrs = ["logger_definition_service.h"],
    visibility = ["//visibility:public"],
    deps = [
        "//src/backend/logging:logger_conf",
        "//src/backend/logging:logger_definition_model",
        "//src/backend/logging:logger_definition_parser",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "logger_definition_service_test",
    srcs = ["logger_definition_service_test.cpp"],
    deps = [
        ":logger_definition_service",
        "//src/backend/ports/testing:in_memory_atomic_file_writer",
        "//src/backend/ports/testing:in_memory_file_repository",
        "//src/backend/ports/testing:in_memory_resource_bundle",
    ],
)
```

Then add `"logger_definition_service"` to the `src/backend/logging` set in `scripts/check-portable-closure.py`.

- [ ] **Step 6: Run tests and the closure guard**

Run: `bazel test --config=release //src/backend/logging:all //:portable_closure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/backend/logging/logger_definition_service.h \
        src/backend/logging/logger_definition_service.cpp \
        src/backend/logging/logger_definition_service_test.cpp \
        src/backend/logging/BUILD.bazel
git commit -m "feat: add the portable logger definition service

Owns handle resolution -- including the CDBG example fallback that was
inline in read_logger_definition_file -- and the read/parse/maybe-write
composition. Initializing a missing ECU's selection now goes through
IAtomicFileWriter::replace instead of truncating the open file before
serializing, so a failed write can no longer destroy a user's config."
```

---

### Task 6: The legacy fan-out adapter

**Files:**
- Create: `src/backend/definitions/log_values.h`
- Create: `src/backend/logging/legacy_logger_adapter.h`, `legacy_logger_adapter.cpp`
- Test: `src/backend/logging/legacy_logger_adapter_test.cpp`
- Modify: `src/backend/definitions/file_actions.h:84-131`, `src/backend/definitions/BUILD.bazel`, `src/backend/logging/BUILD.bazel`

**Interfaces:**
- Consumes: `LoggerDefinition`, `LoggerSelection` (Task 2).
- Produces: `fastecu::definitions::LogValuesStructure` (extracted, unchanged fields), plus `fastecu::logging::apply_definition(const LoggerDefinition&, definitions::LogValuesStructure&)` and `apply_selection(const LoggerSelection&, definitions::LogValuesStructure&)`.

**The once-at-load contract.** `log_value_enabled` and `log_switch_enabled` carry the XML default at parse time and are then **overwritten at runtime** by `log_operations_ssm.cpp:348-360` from the ECU's capability response. If `apply_definition` ran a second time after logging started, it would silently reset every capability flag the ECU reported. Therefore: `apply_definition` is called exactly once, at load, and `apply_selection` must never touch a definition field. This task's test asserts the second half.

This task keeps `log_value_units` as-is (the packed string) so the tree keeps compiling; Task 9 replaces it.

- [ ] **Step 1: Extract `LogValuesStructure` into its own header and target**

The adapter needs the struct, and Task 7 makes `//src/backend/definitions` depend on the adapter — so reaching the struct through the whole `definitions` target would close a cycle. Break it exactly the way `config_values.h` and `ecu_cal_def.h` already are.

Create `src/backend/definitions/log_values.h`: `#pragma once`, the includes `<QString>` and `<QStringList>`, and the **verbatim** body of `struct LogValuesStructure` from `file_actions.h:84-131`, wrapped in `namespace fastecu::definitions`. Change nothing about the fields in this step — the dead-field removal is Task 10 and the type change is Task 9.

In `file_actions.h`, replace the struct definition with an alias and a member, matching how the file already handles `EcuCalDefStructure` at line 143:

```cpp
    using LogValuesStructure = fastecu::definitions::LogValuesStructure;
    LogValuesStructure LogValuesStruct;
```

and add `#include "src/backend/definitions/log_values.h"`. Every existing `FileActions::LogValuesStructure` spelling keeps working through the alias, so no call site changes.

In `src/backend/definitions/BUILD.bazel`, add a `log_values` target beside the existing `config_values` / `ecu_cal_def` targets, copying their shape (visibility included — it must at least expose `//src/backend/logging:__pkg__`), and add `:log_values` to the `definitions` target's deps in alphabetical order.

- [ ] **Step 2: Verify the extraction is behaviour-neutral**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test --config=release //src/backend/definitions:all
```
Expected: PASS with no test changes at all. A pure header move that needs a test edit is not a pure header move — stop and report if one is required.

- [ ] **Step 3: Write the failing test**

Create `src/backend/logging/legacy_logger_adapter_test.cpp`:

```cpp
#include "src/backend/logging/legacy_logger_adapter.h"

#include <QtTest>

#include "src/backend/logging/logger_definition_model.h"

class LegacyLoggerAdapterTest : public QObject
{
    Q_OBJECT

  private slots:
    void definition_fans_out_into_parallel_arrays()
    {
        fastecu::logging::LoggerDefinition definition;
        fastecu::logging::LoggerParameter p;
        p.protocol = "SSM";
        p.id = "P1";
        p.name = "Engine Speed";
        p.description = "RPM";
        p.address = "0x1234";
        p.length = "2";
        p.ecu_byte_index = "3";
        p.ecu_bit = "4";
        p.target = "1";
        p.enabled = true;
        p.conversions.push_back({"rpm", "x*0.25", "0.00", "0", "8000", "500"});
        definition.parameters.push_back(p);

        fastecu::logging::LoggerSwitch s;
        s.protocol = "SSM";
        s.id = "S1";
        s.name = "Test Switch";
        s.address = "0x20";
        s.ecu_bit = "1";
        definition.switches.push_back(s);

        FileActions::LogValuesStructure values;
        fastecu::logging::apply_definition(definition, values);

        QCOMPARE(values.log_value_id.size(), 1);
        QCOMPARE(values.log_value_protocol.at(0), QString("SSM"));
        QCOMPARE(values.log_value_name.at(0), QString("Engine Speed"));
        QCOMPARE(values.log_value_address.at(0), QString("0x1234"));
        QCOMPARE(values.log_value_length.at(0), QString("2"));
        QCOMPARE(values.log_value_enabled.at(0), QString("1"));
        QCOMPARE(values.log_value.at(0), QString("0.00"));
        QCOMPARE(values.log_switch_id.at(0), QString("S1"));
        QCOMPARE(values.log_switch_address.at(0), QString("0x20"));
        QCOMPARE(values.log_switch_state.at(0), QString("0"));
        // Every parallel array has the same length -- the alignment fix.
        QCOMPARE(values.log_value_protocol.size(), values.log_value_id.size());
        QCOMPARE(values.log_value_address.size(), values.log_value_id.size());
        QCOMPARE(values.log_value_length.size(), values.log_value_id.size());
    }

    // The ownership contract from the 5d-5 design doc: applying a selection
    // must never rewrite definition state. log_value_enabled is overwritten at
    // runtime from the ECU's capability response; re-fanning would reset it.
    void selection_round_trip_leaves_enabled_flags_untouched()
    {
        FileActions::LogValuesStructure values;
        values.log_value_id << "P1" << "P2";
        values.log_value_enabled << "0" << "0";
        values.log_switch_id << "S1";
        values.log_switch_enabled << "0";

        // Simulate the runtime capability override.
        values.log_value_enabled[0] = "1";
        values.log_switch_enabled[0] = "1";

        fastecu::logging::LoggerSelection selection;
        selection.protocol = "SSM";
        selection.gauge_ids = {"P1"};
        selection.lower_panel_ids = {"P2"};
        selection.switch_ids = {"S1"};
        fastecu::logging::apply_selection(selection, values);

        QCOMPARE(values.log_value_enabled.at(0), QString("1"));
        QCOMPARE(values.log_switch_enabled.at(0), QString("1"));
        QCOMPARE(values.log_value_id.size(), 2);
        QCOMPARE(values.logging_values_protocol, QString("SSM"));
        QCOMPARE(values.dashboard_log_value_id, QStringList{"P1"});
    }
};

QTEST_APPLESS_MAIN(LegacyLoggerAdapterTest)
#include "legacy_logger_adapter_test.moc"
```

- [ ] **Step 4: Run test to verify it fails**

Run: `bazel test --config=release //src/backend/logging:legacy_logger_adapter_test`
Expected: FAIL — missing target, then missing header.

- [ ] **Step 5: Write the header**

Create `src/backend/logging/legacy_logger_adapter.h`:

```cpp
#pragma once

#include "src/backend/definitions/log_values.h"
#include "src/backend/logging/logger_definition_model.h"

namespace fastecu::logging
{

// Fans the portable logger types out into the legacy parallel-array struct.
//
// apply_definition must run EXACTLY ONCE, at load. log_value_enabled and
// log_switch_enabled hold the XML defaults only until log_operations_ssm
// overwrites them from the ECU's capability response; re-applying a
// definition afterwards would silently reset every reported capability.
void apply_definition(
    const LoggerDefinition& definition,
    definitions::LogValuesStructure& values);

// Writes only the four selection fields. Never touches definition state.
void apply_selection(
    const LoggerSelection& selection,
    definitions::LogValuesStructure& values);

} // namespace fastecu::logging
```

- [ ] **Step 6: Write the implementation**

Create `src/backend/logging/legacy_logger_adapter.cpp`:

```cpp
#include "src/backend/logging/legacy_logger_adapter.h"

#include <QString>

namespace fastecu::logging
{
namespace
{

QString qstr(const std::string& value)
{
    return QString::fromStdString(value);
}

QStringList to_qstringlist(const std::vector<std::string>& values)
{
    QStringList list;
    list.reserve(static_cast<qsizetype>(values.size()));
    for (const std::string& value : values)
    {
        list.append(qstr(value));
    }
    return list;
}

} // namespace

void apply_definition(
    const LoggerDefinition& definition, definitions::LogValuesStructure& values)
{
    for (const LoggerParameter& parameter : definition.parameters)
    {
        values.log_value_protocol.append(qstr(parameter.protocol));
        values.log_value_id.append(qstr(parameter.id));
        values.log_value_name.append(qstr(parameter.name));
        values.log_value_description.append(qstr(parameter.description));
        values.log_value_ecu_byte_index.append(qstr(parameter.ecu_byte_index));
        values.log_value_ecu_bit.append(qstr(parameter.ecu_bit));
        values.log_value_target.append(qstr(parameter.target));
        values.log_value_address.append(qstr(parameter.address));
        values.log_value_length.append(qstr(parameter.length));
        values.log_value_enabled.append(parameter.enabled ? "1" : "0");
        values.log_value.append("0.00");

        QString packed;
        for (std::size_t i = 0; i < parameter.conversions.size(); i++)
        {
            const Conversion& c = parameter.conversions.at(i);
            if (i > 0)
            {
                packed.append(",");
            }
            packed.append(QString("conversion %1,").arg(i));
            packed.append(qstr(c.units) + ",");
            packed.append(qstr(c.expr) + ",");
            packed.append(qstr(c.format) + ",");
            packed.append(qstr(c.gauge_min) + ",");
            packed.append(qstr(c.gauge_max) + ",");
            packed.append(qstr(c.gauge_step));
        }
        values.log_value_units.append(packed);
    }

    for (const LoggerSwitch& paramswitch : definition.switches)
    {
        values.log_switch_protocol.append(qstr(paramswitch.protocol));
        values.log_switch_id.append(qstr(paramswitch.id));
        values.log_switch_name.append(qstr(paramswitch.name));
        values.log_switch_description.append(qstr(paramswitch.description));
        values.log_switch_address.append(qstr(paramswitch.address));
        values.log_switch_ecu_byte_index.append(qstr(paramswitch.ecu_byte_index));
        values.log_switch_ecu_bit.append(qstr(paramswitch.ecu_bit));
        values.log_switch_target.append(qstr(paramswitch.target));
        values.log_switch_enabled.append(paramswitch.enabled ? "1" : "0");
        values.log_switch_state.append("0");
    }
}

void apply_selection(
    const LoggerSelection& selection, definitions::LogValuesStructure& values)
{
    values.logging_values_protocol = qstr(selection.protocol);
    values.dashboard_log_value_id = to_qstringlist(selection.gauge_ids);
    values.lower_panel_log_value_id = to_qstringlist(selection.lower_panel_ids);
    values.lower_panel_switch_id = to_qstringlist(selection.switch_ids);
}

} // namespace fastecu::logging
```

The packed-string construction here is deliberately temporary: it reproduces the legacy format so the tree keeps compiling until Task 9 replaces `log_value_units` with the typed member, at which point this loop collapses to a direct copy of `parameter.conversions`.

- [ ] **Step 7: Add the Bazel targets**

`src/backend/logging/BUILD.bazel` currently loads only the portable helpers, so add the Qt loads at the top of the file (buildifier keeps `load` statements sorted):

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "fastecu_qttest", "qt_cc_library")
```

Then the adapter and its test. Model both on `//src/backend/definition:legacy_definition_adapter` (`src/backend/definition/BUILD.bazel:107-120`) — note `hdrs = []` with the real header in `normal_hdrs`, because it has no `Q_OBJECT` and must not be moc'd:

```python
qt_cc_library(
    name = "legacy_logger_adapter",
    srcs = ["legacy_logger_adapter.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["legacy_logger_adapter.h"],
    visibility = ["//visibility:public"],
    deps = QT_DEPS + [
        ":logger_definition_model",
        "//src/backend/definitions:log_values",
    ],
)

fastecu_qttest(
    name = "legacy_logger_adapter_test",
    src = "legacy_logger_adapter_test.cpp",
    deps = [":legacy_logger_adapter"],
)
```

`fastecu_qttest` takes **`src`** (singular), not `srcs` — see the existing uses in `src/platform/desktop/common/logging/BUILD.bazel:58,68`.

Do **not** add `legacy_logger_adapter` to `PORTABLE_ROOTS`. It links Qt on purpose; listing it there would fail `//:portable_closure`.

- [ ] **Step 8: Run tests to verify they pass**

Run: `bazel test --config=release //src/backend/logging:all //:portable_closure`
Expected: PASS — the two new adapter cases, every portable test from Tasks 2-5, and the closure guard proving the Qt adapter did not leak into a portable target's closure.

- [ ] **Step 9: Commit**

```bash
git add src/backend/logging/legacy_logger_adapter.h \
        src/backend/logging/legacy_logger_adapter.cpp \
        src/backend/logging/legacy_logger_adapter_test.cpp \
        src/platform/desktop/common/logging/BUILD.bazel
git commit -m "feat: add the legacy logger fan-out adapter

Bridges the portable LoggerDefinition/LoggerSelection into the existing
parallel-array struct. apply_selection deliberately writes only the four
selection fields: log_value_enabled is overwritten at runtime from the
ECU's capability response, so re-fanning a definition would reset it.
A test pins that contract."
```

---

### Task 7: Delegate `read_logger_definition_file`

**Files:**
- Modify: `src/backend/definitions/file_actions.cpp:1113-1302`
- Modify: `src/backend/definitions/BUILD.bazel`

**Interfaces:**
- Consumes: `LoggerDefinitionService` (Task 5), `apply_definition` / `apply_selection` (Task 6), `initial_selection` (Task 4).
- Produces: no signature change. `read_logger_definition_file()` still returns `LogValuesStructure*`.

**Contracts that must not change:**
- Returns `logValues` (the member's address, **not** `nullptr`) when the file cannot be opened.
- Shows `QMessageBox::warning(this, tr("Logger file"), "Unable to open logger definition file '<name>' for reading")` on failure — driven by the `Result` now, not by `QFile::open`.
- Writes the resolved handle back into `configValues->romraider_logger_definition_file` in the CDBG case.
- Still calls `validate_logger_values` / `validate_logger_switches` and `logValidationErrors` at the end.
- Still seeds the three selection lists with `initial_selection` (the first-N walk — see "Decisions locked").

- [ ] **Step 1: Rewrite the method**

Replace the body of `FileActions::read_logger_definition_file` with:

```cpp
FileActions::LogValuesStructure *FileActions::read_logger_definition_file()
{
    LogValuesStructure *logValues = &LogValuesStruct;
    ConfigValuesStructure *configValues = &ConfigValuesStruct;

    fastecu::logging::LoggerDefinitionService service(
        *fileRepository_, *resourceBundle_, *atomicFileWriter_);

    const auto handle = service.resolve_definition_handle(
        configValues->romraider_logger_definition_file.toStdString(),
        configValues->flash_protocol_selected_log_protocol.toStdString(),
        configValues->config_files_directory.toStdString());
    if (!handle)
    {
        QMessageBox::warning(this, tr("Logger file"),
                             "Unable to resolve logger definition file: " +
                                 QString::fromStdString(handle.error().detail));
        return logValues;
    }
    if (configValues->romraider_logger_definition_file.isEmpty() && !handle->empty())
    {
        configValues->romraider_logger_definition_file = QString::fromStdString(*handle);
        emit LOG_D("Using bundled CDBG logger definition: " +
                       configValues->romraider_logger_definition_file,
                   true, true);
    }

    const auto definition = service.load_definition(*handle);
    if (!definition)
    {
        QMessageBox::warning(this, tr("Logger file"),
                             "Unable to open logger definition file '" +
                                 QString::fromStdString(*handle) + "' for reading");
        return logValues;
    }

    fastecu::logging::apply_definition(*definition, *logValues);
    fastecu::logging::apply_selection(
        fastecu::logging::initial_selection(*definition), *logValues);

    QStringList validationErrors;
    validate_logger_values(*logValues, &validationErrors);
    validate_logger_switches(*logValues, &validationErrors);
    if (!validationErrors.isEmpty())
    {
        logValidationErrors("Invalid logger definition:", validationErrors);
    }

    return logValues;
}
```

Check the member names for the injected ports against the `FileActions` constructor (`FileActions(fileSystem, resourceBundle, fileRepository, atomicFileWriter)` per `file_actions_parsing_test.cpp`) and use the real ones.

- [ ] **Step 2: Add the Bazel deps**

`src/backend/definitions/BUILD.bazel`'s `definitions` target gains three deps, in alphabetical order:

```python
        "//src/backend/logging:legacy_logger_adapter",
        "//src/backend/logging:logger_conf",
        "//src/backend/logging:logger_definition_service",
```

All three are backend targets, so no layering edge is crossed. The cycle that would otherwise form — `definitions` → `legacy_logger_adapter` → `definitions` — is already broken by Task 6 Step 1's extraction: the adapter depends on the fine-grained `//src/backend/definitions:log_values`, never on the whole `definitions` target. If Bazel reports a cycle here, that extraction is incomplete; fix it there rather than by moving the adapter call.

- [ ] **Step 3: Update Task 1's misalignment characterization**

`logger_definition_misaligns_rows_missing_optional_children` now fails: rows are aligned. Rewrite it to assert the fixed behaviour, keeping the name's intent:

```cpp
    // Was a characterization of the parallel-array skew (5d-5b Task 1); the
    // typed model aligns rows by construction, so this now pins the fix.
    void logger_definition_aligns_rows_missing_optional_children()
    {
        // ... same fixture ...
        QCOMPARE(values->log_value_id.size(), 2);
        QCOMPARE(values->log_value_units.size(), 2);
        QCOMPARE(values->log_value_units.at(0), QString(""));
        QCOMPARE(values->log_value_address.at(1), QString("0x20"));
        QVERIFY(FileActions::validate_logger_values(*values));
    }
```

- [ ] **Step 4: Run the full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
bazel test --config=release //src/backend/definitions:all
```
Expected: all pass. `logger_definition_reads_parameter_and_switch` and `logger_definition_seeds_selection_lists_ignoring_enabled` must still pass **unchanged** — they are the proof the conversion preserved behaviour.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definitions/file_actions.cpp \
        src/backend/definitions/file_actions_parsing_test.cpp \
        src/backend/definitions/BUILD.bazel
git commit -m "refactor: delegate read_logger_definition_file to the portable service

The QDom walk, the inline CDBG fallback and the first-N selection seeding
all move behind LoggerDefinitionService and the fan-out adapter. The
method's signature and return contract are unchanged, including returning
logValues rather than nullptr when the file cannot be opened.

Rows for parameters missing <address> or <conversions> are now aligned
instead of skewing the parallel arrays; Task 1's characterization test is
rewritten to pin the fixed behaviour."
```

---

### Task 8: Delegate `read_logger_conf`

**Files:**
- Modify: `src/backend/definitions/file_actions.cpp:820-1049`

**Interfaces:**
- Consumes: `LoggerDefinitionService` (Task 5), `apply_selection` (Task 6).
- Produces: no signature change — `read_logger_conf(LogValuesStructure*, const QString&, bool)` still returns `LogValuesStructure*`.

**Contracts that must not change:**
- Returns `nullptr` when the conf file cannot be opened, and shows `QMessageBox::warning(this, tr("Logger file"), "Unable to open logger config file '<name>' for reading")`.
- Returns `0` (i.e. `nullptr`) on the no-protocol branch — when the ECU id is absent **and** `logValues->log_value_protocol` is empty — after showing `QMessageBox::warning(this, tr("Logger definition file"), "No logger definition file selected, returning without initializing log parameters!")`.
- `modify == true` persists the caller's current selection; `modify == false` loads it.

The `modify` flag stays in the signature — the four call sites (`log_operations_ssm.cpp:392`, `logvalues.cpp:212/239/264`) are step-6 territory.

- [ ] **Step 1: Rewrite the method**

```cpp
FileActions::LogValuesStructure *FileActions::read_logger_conf(
    FileActions::LogValuesStructure *logValues, const QString& ecu_id, bool modify)
{
    ConfigValuesStructure *configValues = &ConfigValuesStruct;
    const std::string handle = configValues->logger_file.toStdString();

    emit LOG_D("Looking for ECU ID: " + ecu_id + " in logger def file: " +
                   configValues->logger_file,
               true, true);

    fastecu::logging::LoggerDefinitionService service(
        *fileRepository_, *resourceBundle_, *atomicFileWriter_);

    if (modify)
    {
        fastecu::logging::LoggerSelection selection;
        selection.protocol = logValues->logging_values_protocol.toStdString();
        for (const QString& id : logValues->dashboard_log_value_id)
        {
            selection.gauge_ids.push_back(id.toStdString());
        }
        for (const QString& id : logValues->lower_panel_log_value_id)
        {
            selection.lower_panel_ids.push_back(id.toStdString());
        }
        for (const QString& id : logValues->lower_panel_switch_id)
        {
            selection.switch_ids.push_back(id.toStdString());
        }
        if (auto saved = service.save_selection(handle, ecu_id.toStdString(), selection); !saved)
        {
            QMessageBox::warning(this, tr("Logger file"),
                                 "Unable to open logger config file '" +
                                     configValues->logger_file + "' for reading");
            return nullptr;
        }
        return logValues;
    }

    // A read must not initialize when there is no definition to initialize from.
    if (logValues->log_value_protocol.empty())
    {
        // Only reachable when the ECU id is absent; probe first so the
        // no-protocol warning still wins over a successful load.
        const auto probe = service.load_or_initialize_selection(
            handle, ecu_id.toStdString(), fastecu::logging::LoggerDefinition{});
        if (!probe)
        {
            QMessageBox::warning(this, tr("Logger file"),
                                 "Unable to open logger config file '" +
                                     configValues->logger_file + "' for reading");
            return nullptr;
        }
        if (probe->gauge_ids.empty() && probe->lower_panel_ids.empty() &&
            probe->switch_ids.empty())
        {
            QMessageBox::warning(this, tr("Logger definition file"),
                                 "No logger definition file selected, returning without "
                                 "initializing log parameters!");
            emit LOG_D("No logger definition file selected, returning without "
                       "initializing log parameters!",
                       true, true);
            return nullptr;
        }
        fastecu::logging::apply_selection(*probe, *logValues);
        return logValues;
    }

    fastecu::logging::LoggerDefinition definition;
    for (int i = 0; i < logValues->log_value_id.size(); i++)
    {
        fastecu::logging::LoggerParameter parameter;
        parameter.protocol = logValues->log_value_protocol.at(i).toStdString();
        parameter.id = logValues->log_value_id.at(i).toStdString();
        parameter.enabled = logValues->log_value_enabled.at(i) == "1";
        definition.parameters.push_back(std::move(parameter));
    }
    for (int i = 0; i < logValues->log_switch_id.size(); i++)
    {
        fastecu::logging::LoggerSwitch paramswitch;
        paramswitch.protocol = logValues->log_switch_protocol.at(i).toStdString();
        paramswitch.id = logValues->log_switch_id.at(i).toStdString();
        // Carries the ECU's runtime capability response, not the XML default --
        // this is the state default_selection must filter on.
        paramswitch.enabled = logValues->log_switch_enabled.at(i) == "1";
        definition.switches.push_back(std::move(paramswitch));
    }

    const auto selection =
        service.load_or_initialize_selection(handle, ecu_id.toStdString(), definition);
    if (!selection)
    {
        QMessageBox::warning(this, tr("Logger file"),
                             "Unable to open logger config file '" +
                                 configValues->logger_file + "' for reading");
        return nullptr;
    }
    fastecu::logging::apply_selection(*selection, *logValues);
    return logValues;
}
```

The `log_value_protocol.empty()` branch above is subtle: the legacy code only reached its no-protocol warning **after** failing to find the ECU id. Reproducing that ordering exactly is what the probe is for. If Task 1's characterization shows a simpler equivalent ordering, prefer the simpler one and say so in the report.

- [ ] **Step 2: Delete the commented-out `save_logger_conf`**

Remove `file_actions.cpp:1050-1112` (the whole `/* ... */` block) and the declaration at `file_actions.h:212`, together with its `/* Save logger conf file */` comment banner. Its only reference is the commented-out call that this rewrite already deleted.

- [ ] **Step 3: Extend the conf tests**

Add to `file_actions_parsing_test.cpp`:

```cpp
    void logger_conf_returns_nullptr_when_the_file_is_missing()
    {
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        actions.ConfigValuesStruct.logger_file = "/nonexistent/logger.cfg";
        FileActions::LogValuesStructure values;
        values.log_value_protocol << "SSM";
        values.log_value_id << "P1";
        values.log_value_enabled << "1";
        QCOMPARE(actions.read_logger_conf(&values, "ECUID1", false), nullptr);
    }

    void logger_conf_round_trips_a_modified_selection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString conf = writeTextFile(dir, "logger.cfg",
                                           "<config><logger/></config>");
        QVERIFY(!conf.isEmpty());

        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_);
        actions.ConfigValuesStruct.logger_file = conf;
        FileActions::LogValuesStructure values;
        values.log_value_protocol << "SSM" << "SSM";
        values.log_value_id << "P1" << "P2";
        values.log_value_enabled << "1" << "1";
        values.logging_values_protocol = "SSM";
        values.dashboard_log_value_id << "P2";
        values.lower_panel_log_value_id << "P1";

        QVERIFY(actions.read_logger_conf(&values, "ECUID1", true) != nullptr);

        FileActions::LogValuesStructure reloaded;
        reloaded.log_value_protocol << "SSM" << "SSM";
        reloaded.log_value_id << "P1" << "P2";
        reloaded.log_value_enabled << "1" << "1";
        QVERIFY(actions.read_logger_conf(&reloaded, "ECUID1", false) != nullptr);
        QCOMPARE(reloaded.dashboard_log_value_id, QStringList{"P2"});
        QCOMPARE(reloaded.lower_panel_log_value_id, QStringList{"P1"});
        // The definition-side flags survived the round trip.
        QCOMPARE(reloaded.log_value_enabled, (QStringList{"1", "1"}));
    }
```

- [ ] **Step 4: Run the full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
bazel test --config=release //src/backend/definitions:all
```
Expected: all pass, including Task 1's `logger_conf_writes_four_space_indented_xml` **unchanged** — that test is the proof the new writer matches the old bytes.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definitions/file_actions.cpp src/backend/definitions/file_actions.h \
        src/backend/definitions/file_actions_parsing_test.cpp
git commit -m "refactor: delegate read_logger_conf to the portable service

The three operations fused behind the modify flag now map onto
read_selection, write_selection and default_selection. The signature,
both warning dialogs and the nullptr/0 return contracts are unchanged.

Persisting no longer truncates the open file before serializing: it goes
through IAtomicFileWriter::replace, so a failed write can no longer
destroy the user's logger configuration.

Also deletes save_logger_conf, whose body has been inside a block comment
and whose only call site was commented out."
```

---

### Task 9: Type the conversions and convert the five consumers

**Files:**
- Modify: `src/backend/definitions/file_actions.h:95`, `file_actions.cpp:640`
- Modify: `src/backend/logging/legacy_logger_adapter.cpp`
- Modify: `src/ui/desktop/logvalues.cpp:81,125`
- Modify: `src/ui/desktop/mainwindow.cpp:2160,2199`
- Modify: `src/platform/desktop/common/logging/logging_snapshot_adapter.cpp:46`
- Modify: `src/platform/desktop/common/logging/logging_adapters_test.cpp:44`
- Modify: `src/backend/definitions/model_validation_test.cpp:213`
- Modify: `src/backend/definitions/file_actions_parsing_test.cpp` (the `log_value_units` assertions)

**Interfaces:**
- Consumes: `fastecu::logging::Conversion` (Task 2).
- Produces: `QList<QList<fastecu::logging::Conversion>> log_value_conversions` replacing `QStringList log_value_units`.

**This task is atomic** — the member's type changes and every consumer changes with it in one commit, or the tree does not compile. Parallel-array indexing by row is preserved exactly; dismantling that is step-6 work.

- [ ] **Step 1: Change the member's type**

In `src/backend/definitions/file_actions.h`, add `#include "src/backend/logging/logger_definition_model.h"` and replace line 95:

```cpp
-        QStringList log_value_units;
+        QList<QList<fastecu::logging::Conversion>> log_value_conversions;
```

- [ ] **Step 2: Simplify the adapter**

In `legacy_logger_adapter.cpp`, the packed-string loop from Task 6 collapses:

```cpp
-        QString packed;
-        for (std::size_t i = 0; i < parameter.conversions.size(); i++)
-        {
-            ... seven appends ...
-        }
-        values.log_value_units.append(packed);
+        QList<Conversion> conversions;
+        conversions.reserve(static_cast<qsizetype>(parameter.conversions.size()));
+        for (const Conversion& conversion : parameter.conversions)
+        {
+            conversions.append(conversion);
+        }
+        values.log_value_conversions.append(conversions);
```

- [ ] **Step 3: Convert the two `logvalues.cpp` sites**

Both `logvalues.cpp:81` and `logvalues.cpp:125` currently read:

```cpp
                    QStringList units = logValues->log_value_units.at(j).split(",");
                    // qDebug() << "Units:" << units;
                    for (int k = 1; k < units.length(); k += 7)
                    {
                        log_units_combobox->addItem(units.at(k));
                    }
```

Both become:

```cpp
                    for (const auto& conversion : logValues->log_value_conversions.at(j))
                    {
                        log_units_combobox->addItem(QString::fromStdString(conversion.units));
                    }
```

- [ ] **Step 4: Convert the two `mainwindow.cpp` sites**

`mainwindow.cpp:2160`:

```cpp
-                QStringList value_unit = logValues->log_value_units.at(j).split(",");
-                QGroupBox *logBox = logBoxes->drawLogBoxes("log", i, logBoxCount, logValues->log_value_name.at(j), value_unit.at(1), logValues->log_value.at(j));
+                const auto& conversions = logValues->log_value_conversions.at(j);
+                const QString unit = conversions.isEmpty()
+                                         ? QString()
+                                         : QString::fromStdString(conversions.at(0).units);
+                QGroupBox *logBox = logBoxes->drawLogBoxes("log", i, logBoxCount, logValues->log_value_name.at(j), unit, logValues->log_value.at(j));
```

`mainwindow.cpp:2199`:

```cpp
-                unit = logValues->log_value_units.at(index).split(",").at(1);
+                const auto& conversions = logValues->log_value_conversions.at(index);
+                unit = conversions.isEmpty()
+                           ? QString()
+                           : QString::fromStdString(conversions.at(0).units);
```

The `isEmpty()` guards are new and necessary: `.at(1)` on a row with no conversions was undefined behaviour before the alignment fix made empty rows reachable.

- [ ] **Step 5: Convert the snapshot adapter**

`logging_snapshot_adapter.cpp:46`:

```cpp
-    const QStringList unit_fields = log_values.log_value_units.at(row).split(',');
-    if (unit_fields.size() < 4 || unit_fields.at(1).isEmpty() || unit_fields.at(2).isEmpty())
+    const auto& conversions = log_values.log_value_conversions.at(row);
+    if (conversions.isEmpty() || conversions.at(0).units.empty() ||
+        conversions.at(0).expr.empty() || conversions.at(0).format.empty())
     {
         return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                              "malformed logging conversion");
     }
```

**`format.empty()` is load-bearing, not defensive padding.** The old `size() < 4` check rejected a packed record too short to index `.at(3)` — the format field. `logging_adapters_test.cpp:302` relies on exactly that, passing `"conversion 0,rpm,x"` (three fields) and expecting a malformed verdict. In the typed model `format` always exists as a member, so "too few fields" only survives as "format is empty". Dropping this clause silently turns that test case from malformed into valid.

Then replace the later `unit_fields.at(1)` / `.at(2)` / `.at(3)` reads with `conversions.at(0).units` / `.expr` / `.format`. Read the whole function before editing — the plan lists the guard and the three reads, but the exact surrounding lines govern.

- [ ] **Step 6: Update the three test sites**

- `logging_adapters_test.cpp` — the `append_value_with_units` helper (line 30) takes `const QString& units` and does `values.log_value_units.append(units)` at line 44. Change the parameter to `const QList<fastecu::logging::Conversion>& conversions` and the body line to `values.log_value_conversions.append(conversions);`. Then translate its six call sites, field-for-field, using the packed order `conversion N, units, expr, format, gauge_min, gauge_max, gauge_step`:

| Line | Current packed literal | Replacement |
|---|---|---|
| 54 (`append_value`'s default) | `"conversion 0,rpm,x," + format + ",0,100,1"` | `{{"rpm", "x", format.toStdString(), "0", "100", "1"}}` |
| 302 | `"conversion 0,rpm,x"` | `{{"rpm", "x", "", "", "", ""}}` |
| 318 | `"conversion 0,rpm,,0.00,0,100,1"` | `{{"rpm", "", "0.00", "0", "100", "1"}}` |
| 334 | `"conversion 0,rpm,x,0.00,0,100,1"` | `{{"rpm", "x", "0.00", "0", "100", "1"}}` |
| 351 | `"conversion 0,rpm,x,0.00,0,100,1"` | `{{"rpm", "x", "0.00", "0", "100", "1"}}` |
| 402 | `"conversion 0,rpm,not_an_expr,0.00,0,100,1"` | `{{"rpm", "not_an_expr", "0.00", "0", "100", "1"}}` |

  Line 302's empty `format` is what keeps that case malformed under the guard from Step 5 — it is the typed spelling of "the record had too few fields". Lines 334 and 351 stay valid conversions; those two cases exercise a bad `address` and a bad `length` respectively, not a bad conversion.
- `model_validation_test.cpp:213` — `logValues.log_value_units << "rpm,x,0"` becomes `logValues.log_value_conversions.append(QList<fastecu::logging::Conversion>{{"rpm", "x", "0", "", "", ""}});`.
- `file_actions_parsing_test.cpp:194` — the packed-string `QCOMPARE` becomes field assertions:

```cpp
        QCOMPARE(values->log_value_conversions.at(0).size(), 1);
        QCOMPARE(QString::fromStdString(values->log_value_conversions.at(0).at(0).units),
                 QString("rpm"));
        QCOMPARE(QString::fromStdString(values->log_value_conversions.at(0).at(0).expr),
                 QString("x*0.25"));
        QCOMPARE(QString::fromStdString(values->log_value_conversions.at(0).at(0).format),
                 QString("0.00"));
```

Task 7's rewritten `logger_definition_aligns_rows_missing_optional_children` also asserts on `log_value_units`; change its empty-string assertion to `QVERIFY(values->log_value_conversions.at(0).isEmpty())`.

- [ ] **Step 7: Update the validator**

`file_actions.cpp:640`'s length check becomes structural — the typed member cannot be short:

```cpp
-    validateListLength("log_value", "units", logValues.log_value_units.size(), rows, out);
+    validateListLength("log_value", "conversions", logValues.log_value_conversions.size(), rows, out);
```

Keep the check: the adapter could still append unevenly if a future edit breaks it, and the check is one line.

- [ ] **Step 8: Run the full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
```
Expected: all pass. A missed consumer shows up as a compile error, which is the point of doing this atomically.

Then confirm no packed-format reader survives:
```bash
git grep -n "log_value_units\|+= 7"
```
Expected: no hits in `src/`.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor: replace the packed conversions string with a typed model

log_value_units held a comma-joined stride-7 record that five production
sites decoded with hard-coded positional indexes. It becomes
QList<QList<Conversion>>, and each consumer reads a named field.

Two mainwindow sites gain an isEmpty() guard: reading .at(1) on a row
with no conversions was undefined behaviour, previously unreachable only
because the parallel arrays skewed instead of holding an empty row."
```

---

### Task 10: Delete the dead logger code

**Files:**
- Modify: `src/backend/definitions/file_actions.h:96-104,110,133-141`
- Modify: `src/backend/definitions/file_actions.cpp:26-119`
- Modify: `src/ui/desktop/log_operations_ssm.cpp:331,338`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing. Pure removal.

Each removal below was verified by reference check against the current tree, not assumed. **Re-verify each with `git grep` before deleting** — the 5d-4b lesson is that vestigial code hides behind stale string comparisons as well as unreferenced symbols.

- [ ] **Step 1: Verify the removals are still dead**

```bash
git grep -n "log_value_from_byte\|log_value_format\|log_value_gauge_min\|log_value_gauge_max\|log_value_gauge_step\|log_value_ecu_id\|log_value_type"
git grep -n "log_values_by_protocol"
git grep -n "dt_codes_struct\|dt_codes_structure"
git grep -n "debugLogTransports\|debugLogDtcodes\|debugLogEcuparams"
```

Expected: the seven fields and `dt_codes_structure` appear **only** in their `file_actions.h` declarations; `log_values_by_protocol` appears in its declaration plus `log_operations_ssm.cpp:331,338` (a clear and an append, never a read); the three `debugLog*` helpers appear only in their definitions at `file_actions.cpp:26-119` — Task 7's rewrite removed their call sites.

If any of them has acquired a reader, **stop and report** rather than deleting.

- [ ] **Step 2: Delete the seven dead fields and `log_values_by_protocol`**

From `LogValuesStructure` in `file_actions.h`, delete `log_value_from_byte`, `log_value_format`, `log_value_gauge_min`, `log_value_gauge_max`, `log_value_gauge_step`, `log_value_ecu_id`, `log_value_type` and `log_values_by_protocol`. These seven are the dedicated homes for exactly the data the packed CSV string absorbed instead — the fields the parser should have filled and never did. Removing them and the packed format is one cleanup.

In `log_operations_ssm.cpp`, delete the `log_values_by_protocol` clear at line 331 and the append at line 338. Read the surrounding loop first: if the append is the loop's only body, delete the loop too.

- [ ] **Step 3: Delete `dt_codes_structure`**

Delete `file_actions.h:133-141` — the struct and its `dt_codes_struct` member. The `<dtcodes>` and `<ecuparams>` sections were parsed only to feed the debug helpers going away in Step 4; the struct was declared, never populated and never read.

- [ ] **Step 4: Delete the three debug helpers**

Delete `file_actions.cpp:26-119` — `debugLogTransports`, `debugLogDtcodes`, `debugLogEcuparams`. They are gated on `kDebugFileActions` (an `if constexpr (!kDebugFileActions) return;` no-op), reachable only from the QDom traversal Task 7 replaced, and they print XML attributes the user already has in the file. If `kDebugFileActions` has no remaining reader after this, delete it too.

- [ ] **Step 5: Run the full gate**

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test  -k --config=release //tests/... //:bazel_openssl_wiring \
            //:serial_compat_allowlist //:portable_closure
prek run --all-files
```
Expected: all pass. A deletion that was not actually dead fails to compile here.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor: delete the dead logger fields and debug helpers

Seven LogValuesStructure fields had no reference outside their own
declaration -- they were the dedicated homes for exactly the data the
packed conversions string absorbed instead. log_values_by_protocol was
cleared and appended but never read. dt_codes_structure was declared,
never populated and never read. The three debugLog* helpers were
kDebugFileActions no-ops reachable only from the QDom traversal that
5d-5b replaced.

Each verified by reference check against the tree, not assumed."
```

---

## Definition of done

- `git grep -n "log_value_units"` returns nothing in `src/`.
- `git grep -n "QDomDocument"` returns nothing in the logger paths of `file_actions.cpp`.
- `FileActions::read_logger_definition_file` and `read_logger_conf` keep their exact signatures and return contracts (`logValues` not `nullptr` on definition-open failure; `nullptr` on conf-open failure and on the no-protocol branch).
- Four new portable targets exist in `//src/backend/logging` and `//:portable_closure` passes.
- A selection round-trip leaves `log_value_enabled` unchanged (pinned by `legacy_logger_adapter_test`).
- `write_selection` emits four-space-indented XML matching what `QDomDocument::save(output, 4)` wrote.
- `bazel test -k --config=release //...` passes; `prek run --all-files` passes.
- New-code coverage `>=80%` and the SonarCloud Quality Gate is green.

## Follow-ups

File as issues when this merges; do not carry them inside it.

- **Drop the `QDomDocument`-referenced byte-identity test** (`WriteSelection.ReproducesTheFourSpaceQDomIndent`). The migration it proves happens exactly once, after which pugixml is the reference writer and the assertion pins a writer the tree no longer contains — it would block any later deliberate formatting change for no benefit. Replace it with a **round-trip against a copy of the real shipped `logger.cfg`** (`read_selection` → `write_selection` with the selection unchanged, asserting the output is byte-identical to the input): that fixture carries an XML declaration, several `<ecu>` siblings and self-closing leaf elements, so it is the one that would have caught the declaration loss, the `<ecu>` reordering and the original `" />"` reflow — three defects the single synthetic golden could not see, because the golden's input had none of those features. Keep the `write_selection` → `read_selection` round-trip assertion. **Keep the four-space `format_indent`.**
- **Dismantle the parallel arrays.** `LogValuesStructure`'s row-indexed `QStringList`s are what force `apply_definition` to exist at all. Replacing them with a `QList<LoggerParameter>` is step-6 work touching `MainWindow` and `LogValues` directly.
- **Retire the `modify` flag.** `read_logger_conf(values, ecu, true)` is a save; its four call sites should call a `save_logger_selection` method instead, at which point the portable split is visible at the call sites too.
