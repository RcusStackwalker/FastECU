# Step 6a-2 Definition Authoring Dialog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `FileActions::create_new_definition_for_rom` and `use_existing_definition_for_rom` — 294 lines of `QFileDialog` and field-collecting `QDialog` — out of the backend into a new `src/ui/desktop/definition` package, leaving only the already-portable `submit_*_definition` seam behind.

**Architecture:** The two functions are interactive wizards end to end: collect ROM header fields in a grid of line edits, choose a destination path, then call an existing backend submit. The backend half already exists and is portable; only the presentation needs relocating. The move is split three ways — the pure widget-to-model mapping (testable headlessly), the dialog class that drives the modals, and the swap that deletes the originals. The two functions share about 60 lines verbatim (the path-selection nag loop, the suffix normalization, the config-append block); those are deduplicated during the move rather than copied twice.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest/GoogleMock 1.17.0.bcr.2, Qt 6.8.3 (Widgets).

**Spec:** [docs/superpowers/specs/2026-09-01-step6a-file-actions-dewidget-design.md](../specs/2026-09-01-step6a-file-actions-dewidget-design.md) — this plan implements its "Move b".

## Global Constraints

- **Do not edit any file under `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`.** The flash drain owns those paths on a parallel branch (14 families remaining). Verify with `git diff --name-only origin/master | grep -E 'src/platform/desktop/common/flash/legacy/|src/ui/desktop/flash/'` — it must print nothing.
- `src/ui/desktop/mainwindow.{h,cpp}` are also edited by that branch. Keep edits there minimal and localized; no reformatting or tidying of untouched code.
- **The `FileActions::` type and static surface stays frozen** — the three structure aliases, `RomInfoEnum`, `parse_nrc_message`/`parse_dtc_message`, and the `validate_*` / `collect_ecuflash_*` statics. ~85 files depend on it, most of them drain-owned.
- **Out of scope** (later slices): dropping `QWidget`/`Q_OBJECT` from `FileActions` (6a-3), the `QMessageBox` → `IEventSink` conversion (6a-3), and the `//src/algorithms/expression:qt_compat` drain (6a-4). `FileActions` is still a `QWidget` when this plan ends, and the new dialog still raises `QMessageBox` directly.
- Layering: `ui → backend` is permitted, `backend → ui` never is. The new package lives in `src/ui/desktop/definition` precisely so it may depend on `//src/backend/definitions` and `//src/backend/definition:definition_writer`; the reverse dependency must not be created even temporarily.
- Every header needs `#pragma once` (enforced by prek).
- Qt targets put `Q_OBJECT` headers in `MOC_HDRS`/`hdrs` and everything else in `normal_hdrs`. A `Q_OBJECT` header missing from the moc list links but fails at runtime.
- Backend results are `fastecu::Result<T>` / `fastecu::Status`, checked with `.has_value()`, never the implicit `operator bool`.
- Run `prek run --all-files` before each commit. Build and test with `--config=release`.

## Behavior To Preserve

Read from `src/backend/definitions/file_actions.cpp` at `f74c525` (`create_new_definition_for_rom` at line 854, `use_existing_definition_for_rom` at 979). These are the details a rewrite silently loses:

1. **The nag loop.** When the user cancels the file chooser, a second modal appears — "No file selected! If you still want to create file click 'Ok' / If you want to continue to use ROM without definition, click 'Cancel'". `Ok` re-opens the chooser; `Cancel` gives up. This repeats indefinitely until a file is chosen or the user cancels the nag. The *open* variant says "select file" where the save variant says "create file".
2. **Giving up is success, not failure.** When the user cancels out, both functions `return ecuCalDef` — the ROM stays open without a definition. Only a genuine error returns `nullptr`.
3. **Suffix normalization, in this order.** A single trailing `.` is stripped first, then `.xml` is appended if not already present. So `foo.` becomes `foo.xml`, and `foo.xml` is untouched.
4. **The two dialogs differ only in prefill.** `create_new` iterates `ecuCalDef->DefHeaderNames` and leaves every field empty. `use_existing` first reads the chosen source file, runs it through `FileActions::collect_ecuflash_base_header_fields`, and iterates the resulting `(name, value)` **pairs** — stepping `i += 2` — prefilling each editor via `setText`. Both label their rows from `ecuCalDef->DefHeaderStrings.at(index)`, indexed by row, not by `i`.
5. **`notes` is the only `QTextEdit`.** Every other field is a `QLineEdit`. The notes editor spans two columns and is placed one row lower (`index + 1`, spanning `1, 2`).
6. **Editor `objectName()` carries the field name** and is what `definitionHeaderInput` maps on. Losing the object name silently produces an all-empty header.
7. **`include` is excluded from the debug log.** Both functions log every line edit's text at `LOG_D` except the one named `include`.
8. **On success, four config lists are appended in order:** `ecuflash_def_cal_id` (from `input->xml_id`), `ecuflash_def_cal_id_addr` (from the raw `internalidaddress` text, not the parsed integer), `ecuflash_def_ecu_id`, and `ecuflash_def_filename`.
9. **`internalidaddress` parses as hex**, and an unparseable non-empty value is an `InvalidConfig` error, not a zero. An empty value yields `std::nullopt`.
10. **`use_existing` reads its source through the injected repository**, not `QFile` — `definitionFileRepository_.read(source)`. A read failure logs and returns `nullptr` after a "Unable to open definition file for reading" warning.

## Deliberate changes

Three, all forced or clearly correct. Everything else is a faithful move.

- **Dialog ownership moves from `FileActions` to the calling `QWidget`.** The originals do `new QDialog(this)` parented to the `FileActions` widget, which leaks one dialog per invocation for the process lifetime. 6a-3 removes `QWidget` from `FileActions` entirely, so this parent cannot survive regardless. The new code stack-allocates the dialogs, which both fixes the leak and removes the parent question.
- **The duplicated nag loop becomes one function.** It is byte-identical between the two save paths and differs from the open path only in the chooser call and one word of the message.
- **`emit LOG_D` becomes signals on the new dialog class**, connected by `MainWindow` exactly as it already connects `FileActions`'s. Log output is unchanged.

## File Structure

| File | Responsibility |
|---|---|
| `src/ui/desktop/definition/definition_header_form.h` (create) | Declares the pure mapping helpers and the grid builder |
| `src/ui/desktop/definition/definition_header_form.cpp` (create) | `definition_header_input`, `line_edit_value`, `normalize_xml_suffix`, `build_header_form` |
| `src/ui/desktop/definition/definition_header_form_test.cpp` (create) | Headless tests for all four — the testable core of this slice |
| `src/ui/desktop/definition/definition_authoring_dialog.h` (create) | `DefinitionAuthoringDialog` (`Q_OBJECT`) — two public entry points, `LOG_*` signals |
| `src/ui/desktop/definition/definition_authoring_dialog.cpp` (create) | The modal flows: field dialog, path selection with nag loop, submit, config append |
| `src/ui/desktop/definition/definition_authoring_dialog_test.cpp` (create) | Construction and non-modal surface |
| `src/ui/desktop/definition/BUILD.bazel` (create) | `qt_cc_library` ×2 + `fastecu_gtest` ×2 |
| `src/backend/definitions/file_actions.h` (modify) | Promote `submit_new_definition` / `submit_imported_definition` to public; delete the two `*_for_rom` declarations |
| `src/backend/definitions/file_actions.cpp` (modify) | Delete both function bodies and the two now-unused anonymous-namespace helpers |
| `src/ui/desktop/mainwindow.h` (modify) | Hold a `DefinitionAuthoringDialog` member |
| `src/ui/desktop/mainwindow.cpp` (modify) | Construct and connect it; call it from `prompt_for_missing_definition` |
| `src/ui/desktop/BUILD.bazel` (modify) | Depend on `//src/ui/desktop/definition:definition_authoring_dialog` |

---

### Task 1: Pure header-form helpers

**Files:**
- Create: `src/ui/desktop/definition/definition_header_form.h`
- Create: `src/ui/desktop/definition/definition_header_form.cpp`
- Test: `src/ui/desktop/definition/definition_header_form_test.cpp`
- Create: `src/ui/desktop/definition/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::definition::DefinitionHeaderInput` and `fastecu::definition::RomMetadata` from `src/backend/definition/definition_writer.h` (Bazel target `//src/backend/definition:definition_writer`, whose package is `//visibility:public`); `fastecu::Result`, `fastecu::fail`, `fastecu::ErrorKind` from `src/backend/ports/`.
- Produces, all in `namespace fastecu::ui`:
  - `struct HeaderFormEditors { QList<QLineEdit *> line_edits; QList<QTextEdit *> text_edits; };`
  - `HeaderFormEditors build_header_form(QGridLayout *grid, const QStringList& labels, const QStringList& names, const QStringList& values);`
  - `fastecu::Result<fastecu::definition::DefinitionHeaderInput> definition_header_input(const HeaderFormEditors&);`
  - `QString line_edit_value(const HeaderFormEditors&, const QString& name);`
  - `QString normalize_xml_suffix(QString filename);`

  Task 2 uses all five.

**Note on provenance:** `definition_header_input` and `line_edit_value` are moves, not new code. They exist today as anonymous-namespace free functions at `src/backend/definitions/file_actions.cpp:27` and `:39`. Copy the bodies faithfully, adapting only the parameter type (from two separate lists to `HeaderFormEditors`). Task 3 deletes the originals — **do not delete them in this task**, because `file_actions.cpp` still calls them until then, and making the backend depend on this UI package to reach them would invert the layering.

- [ ] **Step 1: Write the failing test**

Create `src/ui/desktop/definition/definition_header_form_test.cpp`:

```cpp
#include "src/ui/desktop/definition/definition_header_form.h"

#include <memory>

#include <QApplication>
#include <QGridLayout>
#include <QLineEdit>
#include <QTextEdit>
#include <QWidget>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using fastecu::ui::build_header_form;
using fastecu::ui::definition_header_input;
using fastecu::ui::HeaderFormEditors;
using fastecu::ui::line_edit_value;
using fastecu::ui::normalize_xml_suffix;

namespace
{

// QGridLayout and the editors are QWidgets, which abort at construction
// without a live QApplication. fastecu_gtest links plain gtest_main, so
// bring one up via a ::testing::Environment, mirroring QtPortEnvironment in
// src/platform/desktop/common/ports/qt_port_adapters_test.cpp.
class DefinitionFormEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static char program[] = "definition_header_form_test";
        static char *argv[] = {program, nullptr};
        app_ = std::make_unique<QApplication>(argc, argv);
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *definition_form_environment = ::testing::AddGlobalTestEnvironment(new DefinitionFormEnvironment);

// The field names the real EcuCalDefStructure::DefHeaderNames carries, in
// the order definitionHeaderInput maps them.
const QStringList kNames = {"xmlid",        "internalidaddress", "internalidstring", "ecuid",
                            "make",         "market",            "model",            "submodel",
                            "transmission", "year",              "flashmethod",      "memmodel",
                            "checksummodule", "include",         "notes"};

QStringList labels_for(const QStringList& names)
{
    QStringList labels;
    for (const QString& name : names)
    {
        labels.append(name + " label");
    }
    return labels;
}

} // namespace

TEST(BuildHeaderFormTest, MakesALineEditPerFieldAndOneTextEditForNotes)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);

    const HeaderFormEditors editors = build_header_form(grid, labels_for(kNames), kNames, {});

    EXPECT_EQ(editors.line_edits.size(), kNames.size() - 1);
    ASSERT_EQ(editors.text_edits.size(), 1);
    EXPECT_EQ(editors.text_edits.at(0)->objectName(), "notes");
}

TEST(BuildHeaderFormTest, SetsObjectNameOnEveryEditor)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);

    const HeaderFormEditors editors = build_header_form(grid, labels_for(kNames), kNames, {});

    QStringList seen;
    for (const QLineEdit *editor : editors.line_edits)
    {
        seen.append(editor->objectName());
    }
    EXPECT_THAT(seen, testing::Contains("xmlid"));
    EXPECT_THAT(seen, testing::Contains("checksummodule"));
    EXPECT_THAT(seen, testing::Not(testing::Contains("")));
}

TEST(BuildHeaderFormTest, LeavesEditorsEmptyWhenNoValuesAreSupplied)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);

    const HeaderFormEditors editors = build_header_form(grid, labels_for(kNames), kNames, {});

    for (const QLineEdit *editor : editors.line_edits)
    {
        EXPECT_TRUE(editor->text().isEmpty()) << editor->objectName().toStdString();
    }
    EXPECT_TRUE(editors.text_edits.at(0)->toPlainText().isEmpty());
}

TEST(BuildHeaderFormTest, PrefillsEditorsWhenValuesAreSupplied)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "ecuid", "notes"};
    const QStringList values = {"CAL123", "EC00456", "some notes"};

    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, values);

    ASSERT_EQ(editors.line_edits.size(), 2);
    EXPECT_EQ(editors.line_edits.at(0)->text(), "CAL123");
    EXPECT_EQ(editors.line_edits.at(1)->text(), "EC00456");
    ASSERT_EQ(editors.text_edits.size(), 1);
    EXPECT_EQ(editors.text_edits.at(0)->toPlainText(), "some notes");
}

TEST(DefinitionHeaderInputTest, MapsEveryFieldByObjectName)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const HeaderFormEditors editors = build_header_form(grid, labels_for(kNames), kNames, {});

    for (QLineEdit *editor : editors.line_edits)
    {
        editor->setText(editor->objectName() + "-value");
    }
    editors.line_edits.at(1)->setText("2f8000"); // internalidaddress must parse as hex
    editors.text_edits.at(0)->setPlainText("note body");

    const auto input = definition_header_input(editors);

    ASSERT_TRUE(input.has_value());
    EXPECT_EQ(input->xml_id, "xmlid-value");
    EXPECT_EQ(input->internal_id, "internalidstring-value");
    EXPECT_EQ(input->ecu_id, "ecuid-value");
    EXPECT_EQ(input->metadata.make, "make-value");
    EXPECT_EQ(input->metadata.market, "market-value");
    EXPECT_EQ(input->metadata.model, "model-value");
    EXPECT_EQ(input->metadata.submodel, "submodel-value");
    EXPECT_EQ(input->metadata.transmission, "transmission-value");
    EXPECT_EQ(input->metadata.year, "year-value");
    EXPECT_EQ(input->metadata.flash_method, "flashmethod-value");
    EXPECT_EQ(input->metadata.memory_model, "memmodel-value");
    EXPECT_EQ(input->metadata.checksum_module, "checksummodule-value");
    EXPECT_EQ(input->include, "include-value");
    EXPECT_EQ(input->notes, "note body");
}

TEST(DefinitionHeaderInputTest, ParsesInternalIdAddressAsHex)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "internalidaddress"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"id", "2f8000"});

    const auto input = definition_header_input(editors);

    ASSERT_TRUE(input.has_value());
    ASSERT_TRUE(input->internal_id_address.has_value());
    EXPECT_EQ(*input->internal_id_address, 0x2f8000U);
}

TEST(DefinitionHeaderInputTest, EmptyInternalIdAddressYieldsNullopt)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "internalidaddress"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"id", "   "});

    const auto input = definition_header_input(editors);

    ASSERT_TRUE(input.has_value());
    EXPECT_FALSE(input->internal_id_address.has_value());
}

TEST(DefinitionHeaderInputTest, UnparseableInternalIdAddressIsInvalidConfig)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "internalidaddress"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"id", "not-hex"});

    const auto input = definition_header_input(editors);

    ASSERT_FALSE(input.has_value());
    EXPECT_EQ(input.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(input.error().detail, testing::HasSubstr("internal ID address"));
}

TEST(DefinitionHeaderInputTest, TrimsXmlIdButNotTheOtherFields)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "ecuid"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"  CAL123  ", "  EC0  "});

    const auto input = definition_header_input(editors);

    ASSERT_TRUE(input.has_value());
    EXPECT_EQ(input->xml_id, "CAL123");
    EXPECT_EQ(input->ecu_id, "  EC0  ");
}

TEST(LineEditValueTest, ReturnsTheNamedEditorsTextAndEmptyForAnAbsentName)
{
    QWidget host;
    auto *grid = new QGridLayout(&host);
    const QStringList names = {"xmlid", "ecuid"};
    const HeaderFormEditors editors = build_header_form(grid, labels_for(names), names, {"CAL123", "EC0"});

    EXPECT_EQ(line_edit_value(editors, "ecuid"), "EC0");
    EXPECT_EQ(line_edit_value(editors, "nosuchfield"), QString());
}

TEST(NormalizeXmlSuffixTest, StripsOneTrailingDotThenAppendsXml)
{
    EXPECT_EQ(normalize_xml_suffix("foo"), "foo.xml");
    EXPECT_EQ(normalize_xml_suffix("foo."), "foo.xml");
    EXPECT_EQ(normalize_xml_suffix("foo.xml"), "foo.xml");
    EXPECT_EQ(normalize_xml_suffix("foo.bar"), "foo.bar.xml");
    EXPECT_EQ(normalize_xml_suffix("/tmp/a b/def."), "/tmp/a b/def.xml");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/ui/desktop/definition:definition_header_form_test`
Expected: FAIL — the package does not exist yet.

- [ ] **Step 3: Write the header**

Create `src/ui/desktop/definition/definition_header_form.h`:

```cpp
#pragma once
#include <QGridLayout>
#include <QLineEdit>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTextEdit>

#include "src/backend/definition/definition_writer.h"
#include "src/backend/ports/result.h"

namespace fastecu::ui
{

// The editors of one ROM-header form, keyed by each editor's objectName().
// `notes` is the only field rendered as a QTextEdit; everything else is a
// QLineEdit. Both lists are non-owning -- the QGridLayout passed to
// build_header_form owns the widgets through Qt's parent chain.
struct HeaderFormEditors
{
    QList<QLineEdit *> line_edits;
    QList<QTextEdit *> text_edits;
};

// Populates `grid` with one labelled row per entry in `names`, and returns
// the editors created. `labels` supplies the human-readable row labels and
// must be at least as long as `names`. `values` prefills the editors when
// non-empty; pass {} to leave every field blank. The `notes` field becomes a
// QTextEdit placed one row lower and spanning both columns, matching the
// legacy layout.
HeaderFormEditors build_header_form(QGridLayout *grid, const QStringList& labels, const QStringList& names,
                                    const QStringList& values);

// Maps the form's editors onto a DefinitionHeaderInput by objectName().
// `internalidaddress` is parsed as hex: empty yields nullopt, unparseable
// yields ErrorKind::InvalidConfig. Only xmlid and internalidaddress are
// trimmed -- every other field is taken verbatim, matching legacy.
fastecu::Result<fastecu::definition::DefinitionHeaderInput> definition_header_input(const HeaderFormEditors& editors);

// The raw text of the line edit with this objectName, or an empty QString
// when no such editor exists.
QString line_edit_value(const HeaderFormEditors& editors, const QString& name);

// Strips a single trailing '.' then appends ".xml" when not already
// present. Order matters: "foo." becomes "foo.xml", not "foo..xml".
QString normalize_xml_suffix(QString filename);

} // namespace fastecu::ui
```

- [ ] **Step 4: Write the implementation**

Create `src/ui/desktop/definition/definition_header_form.cpp`:

```cpp
#include "src/ui/desktop/definition/definition_header_form.h"

#include <cstdint>
#include <optional>

#include <QHash>
#include <QLabel>

namespace fastecu::ui
{

HeaderFormEditors build_header_form(QGridLayout *grid, const QStringList& labels, const QStringList& names,
                                    const QStringList& values)
{
    HeaderFormEditors editors;
    for (int index = 0; index < names.length(); index++)
    {
        auto *label = new QLabel(labels.at(index));
        grid->addWidget(label, index, 0);

        const QString value = index < values.length() ? values.at(index) : QString();
        if (names.at(index) == "notes")
        {
            auto *editor = new QTextEdit();
            editor->setObjectName(names.at(index));
            editor->setText(value);
            // One row lower and spanning both columns, as legacy did.
            grid->addWidget(editor, index + 1, 0, 1, 2);
            editors.text_edits.append(editor);
        }
        else
        {
            auto *editor = new QLineEdit();
            editor->setObjectName(names.at(index));
            editor->setText(value);
            grid->addWidget(editor, index, 1);
            editors.line_edits.append(editor);
        }
    }
    return editors;
}

fastecu::Result<fastecu::definition::DefinitionHeaderInput> definition_header_input(const HeaderFormEditors& editors)
{
    QHash<QString, QString> fields;
    for (const QLineEdit *editor : editors.line_edits)
    {
        fields.insert(editor->objectName(), editor->text());
    }
    for (const QTextEdit *editor : editors.text_edits)
    {
        fields.insert(editor->objectName(), editor->toPlainText());
    }

    std::optional<std::uint64_t> internalIdAddress;
    const QString addressText = fields.value("internalidaddress").trimmed();
    if (!addressText.isEmpty())
    {
        bool validAddress = false;
        const std::uint64_t parsedAddress = addressText.toULongLong(&validAddress, 16);
        if (!validAddress)
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                 "definition internal ID address is not a valid integer");
        }
        internalIdAddress = parsedAddress;
    }

    return fastecu::definition::DefinitionHeaderInput{
        .xml_id = fields.value("xmlid").trimmed().toStdString(),
        .internal_id = fields.value("internalidstring").toStdString(),
        .ecu_id = fields.value("ecuid").toStdString(),
        .internal_id_address = internalIdAddress,
        .metadata =
            fastecu::definition::RomMetadata{
                .make = fields.value("make").toStdString(),
                .market = fields.value("market").toStdString(),
                .model = fields.value("model").toStdString(),
                .submodel = fields.value("submodel").toStdString(),
                .transmission = fields.value("transmission").toStdString(),
                .year = fields.value("year").toStdString(),
                .flash_method = fields.value("flashmethod").toStdString(),
                .memory_model = fields.value("memmodel").toStdString(),
                .checksum_module = fields.value("checksummodule").toStdString(),
            },
        .include = fields.value("include").toStdString(),
        .notes = fields.value("notes").toStdString(),
    };
}

QString line_edit_value(const HeaderFormEditors& editors, const QString& name)
{
    for (const QLineEdit *editor : editors.line_edits)
    {
        if (editor->objectName() == name)
        {
            return editor->text();
        }
    }
    return {};
}

QString normalize_xml_suffix(QString filename)
{
    if (filename.endsWith(QString(".")))
    {
        filename.remove(filename.length() - 1, 1);
    }
    if (!filename.endsWith(QString(".xml")))
    {
        filename.append(QString(".xml"));
    }
    return filename;
}

} // namespace fastecu::ui
```

Cross-check `definition_header_input` and `line_edit_value` against their originals at `src/backend/definitions/file_actions.cpp:27` and `:39` — the bodies must be identical apart from the parameter type.

- [ ] **Step 5: Create the Bazel package**

Create `src/ui/desktop/definition/BUILD.bazel`:

```python
load("//bazel:gtest_targets.bzl", "fastecu_gtest")
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "qt_cc_library")

package(default_visibility = ["//src/ui:__subpackages__"])

# Qt-linked (QLineEdit/QTextEdit/QGridLayout) but declares no QObject of its
# own, so the header goes through normal_hdrs rather than hdrs -- matching
# //src/ui/desktop/menu:menu_builder.
qt_cc_library(
    name = "definition_header_form",
    srcs = ["definition_header_form.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["definition_header_form.h"],
    deps = QT_DEPS + ["//src/backend/definition:definition_writer"],
)

fastecu_gtest(
    name = "definition_header_form_test",
    srcs = ["definition_header_form_test.cpp"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [":definition_header_form"],
)
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test --config=release //src/ui/desktop/definition:definition_header_form_test --test_output=errors`
Expected: PASS, 11 cases.

- [ ] **Step 7: Commit**

```bash
prek run --all-files
git add src/ui/desktop/definition/
git commit -m "feat(ui): add the definition header form helpers

Moves the widget-to-DefinitionHeaderInput mapping into a UI package and
puts it under test for the first time: hex parsing of internalidaddress,
the xmlid-only trimming, notes as the single QTextEdit, and the trailing
'.' rule in the xml suffix normalization.

The originals stay in file_actions.cpp until 6a-2's final task -- the
backend cannot depend on a UI package to reach them."
```

---

### Task 2: The authoring dialog

**Files:**
- Create: `src/ui/desktop/definition/definition_authoring_dialog.h`
- Create: `src/ui/desktop/definition/definition_authoring_dialog.cpp`
- Test: `src/ui/desktop/definition/definition_authoring_dialog_test.cpp`
- Modify: `src/ui/desktop/definition/BUILD.bazel`

**Interfaces:**
- Consumes: everything Task 1 produced (`HeaderFormEditors`, `build_header_form`, `definition_header_input`, `line_edit_value`, `normalize_xml_suffix`); `FileActions` for its `EcuCalDefStructure` / `ConfigValuesStructure` aliases, the static `collect_ecuflash_base_header_fields`, and — from Task 3 onward — the public `submit_new_definition` / `submit_imported_definition`. **Those two are still private at this point**, so Task 3 promotes them; this task's Step 5 does the promotion so this package compiles.
- Produces: `class fastecu::ui::DefinitionAuthoringDialog : public QObject` with

  ```cpp
  DefinitionAuthoringDialog(FileActions& file_actions, fastecu::IFileRepository& repository, QWidget *parent);
  bool create_new_definition(FileActions::EcuCalDefStructure *ecuCalDef);
  bool use_existing_definition(FileActions::EcuCalDefStructure *ecuCalDef);
  ```

  Both return `true` when the ROM may continue being used (the legacy `return ecuCalDef` case, including the user cancelling) and `false` on a genuine error (the legacy `return nullptr`). Signals `LOG_E`/`LOG_W`/`LOG_I`/`LOG_D`, each `(QString message, bool timestamp, bool linefeed)`, matching `FileActions`'s. Task 3 calls both methods.

- [ ] **Step 1: Write the failing test**

Create `src/ui/desktop/definition/definition_authoring_dialog_test.cpp`:

```cpp
#include "src/ui/desktop/definition/definition_authoring_dialog.h"

#include <memory>

#include <QApplication>
#include <QSignalSpy>
#include <QWidget>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"

using fastecu::ui::DefinitionAuthoringDialog;

namespace
{

class AuthoringDialogEnvironment final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        static int argc = 1;
        static char program[] = "definition_authoring_dialog_test";
        static char *argv[] = {program, nullptr};
        app_ = std::make_unique<QApplication>(argc, argv);
    }

  private:
    std::unique_ptr<QApplication> app_;
};

const auto *authoring_dialog_environment = ::testing::AddGlobalTestEnvironment(new AuthoringDialogEnvironment);

} // namespace

// The two entry points are modal and are deliberately left untested, per the
// step-6a design: "the modal wiring itself is left untested, consistent with
// existing desktop UI practice." What is pinned here is that the object
// constructs without a live FileActions dialog parent and exposes the four
// log signals MainWindow connects -- a missing Q_OBJECT or a renamed signal
// links fine and fails only at runtime, which is exactly what this catches.
TEST(DefinitionAuthoringDialogTest, ConstructsAndExposesTheFourLogSignals)
{
    QWidget parent;
    fastecu::InMemoryFileRepository repository;
    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository config_repository;
    QtAtomicFileWriter writer;
    FileActions file_actions(file_system, resource_bundle, config_repository, writer);

    DefinitionAuthoringDialog dialog(file_actions, repository, &parent);

    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_E).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_W).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_I).isValid());
    EXPECT_TRUE(QSignalSpy(&dialog, &DefinitionAuthoringDialog::LOG_D).isValid());
}
```

The four adapter names and the `FileActions` constructor order above are verified against the source: the classes are `QtFileSystem`, `QtResourceBundle`, `QtFileRepository`, and `QtAtomicFileWriter` in `src/platform/desktop/common/ports/`, all in the global namespace, and `FileActions(fileSystem, resourceBundle, fileRepository, atomicFileWriter)` is the order `src/backend/definitions/file_actions_parsing_test.cpp:124` uses.

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/ui/desktop/definition:definition_authoring_dialog_test`
Expected: FAIL — the target does not exist yet.

- [ ] **Step 3: Write the dialog header**

Create `src/ui/desktop/definition/definition_authoring_dialog.h`:

```cpp
#pragma once
#include <QObject>
#include <QString>
#include <QWidget>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/file_repository.h"

namespace fastecu::ui
{

// The two interactive definition-authoring wizards, moved out of
// FileActions (file_actions.cpp:854 and :979 at f74c525). Each collects ROM
// header fields in a modal form, asks for a destination path, and hands the
// result to FileActions's portable submit_*_definition seam.
//
// Dialogs are parented to the QWidget passed in, not to FileActions: 6a-3
// removes QWidget from FileActions entirely, and the legacy
// `new QDialog(this)` leaked one dialog per invocation.
class DefinitionAuthoringDialog : public QObject
{
    Q_OBJECT

  public:
    DefinitionAuthoringDialog(FileActions& file_actions, fastecu::IFileRepository& repository,
                              QWidget *parent = nullptr);

    // Both return true when the ROM may continue to be used -- including
    // when the user cancels out, which legacy signalled by returning
    // ecuCalDef unchanged. false means a genuine failure (legacy nullptr).
    bool create_new_definition(FileActions::EcuCalDefStructure *ecuCalDef);
    bool use_existing_definition(FileActions::EcuCalDefStructure *ecuCalDef);

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  private:
    FileActions& fileActions_;
    fastecu::IFileRepository& repository_;
    QWidget *parent_;
};

} // namespace fastecu::ui
```

- [ ] **Step 4: Write the dialog implementation**

Create `src/ui/desktop/definition/definition_authoring_dialog.cpp`. Transcribe the two legacy bodies, routing every `emit LOG_D` through this class's signal, every `this` dialog parent through `parent_`, and the three duplicated fragments through the shared helpers:

```cpp
#include "src/ui/desktop/definition/definition_authoring_dialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

#include "src/ui/desktop/definition/definition_header_form.h"

namespace fastecu::ui
{
namespace
{

// The legacy "No file selected!" nag: Ok re-opens the chooser, Cancel gives
// up. `noun` is "create" for a save chooser and "select" for an open one,
// matching the two legacy wordings verbatim.
bool user_wants_to_retry(QWidget *parent, const QString& noun)
{
    QDialog dialog(parent);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel("No file selected!\n\nIf you still want to " + noun +
                             " file click 'Ok'\nIf you want to continue to use ROM without definition, click 'Cancel'");
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(label);
    layout->addWidget(buttons);
    return dialog.exec() != QDialog::Rejected;
}

enum class PathMode
{
    Open,
    Save
};

// The legacy chooser-plus-nag loop, identical in all three legacy call
// sites bar the chooser call and the nag's verb. Returns an empty QString
// when the user gives up.
QString select_definition_path(QWidget *parent, const QString& directory, PathMode mode)
{
    QString filename;
    bool gaveUp = false;
    while (filename.isEmpty() && !gaveUp)
    {
        filename = mode == PathMode::Save
                       ? QFileDialog::getSaveFileName(parent, QObject::tr("Select definition file"), directory,
                                                      QObject::tr("Definition file (*.xml)"))
                       : QFileDialog::getOpenFileName(parent, QObject::tr("Select definition file"), directory,
                                                      QObject::tr("Definition file (*.xml)"));
        if (filename.isEmpty() && !user_wants_to_retry(parent, mode == PathMode::Save ? "create" : "select"))
        {
            gaveUp = true;
        }
    }
    return filename;
}

// The shared "Please provide ROM Information:" modal. Returns the editors
// and whether the user accepted.
struct HeaderDialogResult
{
    bool accepted{false};
    HeaderFormEditors editors;
};

HeaderDialogResult run_header_dialog(QWidget *parent, const QStringList& labels, const QStringList& names,
                                     const QStringList& values)
{
    QDialog dialog(parent);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel("Please provide ROM Information:"));

    auto *grid = new QGridLayout();
    HeaderDialogResult result;
    result.editors = build_header_form(grid, labels, names, values);
    layout->addLayout(grid);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.setMinimumWidth(500);
    result.accepted = dialog.exec() == QDialog::Accepted;
    return result;
}

} // namespace

DefinitionAuthoringDialog::DefinitionAuthoringDialog(FileActions& file_actions, fastecu::IFileRepository& repository,
                                                     QWidget *parent)
    : QObject(parent), fileActions_(file_actions), repository_(repository), parent_(parent)
{
}

bool DefinitionAuthoringDialog::create_new_definition(FileActions::EcuCalDefStructure *ecuCalDef)
{
    FileActions::ConfigValuesStructure *configValues = &fileActions_.ConfigValuesStruct;

    emit LOG_D("Create header", true, true);
    const HeaderDialogResult form =
        run_header_dialog(parent_, ecuCalDef->DefHeaderStrings, ecuCalDef->DefHeaderNames, {});
    if (!form.accepted)
    {
        return true;
    }

    QString filename = select_definition_path(parent_, configValues->ecuflash_definition_files_directory,
                                              PathMode::Save);
    if (filename.isEmpty())
    {
        return true;
    }
    filename = normalize_xml_suffix(filename);

    const auto input = definition_header_input(form.editors);
    if (!input.has_value())
    {
        emit LOG_E("Unable to create definition: " + QString::fromStdString(input.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to create definition: " + QString::fromStdString(input.error().detail));
        return false;
    }
    for (const QLineEdit *editor : form.editors.line_edits)
    {
        if (editor->objectName() != "include")
        {
            emit LOG_D(editor->text(), true, true);
        }
    }

    const fastecu::Status status = fileActions_.submit_new_definition(filename.toStdString(), *input);
    if (!status.has_value())
    {
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to open definition file for writing: " +
                                 QString::fromStdString(status.error().detail));
        return false;
    }

    configValues->ecuflash_def_cal_id.append(QString::fromStdString(input->xml_id));
    configValues->ecuflash_def_cal_id_addr.append(line_edit_value(form.editors, "internalidaddress"));
    configValues->ecuflash_def_ecu_id.append(line_edit_value(form.editors, "ecuid"));
    configValues->ecuflash_def_filename.append(filename);
    return true;
}

bool DefinitionAuthoringDialog::use_existing_definition(FileActions::EcuCalDefStructure *ecuCalDef)
{
    FileActions::ConfigValuesStructure *configValues = &fileActions_.ConfigValuesStruct;

    const QString source = select_definition_path(parent_, configValues->ecuflash_definition_files_directory,
                                                  PathMode::Open);
    if (source.isEmpty())
    {
        return true;
    }

    const auto sourceContents = repository_.read(source.toStdString());
    if (!sourceContents.has_value())
    {
        emit LOG_E("Unable to import definition: " + QString::fromStdString(sourceContents.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"), "Unable to open definition file for reading");
        return false;
    }
    const QByteArray sourceBytes(reinterpret_cast<const char *>(sourceContents->data()),
                                 static_cast<qsizetype>(sourceContents->size()));
    const QStringList headerData =
        FileActions::collect_ecuflash_base_header_fields(*ecuCalDef, {QString::fromUtf8(sourceBytes)});

    // headerData is a flat (name, value, name, value, ...) list; split it
    // into the two parallel lists build_header_form expects.
    QStringList names;
    QStringList values;
    for (int i = 0; i + 1 < headerData.length(); i += 2)
    {
        names.append(headerData.at(i));
        values.append(headerData.at(i + 1));
    }

    emit LOG_D("Create header", true, true);
    const HeaderDialogResult form = run_header_dialog(parent_, ecuCalDef->DefHeaderStrings, names, values);
    if (!form.accepted)
    {
        return true;
    }

    QString filename = select_definition_path(parent_, configValues->ecuflash_definition_files_directory,
                                              PathMode::Save);
    if (filename.isEmpty())
    {
        return true;
    }
    filename = normalize_xml_suffix(filename);

    const auto input = definition_header_input(form.editors);
    if (!input.has_value())
    {
        emit LOG_E("Unable to import definition: " + QString::fromStdString(input.error().detail), true, true);
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to import definition: " + QString::fromStdString(input.error().detail));
        return false;
    }
    emit LOG_D("Write to file", true, true);
    for (const QLineEdit *editor : form.editors.line_edits)
    {
        if (editor->objectName() != "include")
        {
            emit LOG_D(editor->text(), true, true);
        }
    }

    const fastecu::Status status =
        fileActions_.submit_imported_definition(source.toStdString(), filename.toStdString(), *input);
    if (!status.has_value())
    {
        QMessageBox::warning(parent_, tr("Definition file"),
                             "Unable to open definition file for writing: " +
                                 QString::fromStdString(status.error().detail));
        return false;
    }

    configValues->ecuflash_def_cal_id.append(QString::fromStdString(input->xml_id));
    configValues->ecuflash_def_cal_id_addr.append(line_edit_value(form.editors, "internalidaddress"));
    configValues->ecuflash_def_ecu_id.append(line_edit_value(form.editors, "ecuid"));
    configValues->ecuflash_def_filename.append(filename);
    return true;
}

} // namespace fastecu::ui
```

Two divergences from legacy worth stating in your report, both consequences of the move rather than choices: the legacy private helper `log_definition_error` is unreachable from here, so its two call sites become `emit LOG_E` carrying the same detail text; and legacy's `use_existing` reused one `isFileSelected` flag across its open and save loops, which `select_definition_path` gives each loop its own copy of — behaviourally identical, because legacy always returned early between the two when the flag was set.

- [ ] **Step 5: Promote the two submit methods to public**

In `src/backend/definitions/file_actions.h`, move these two declarations from the `private:` section up into `public:`, keeping their signatures and adding a brief comment:

```cpp
    // Public so src/ui/desktop/definition's authoring dialog can reach the
    // portable submit seam; the interactive wizards that used to wrap them
    // moved there in step 6a-2.
    fastecu::Status submit_new_definition(std::string_view destination,
                                          const fastecu::definition::DefinitionHeaderInput&);
    fastecu::Status submit_imported_definition(std::string_view source, std::string_view destination,
                                               const fastecu::definition::DefinitionHeaderInput&);
```

Leave `remember_submitted_ecuflash_handle` private — it is an implementation detail of those two.

- [ ] **Step 6: Add the Bazel targets**

Append to `src/ui/desktop/definition/BUILD.bazel`:

```python
# Declares Q_OBJECT for its four LOG_* signals, so the header goes in
# `hdrs` (which rules_qt's qt_cc_library moc's) rather than normal_hdrs --
# a Q_OBJECT header in normal_hdrs links but fails at runtime.
# //src/platform/desktop/common/ports:ports is the precedent: qt_event_sink.h
# sits in hdrs marked "# MOC header" while its non-QObject siblings do not.
qt_cc_library(
    name = "definition_authoring_dialog",
    srcs = ["definition_authoring_dialog.cpp"],
    hdrs = ["definition_authoring_dialog.h"],  # MOC header
    copts = COMMON_COPTS,
    normal_hdrs = [],
    deps = QT_DEPS + [
        ":definition_header_form",
        "//src/backend/definition:definition_writer",
        "//src/backend/definitions",
        "//src/backend/ports",
    ],
)

fastecu_gtest(
    name = "definition_authoring_dialog_test",
    srcs = ["definition_authoring_dialog_test.cpp"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [
        ":definition_authoring_dialog",
        "//src/backend/ports/testing:in_memory_file_repository",
        "//src/platform/desktop/common/ports",
    ],
)
```

`qt_cc_library` is re-exported from `rules_qt` in `bazel/qt_targets.bzl:7`; it moc's whatever is in `hdrs` and leaves `normal_hdrs` alone. That is why this target inverts Task 1's split — `definition_header_form.h` declares no `QObject` and stays in `normal_hdrs`, while this header must be in `hdrs`.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `bazel test --config=release //src/ui/desktop/definition:all --test_output=errors`
Expected: PASS — 11 form cases plus 1 dialog case.

- [ ] **Step 8: Commit**

```bash
prek run --all-files
git add src/ui/desktop/definition/ src/backend/definitions/file_actions.h
git commit -m "feat(ui): add DefinitionAuthoringDialog

Reproduces the two interactive definition wizards in the UI layer,
sharing one nag-loop path chooser and one header form where the legacy
pair duplicated ~60 lines. Dialogs are stack-allocated and parented to
the caller rather than to FileActions, which 6a-3 stops being a QWidget.

submit_new_definition and submit_imported_definition become public so
the dialog can reach the portable submit seam. The legacy originals are
still in place and still wired; the next task swaps and deletes them."
```

---

### Task 3: Swap MainWindow and delete the originals

**Files:**
- Modify: `src/ui/desktop/mainwindow.h`
- Modify: `src/ui/desktop/mainwindow.cpp` (the `MainWindow` constructor's wiring region, and `prompt_for_missing_definition` at line 1460)
- Modify: `src/ui/desktop/BUILD.bazel`
- Modify: `src/backend/definitions/file_actions.h` (delete two declarations)
- Modify: `src/backend/definitions/file_actions.cpp` (delete two bodies plus two anonymous-namespace helpers)

**Interfaces:**
- Consumes: `fastecu::ui::DefinitionAuthoringDialog` from Task 2 — constructor `(FileActions&, fastecu::IFileRepository&, QWidget *)`, methods `create_new_definition(FileActions::EcuCalDefStructure *) -> bool` and `use_existing_definition(FileActions::EcuCalDefStructure *) -> bool`, signals `LOG_E/W/I/D(QString, bool, bool)`.
- Produces: nothing. This task closes the seam.

`MainWindow` already holds `fileActions` (a `FileActions *`) and `m_configFileRepository` (a `QtFileRepository` **value** member at `mainwindow.h:189`) — the very repository object `FileActions` was constructed with, so the dialog reads the same files the backend would have.

- [ ] **Step 1: Confirm the current call site**

Run: `grep -n "create_new_definition_for_rom\|use_existing_definition_for_rom\|apply_missing_definition_defaults" src/ui/desktop/mainwindow.cpp`
Expected: three hits inside `prompt_for_missing_definition`. Read that whole function before editing — the radio-button chooser stays exactly as it is; only the three branch bodies change.

- [ ] **Step 2: Add the member and wire it**

In `src/ui/desktop/mainwindow.h`, add the include and a member beside the existing `fileActions` declaration:

```cpp
#include "src/ui/desktop/definition/definition_authoring_dialog.h"
```

```cpp
    fastecu::ui::DefinitionAuthoringDialog *definitionAuthoringDialog = nullptr;
```

In `src/ui/desktop/mainwindow.cpp`, immediately after the four existing `QObject::connect(fileActions, &FileActions::LOG_*, syslogger, ...)` lines, construct and connect it:

```cpp
    definitionAuthoringDialog =
        new fastecu::ui::DefinitionAuthoringDialog(*fileActions, m_configFileRepository, this);
    QObject::connect(definitionAuthoringDialog, &fastecu::ui::DefinitionAuthoringDialog::LOG_E, syslogger,
                     &SystemLogger::log_messages);
    QObject::connect(definitionAuthoringDialog, &fastecu::ui::DefinitionAuthoringDialog::LOG_W, syslogger,
                     &SystemLogger::log_messages);
    QObject::connect(definitionAuthoringDialog, &fastecu::ui::DefinitionAuthoringDialog::LOG_I, syslogger,
                     &SystemLogger::log_messages);
    QObject::connect(definitionAuthoringDialog, &fastecu::ui::DefinitionAuthoringDialog::LOG_D, syslogger,
                     &SystemLogger::log_messages);
```

- [ ] **Step 3: Swap the two branches in `prompt_for_missing_definition`**

Replace `fileActions->create_new_definition_for_rom(ecuCalDef)` with
`definitionAuthoringDialog->create_new_definition(ecuCalDef)` and
`fileActions->use_existing_definition_for_rom(ecuCalDef)` with
`definitionAuthoringDialog->use_existing_definition(ecuCalDef)`.

The legacy calls discarded their return value, so the new `bool` may be discarded too — but read the surrounding code first: if the existing branch tests the result for null, translate that test to `!` on the bool rather than dropping it. Leave the `apply_missing_definition_defaults` branch untouched; it stays in the backend.

- [ ] **Step 4: Add the Bazel dependency**

In `src/ui/desktop/BUILD.bazel`, add to the `deps` of the target compiling `mainwindow.cpp` (the one already listing `//src/ui/desktop/menu:menu_builder`):

```python
        "//src/ui/desktop/definition:definition_authoring_dialog",
```

- [ ] **Step 5: Build to verify the wiring compiles**

Run: `bazel build --config=release //:fastecu`
Expected: SUCCESS. The legacy functions still exist but now have no callers.

- [ ] **Step 6: Delete the legacy declarations and bodies**

In `src/backend/definitions/file_actions.h`, delete these two declarations and the comment block above them:

```cpp
    EcuCalDefStructure *create_new_definition_for_rom(FileActions::EcuCalDefStructure *ecuCalDef);
    EcuCalDefStructure *use_existing_definition_for_rom(FileActions::EcuCalDefStructure *ecuCalDef);
```

In `src/backend/definitions/file_actions.cpp`, delete both function bodies in full (locate with `grep -n "FileActions::create_new_definition_for_rom\|FileActions::use_existing_definition_for_rom"`), and then delete the two anonymous-namespace helpers they were the only users of — `lineEditValue` at line 27 and `definitionHeaderInput` at line 39. Confirm nothing else calls them first:

```bash
grep -n "lineEditValue\|definitionHeaderInput" src/backend/definitions/*.cpp
```

Then remove Qt includes from `file_actions.h` that no longer have a user, checking each before deleting:

```bash
grep -n "QLineEdit\|QTextEdit\|QGridLayout\|QRadioButton\|QDialogButtonBox\|QVBoxLayout\|QPushButton" src/backend/definitions/*.cpp src/backend/definitions/*.h
```

**Include removal is best-effort, not required.** About 85 files include `file_actions.h`, including files under the two forbidden paths, and any may rely on receiving those Qt headers transitively. Validate every removal with the full-tree run in Step 7 — not with the narrow grep — and restore any include whose removal breaks a file you may not edit. Never edit a forbidden-path file to make a removal work.

- [ ] **Step 7: Run the full suite**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //...`
Expected: PASS across all targets, including `//:portable_closure`, `//:serial_compat_allowlist`, and `//:openpty_includes`. Expect 5 Windows-only skips.

- [ ] **Step 8: Verify the parallel-branch constraint**

```bash
git diff --name-only origin/master | grep -E 'src/platform/desktop/common/flash/legacy/|src/ui/desktop/flash/' || echo "OK: no drain-owned files touched"
```

Expected: `OK: no drain-owned files touched`.

- [ ] **Step 9: Commit**

```bash
prek run --all-files
git add src/ui/desktop/mainwindow.h src/ui/desktop/mainwindow.cpp src/ui/desktop/BUILD.bazel \
        src/backend/definitions/file_actions.h src/backend/definitions/file_actions.cpp
git commit -m "refactor: drive definition authoring from the UI dialog

MainWindow's prompt_for_missing_definition now calls
DefinitionAuthoringDialog, and the two ~150-line wizards leave
src/backend/definitions along with the two form helpers they owned.
apply_missing_definition_defaults stays in the backend; it has no UI."
```

---

## Verification

After Task 3, these all hold:

- `grep -rn "create_new_definition_for_rom\|use_existing_definition_for_rom" src/` returns no declaration or call site; provenance comments naming them may remain.
- `grep -c "QFileDialog" src/backend/definitions/file_actions.cpp` is **0** — the remaining `QMessageBox` sites are 6a-3's work, but no file chooser survives in the backend.
- `bazel test --config=release //...` is green.
- `git diff --name-only origin/master` lists no path under `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`.
- `src/backend/definitions/file_actions.cpp` is roughly 300 lines shorter, from 1353 down to about 1050.
