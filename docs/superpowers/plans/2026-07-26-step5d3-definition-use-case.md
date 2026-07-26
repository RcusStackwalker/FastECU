# Step 5d-3 Portable Definition Use Case Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Qt-bound RomRaider/EcuFlash parsers and definition-file writers with strict, atomic portable use cases while keeping `FileActions` and `MainWindow` source-compatible.

**Architecture:** Separate format parsers produce unresolved typed documents, a shared resolver loads and merges inheritance chains, and `DefinitionService` owns discovery, matching, loading, creation, and import orchestration through ports. A Qt-linked adapter commits successful values to the legacy structures in one operation; UI dialogs remain in `FileActions`.

**Tech Stack:** C++23, Bazel, GoogleTest, Qt 6 compatibility adapters/tests, pugixml 1.15, `std::expected` via `fastecu::Result<T>`.

## Global Constraints

- Portable production code lives in `src/backend/definition/`; never add it beside the legacy `src/backend/definitions/*.cpp` glob.
- Portable model/parser/resolver/service targets depend on no Qt or platform targets and must enter `//:portable_closure`.
- Parse, resolve, validate, and adapt into temporary values; caller-owned state changes only after complete success.
- Preserve valid-file results, but return `InvalidConfig` for malformed XML, invalid strict booleans/numbers, unresolved references, cycles, and inconsistent models.
- Use `InvalidConfig` with contextual detail for missing definitions/parents/sources/ROM matches; propagate port errors unchanged (`Internal` for current operational I/O adapters).
- Preserve `FileActions` public signatures and all `MainWindow` call sites.
- Retain dialogs, file selection, and retry/cancel policy in Qt UI code until step 6.
- Use `IAtomicFileWriter` with `QSaveFile` for definition replacement; do not strengthen `IFileRepository::write`.
- Keep `docs/coverage-baseline.txt` absent and meet >=80% coverage for new code.
- Do not touch the unrelated untracked `docs/modularization-plan.md` or `scripts/__pycache__/`.

---

## File structure

### New portable package

- `src/backend/definition/definition_model.{h,cpp}` — typed catalog, unresolved/resolved definition values, validation helpers.
- `src/backend/definition/romraider_parser.{h,cpp}` — RomRaider XML grammar only.
- `src/backend/definition/ecuflash_parser.{h,cpp}` — EcuFlash XML grammar only.
- `src/backend/definition/definition_resolver.{h,cpp}` — inheritance loading, cycle detection, merge, final validation.
- `src/backend/definition/definition_service.{h,cpp}` — discovery, catalogs, ROM matching, load orchestration.
- `src/backend/definition/definition_writer.{h,cpp}` — creation/import tree transformation and deterministic serialization.
- `src/backend/definition/BUILD.bazel` — focused libraries and portable tests.

### New/changed ports and desktop adapters

- `src/backend/ports/atomic_file_writer.h` — portable atomic replacement interface.
- `src/platform/desktop/common/ports/qt_atomic_file_writer.{h,cpp}` — `QSaveFile` implementation.
- `src/backend/definitions/ecu_cal_def.h` — extracted unchanged legacy Qt value type.
- `src/backend/definitions/file_actions.{h,cpp}` — aliases/wiring and UI-only wrappers.
- `src/backend/definition/legacy_definition_adapter.{h,cpp}` — Qt-linked atomic mapping boundary.
- `src/backend/definitions/BUILD.bazel`, `src/platform/desktop/common/ports/BUILD.bazel` — narrow targets/dependencies.

### Removed legacy implementation

- `src/backend/definitions/file_defs_romraider.cpp`
- `src/backend/definitions/file_defs_ecuflash.cpp`

---

### Task 1: Extract the legacy calibration-definition value

**Files:**
- Create: `src/backend/definitions/ecu_cal_def.h`
- Modify: `src/backend/definitions/file_actions.h`
- Modify: `src/backend/definitions/BUILD.bazel`
- Modify: `tests/test_model_validation.cpp`

**Interfaces:**
- Consumes: the current nested `FileActions::EcuCalDefStructure`.
- Produces: `fastecu::definitions::EcuCalDefStructure` and the source-compatible `FileActions::EcuCalDefStructure` alias.

- [ ] **Step 1: Add a compile/behavior characterization**

Add to `tests/test_model_validation.cpp`:

```cpp
#include <type_traits>
#include "src/backend/definitions/ecu_cal_def.h"

static_assert(std::is_same_v<FileActions::EcuCalDefStructure,
                             fastecu::definitions::EcuCalDefStructure>);

void extracted_ecu_cal_def_keeps_legacy_defaults()
{
    FileActions::EcuCalDefStructure value;
    QCOMPARE(value.RomInfoNames.at(FileActions::XmlId), QString("xmlid"));
    QCOMPARE(value.DefHeaderNames.last(), QString("notes"));
}
```

- [ ] **Step 2: Run the focused test and verify it fails**

Run:

```bash
bazel test --config=release //tests:test_model_validation
```

Expected: compilation fails because `ecu_cal_def.h` and the alias do not exist.

- [ ] **Step 3: Move the struct without changing fields or defaults**

Create `ecu_cal_def.h` with the complete current struct inside:

```cpp
namespace fastecu::definitions {
struct EcuCalDefStructure {
    // Move every current field and default initializer here verbatim.
};
}  // namespace fastecu::definitions
```

Replace the nested struct in `file_actions.h` with:

```cpp
#include "src/backend/definitions/ecu_cal_def.h"
using EcuCalDefStructure = fastecu::definitions::EcuCalDefStructure;
EcuCalDefStructure EcuCalDefStruct;
```

Keep `RomInfoEnum` in `FileActions` for compatibility. Add an `ecu_cal_def`
`qt_cc_library` leaf target following the existing `config_values` pattern,
and make `definitions` depend on it.

- [ ] **Step 4: Run focused tests and a definitions build**

Run:

```bash
bazel test --config=release //tests:test_model_validation
bazel build --config=release //src/backend/definitions:definitions
```

Expected: both pass without call-site edits.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definitions/ecu_cal_def.h src/backend/definitions/file_actions.h \
  src/backend/definitions/BUILD.bazel tests/test_model_validation.cpp
git commit -m "refactor: extract legacy calibration definition value"
```

---

### Task 2: Add the atomic definition-writer port

**Files:**
- Create: `src/backend/ports/atomic_file_writer.h`
- Create: `src/platform/desktop/common/ports/qt_atomic_file_writer.h`
- Create: `src/platform/desktop/common/ports/qt_atomic_file_writer.cpp`
- Create: `tests/test_qt_atomic_file_writer.cpp`
- Modify: `src/platform/desktop/common/ports/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Interfaces:**
- Produces:

```cpp
class IAtomicFileWriter {
  public:
    virtual ~IAtomicFileWriter() = default;
    virtual Status replace(std::string_view handle,
                           std::span<const std::uint8_t> data) = 0;
};
```

- [ ] **Step 1: Write desktop contract tests**

In `tests/test_qt_atomic_file_writer.cpp`, test:

```cpp
TEST(QtAtomicFileWriterTest, ReplacesExistingFile)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("definition.xml");
    write_test_file(path, "old");
    QtAtomicFileWriter writer;
    const std::array<std::uint8_t, 3> bytes{'n', 'e', 'w'};
    ASSERT_TRUE(writer.replace(path.toStdString(), bytes));
    EXPECT_EQ(read_test_file(path), "new");
}

TEST(QtAtomicFileWriterTest, InvalidDestinationPreservesExistingFile)
{
    // Use a destination whose parent does not exist; assert ErrorKind::Internal.
}
```

- [ ] **Step 2: Run and verify the target fails to build**

Run:

```bash
bazel test --config=release //tests:test_qt_atomic_file_writer
```

Expected: target/header is missing.

- [ ] **Step 3: Implement the port and `QSaveFile` adapter**

The adapter implementation must:

```cpp
fastecu::Status QtAtomicFileWriter::replace(
    std::string_view handle, std::span<const std::uint8_t> data)
{
    QSaveFile file(QString::fromUtf8(handle.data(), static_cast<qsizetype>(handle.size())));
    if (!file.open(QIODevice::WriteOnly))
        return fastecu::fail(fastecu::ErrorKind::Internal, file.errorString().toStdString());
    if (file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<qint64>(data.size())) != static_cast<qint64>(data.size())) {
        file.cancelWriting();
        return fastecu::fail(fastecu::ErrorKind::Internal, file.errorString().toStdString());
    }
    if (!file.commit())
        return fastecu::fail(fastecu::ErrorKind::Internal, file.errorString().toStdString());
    return {};
}
```

- [ ] **Step 4: Run port and adapter tests**

Run:

```bash
bazel test --config=release //src/backend/ports/... //tests:test_qt_atomic_file_writer
```

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/backend/ports/atomic_file_writer.h \
  src/platform/desktop/common/ports/qt_atomic_file_writer.h \
  src/platform/desktop/common/ports/qt_atomic_file_writer.cpp \
  src/platform/desktop/common/ports/BUILD.bazel tests/test_qt_atomic_file_writer.cpp tests/BUILD.bazel
git commit -m "feat: add atomic file replacement port"
```

---

### Task 3: Introduce typed definition models and catalog rules

**Files:**
- Create: `src/backend/definition/definition_model.h`
- Create: `src/backend/definition/definition_model.cpp`
- Create: `src/backend/definition/definition_model_test.cpp`
- Create: `src/backend/definition/BUILD.bazel`

**Interfaces:**
- Produces:

```cpp
enum class DefinitionFormat { RomRaider, EcuFlash };
enum class IdEncoding { Ascii, Hex };

struct DefinitionIndexEntry {
    DefinitionFormat format;
    std::string definition_id;
    std::string internal_id;
    std::optional<std::uint64_t> internal_id_address;
    IdEncoding internal_id_encoding;
    std::string ecu_id;
    std::string source;
    std::vector<std::string> parents;
};

struct Scaling {
    std::string name, units, from_byte, to_byte, format;
    std::string minimum, maximum, coarse_increment, fine_increment;
    std::string storage_type, endian;
    std::vector<std::pair<std::string, std::string>> selections;
};

struct AxisDefinition {
    std::string type, name, units, format, storage_type, endian;
    std::optional<std::uint64_t> address;
    std::uint32_t size{1};
    std::string from_byte{"x"}, to_byte{"x"}, scaling_name;
};

struct CalibrationMap {
    std::string id, name, type, category, subcategory, description;
    std::optional<std::uint64_t> address;
    std::uint32_t x_size{1}, y_size{1};
    bool swap_xy{false}, flip_x{false}, flip_y{false};
    std::string level, user_level, scaling_name, storage_type, endian;
    AxisDefinition x_axis, y_axis;
};

struct RomIdentity {
    std::string xml_id, internal_id, ecu_id;
    std::optional<std::uint64_t> internal_id_address;
};

struct RomMetadata {
    std::string make, market, model, submodel, transmission, year;
    std::string flash_method, memory_model, checksum_module, file_size, notes;
};

struct UnresolvedDefinition {
    DefinitionFormat format;
    std::string source;
    RomIdentity identity;
    RomMetadata metadata;
    std::vector<std::string> parents;
    std::vector<CalibrationMap> maps;
    std::vector<Scaling> scalings;
};

struct RomDefinition : UnresolvedDefinition {
    std::vector<std::string> resolved_sources;
};

class DefinitionCatalog {
  public:
    static Result<DefinitionCatalog> create(std::vector<DefinitionIndexEntry>);
    Result<std::reference_wrapper<const DefinitionIndexEntry>>
        find(DefinitionFormat, std::string_view id) const;
    std::span<const DefinitionIndexEntry> entries() const;
};
```

- [ ] **Step 1: Write failing model/catalog tests**

Test successful lookup, deterministic first identical duplicate, rejection of
conflicting duplicates, empty ID, invalid missing source, and invalid parent
syntax:

```cpp
TEST(DefinitionCatalogTest, ConflictingDuplicateIsInvalidConfig)
{
    auto first = entry("A", "a.xml", {"BASE"});
    auto second = entry("A", "b.xml", {"OTHER"});
    auto result = DefinitionCatalog::create({first, second});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}
```

- [ ] **Step 2: Run and verify failure**

Run:

```bash
bazel test --config=release //src/backend/definition:definition_model_test
```

Expected: package/target is missing.

- [ ] **Step 3: Implement minimal typed values and catalog validation**

Implement value equality where tests need it. Preserve input order. Accept an
otherwise identical duplicate from a different source while storing the first
canonical entry; reject duplicates whose internal ID, address, encoding, or
parent list conflicts. Return contextual `InvalidConfig` details naming both
sources.

- [ ] **Step 4: Run tests**

Run:

```bash
bazel test --config=release //src/backend/definition:definition_model_test
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definition
git commit -m "feat: add typed definition models and catalog"
```

---

### Task 4: Parse RomRaider documents without I/O

**Files:**
- Create: `src/backend/definition/romraider_parser.h`
- Create: `src/backend/definition/romraider_parser.cpp`
- Create: `src/backend/definition/romraider_parser_test.cpp`
- Modify: `src/backend/definition/BUILD.bazel`

**Interfaces:**
- Consumes: XML byte span and source handle.
- Produces:

```cpp
Result<std::vector<DefinitionIndexEntry>> parse_romraider_index(
    std::span<const std::uint8_t> xml, std::string_view source);
Result<UnresolvedDefinition> parse_romraider_definition(
    std::span<const std::uint8_t> xml, std::string_view source,
    std::string_view definition_id);
```

- [ ] **Step 1: Port valid behavior into failing golden tests**

Use compact in-memory documents to cover multiple `<rom>` entries, `<rom
base="">`, `romid`, tables, scaling expressions, axes, selections, switch
conversion, categories, dimensions, and optional metadata. Assert typed values,
not legacy lists.

```cpp
TEST(RomRaiderParserTest, ParsesChildAndRecordsBaseWithoutResolvingIt)
{
    const auto xml = bytes(R"xml(
      <roms><rom base="BASE"><romid><xmlid>CHILD</xmlid>
      <internalidaddress>100</internalidaddress>
      <internalidstring>ABCD</internalidstring></romid>
      <table name="Fuel" address="200" type="2D"/></rom></roms>)xml");
    auto result = parse_romraider_definition(xml, "rr.xml", "CHILD");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->parents, std::vector<std::string>{"BASE"});
    EXPECT_EQ(result->maps.at(0).address, 0x200);
}
```

- [ ] **Step 2: Verify failure**

Run:

```bash
bazel test --config=release //src/backend/definition:romraider_parser_test
```

Expected: parser symbols are missing.

- [ ] **Step 3: Implement parser with pugixml**

Use `pugi::xml_document::load_buffer`. Add private helpers for contextual
element text, hexadecimal unsigned parsing, strict booleans, table parsing,
axis parsing, and scaling parsing. Do not read files or resolve `base`.

- [ ] **Step 4: Add strict failure tests and make them pass**

Add malformed XML, missing ID, invalid address, invalid boolean, duplicate map
identity, and wrong-root tests. Require `InvalidConfig` and source/element context
in `error.detail`.

Run:

```bash
bazel test --config=release //src/backend/definition:romraider_parser_test
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definition/romraider_parser.* src/backend/definition/romraider_parser_test.cpp \
  src/backend/definition/BUILD.bazel
git commit -m "feat: add portable RomRaider parser"
```

---

### Task 5: Parse EcuFlash documents without I/O

**Files:**
- Create: `src/backend/definition/ecuflash_parser.h`
- Create: `src/backend/definition/ecuflash_parser.cpp`
- Create: `src/backend/definition/ecuflash_parser_test.cpp`
- Modify: `src/backend/definition/BUILD.bazel`

**Interfaces:**
- Produces:

```cpp
Result<std::vector<DefinitionIndexEntry>> parse_ecuflash_index(
    std::span<const std::uint8_t> xml, std::string_view source);
Result<UnresolvedDefinition> parse_ecuflash_definition(
    std::span<const std::uint8_t> xml, std::string_view source);
```

- [ ] **Step 1: Write failing golden tests from current behavior**

Cover identity, include, metadata, global scalings, tables, nested axes,
selections, `address` over `storageaddress`, `sizex`/`sizey`, categories,
description/user levels, and value-format conversion:

```cpp
TEST(EcuFlashParserTest, AddressWinsAndStrictFlagsParse)
{
    auto result = parse_ecuflash_definition(bytes(R"xml(
      <rom><romid><xmlid>TEST</xmlid></romid>
      <table name="Fuel" address="1000" storageaddress="2000"
             swapxy="true" flipx="false" flipy="true"/></rom>)xml"), "test.xml");
    ASSERT_TRUE(result);
    EXPECT_EQ(result->maps.at(0).address, 0x1000);
    EXPECT_TRUE(result->maps.at(0).swap_xy);
    EXPECT_FALSE(result->maps.at(0).flip_x);
}
```

- [ ] **Step 2: Verify failure**

Run:

```bash
bazel test --config=release //src/backend/definition:ecuflash_parser_test
```

Expected: parser symbols are missing.

- [ ] **Step 3: Implement the EcuFlash grammar**

Parse the document rooted at `<rom>`, record `<include>` without resolving it,
parse global scalings before maps, and keep map scaling references symbolic.
Convert printf-like formats with the characterized legacy rule (`%.1f` →
`0.0`, no positive precision → `0`). Do not mutate defaults after returning.

- [ ] **Step 4: Add strict failures and make them pass**

Test malformed XML, invalid address/dimension, invalid `swapxy`/`flipx`/`flipy`,
missing ID, duplicate conflicting scaling, and structurally incomplete axis.

Run:

```bash
bazel test --config=release //src/backend/definition:ecuflash_parser_test
```

Expected: pass; invalid booleans are errors rather than warnings/defaults.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definition/ecuflash_parser.* src/backend/definition/ecuflash_parser_test.cpp \
  src/backend/definition/BUILD.bazel
git commit -m "feat: add portable EcuFlash parser"
```

---

### Task 6: Resolve and validate inheritance atomically

**Files:**
- Create: `src/backend/definition/definition_resolver.h`
- Create: `src/backend/definition/definition_resolver.cpp`
- Create: `src/backend/definition/definition_resolver_test.cpp`
- Modify: `src/backend/definition/BUILD.bazel`

**Interfaces:**
- Consumes:

```cpp
using DefinitionLoader =
    std::function<Result<UnresolvedDefinition>(DefinitionFormat,
                                               std::string_view id)>;
```

- Produces:

```cpp
Result<RomDefinition> resolve_definition(
    UnresolvedDefinition root, const DefinitionLoader& loader);
```

- [ ] **Step 1: Write failing graph-safety tests**

Test single/multi-level resolution, missing parent, self-cycle, `A → B → A`,
cross-format parent rejection, and a diamond graph whose shared base is loaded
once.

```cpp
TEST(DefinitionResolverTest, ReportsCompleteCycle)
{
    auto result = resolve_definition(doc("A", {"B"}), loader({
        {"B", doc("B", {"C"})}, {"C", doc("C", {"A"})}}));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("A -> B -> C -> A"));
}
```

- [ ] **Step 2: Verify failure**

Run:

```bash
bazel test --config=release //src/backend/definition:definition_resolver_test
```

Expected: resolver target is missing.

- [ ] **Step 3: Implement DFS, memoization, and base-to-child merge**

Use a per-call state object containing `visiting`, `stack`, and
`resolved_by_id`; retain no state globally. Merge metadata only when the child
provides a value. Match maps by stable `id` when present, otherwise by name;
child fields override supplied values and inherit absent values. Resolve
scalings by name and reject conflicting duplicate definitions.

- [ ] **Step 4: Add final-model validation tests**

Test unresolved scaling, zero required dimension, incomplete axis, duplicate
map key, and contradictory storage/selection metadata. Also assert the input
root remains unchanged after every failure.

Run:

```bash
bazel test --config=release //src/backend/definition:definition_resolver_test
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definition/definition_resolver.* \
  src/backend/definition/definition_resolver_test.cpp src/backend/definition/BUILD.bazel
git commit -m "feat: resolve definition inheritance safely"
```

---

### Task 7: Add catalog discovery, ROM matching, and resolved loading

**Files:**
- Create: `src/backend/definition/definition_service.h`
- Create: `src/backend/definition/definition_service.cpp`
- Create: `src/backend/definition/definition_service_test.cpp`
- Modify: `src/backend/definition/BUILD.bazel`

**Interfaces:**
- Produces:

```cpp
class DefinitionService {
  public:
    DefinitionService(IFileSystem&, IFileRepository&, IAtomicFileWriter&);
    Result<DefinitionCatalog> build_romraider_catalog(
        std::span<const std::string> ordered_handles);
    Result<DefinitionCatalog> build_ecuflash_catalog(std::string_view directory);
    Result<DefinitionIndexEntry> match_rom(
        const DefinitionCatalog&, std::span<const std::uint8_t> rom) const;
    Result<RomDefinition> load(
        const DefinitionCatalog&, DefinitionFormat, std::string_view id);
};
```

- [ ] **Step 1: Write in-memory fake ports and failing discovery tests**

The filesystem fake returns nested `DirEntry` values; the repository fake maps
handles to bytes and records read counts. Test recursive `.xml` discovery,
case-insensitive suffixes, skipped directories/non-XML files, lexical full-path
ordering, RomRaider configured ordering, and one-read-per-file catalog builds.

- [ ] **Step 2: Verify failure**

Run:

```bash
bazel test --config=release //src/backend/definition:definition_service_test
```

Expected: service target is missing.

- [ ] **Step 3: Implement catalog construction and error propagation**

Join directory paths without platform APIs, recurse through `IFileSystem`, sort
full handles, read through `IFileRepository`, dispatch to the correct index
parser, then call `DefinitionCatalog::create`. Any list/read/parse failure
returns without publishing a catalog.

- [ ] **Step 4: Write and implement bounds-safe ROM matching**

Tests must include exact-end match, one-byte-short ROM, ASCII ID, upper/lower
hex text, invalid odd-length hex, invalid address, ambiguous first match, empty
ID, and no match.

Implement comparisons over half-open spans; never index before validating
`address <= rom.size()` and `candidate_size <= rom.size() - address`.

- [ ] **Step 5: Write and implement resolved loading**

Test child/base files, memoized repeated parents, repository read failure,
missing ID, and parse/resolution failure. `load` creates a per-call loader that
uses catalog entries and dispatches to the appropriate format parser.

Run:

```bash
bazel test --config=release //src/backend/definition:definition_service_test
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add src/backend/definition/definition_service.* \
  src/backend/definition/definition_service_test.cpp src/backend/definition/BUILD.bazel
git commit -m "feat: add portable definition service"
```

---

### Task 8: Create and import definitions through atomic replacement

**Files:**
- Create: `src/backend/definition/definition_writer.h`
- Create: `src/backend/definition/definition_writer.cpp`
- Create: `src/backend/definition/definition_writer_test.cpp`
- Modify: `src/backend/definition/definition_service.h`
- Modify: `src/backend/definition/definition_service.cpp`
- Modify: `src/backend/definition/BUILD.bazel`

**Interfaces:**
- Produces:

```cpp
struct DefinitionHeaderInput {
    std::string xml_id, internal_id, ecu_id;
    std::uint64_t internal_id_address;
    RomMetadata metadata;
    std::string include, notes;
};

Result<std::vector<std::uint8_t>> create_ecuflash_xml(
    const DefinitionHeaderInput&);
Result<std::vector<std::uint8_t>> rewrite_ecuflash_xml(
    std::span<const std::uint8_t> source, const DefinitionHeaderInput&);

Status DefinitionService::create_definition(
    std::string_view destination, const DefinitionHeaderInput&);
Status DefinitionService::import_definition(
    std::string_view source, std::string_view destination,
    const DefinitionHeaderInput&);
```

- [ ] **Step 1: Write failing semantic creation tests**

Create XML, parse it with `parse_ecuflash_definition`, and compare identity and
metadata. Assert XML declaration, UTF-8 bytes, two-space indentation, and final
newline without asserting attribute order.

- [ ] **Step 2: Write failing structural import tests**

Use a source containing comments, scalings, tables, unknown elements, and
existing header fields. Rewrite the intended `romid`, `include`, and `notes`;
reparse and assert unrelated nodes/comments remain.

- [ ] **Step 3: Verify failure**

Run:

```bash
bazel test --config=release //src/backend/definition:definition_writer_test
```

Expected: writer symbols are missing.

- [ ] **Step 4: Implement tree transformation and validation**

Build/modify a `pugi::xml_document`, serialize with two-space indentation, add
a terminal newline, then call `parse_ecuflash_definition` on the serialized
bytes before returning them. Reject empty required IDs and invalid address
conversion as `InvalidConfig`.

- [ ] **Step 5: Test and implement service atomicity**

Use a recording `IAtomicFileWriter` fake. Assert it is not called when input,
source reading, transformation, or validation fails; assert exact destination
and bytes on success; propagate replacement failure as `Io`.

Run:

```bash
bazel test --config=release //src/backend/definition:definition_writer_test \
  //src/backend/definition:definition_service_test
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add src/backend/definition
git commit -m "feat: create and import definitions atomically"
```

---

### Task 9: Map typed results to legacy state in one commit

**Files:**
- Create: `src/backend/definition/legacy_definition_adapter.h`
- Create: `src/backend/definition/legacy_definition_adapter.cpp`
- Create: `tests/test_legacy_definition_adapter.cpp`
- Modify: `src/backend/definition/BUILD.bazel`
- Modify: `tests/BUILD.bazel`

**Interfaces:**
- Produces:

```cpp
class LegacyDefinitionAdapter {
  public:
    explicit LegacyDefinitionAdapter(DefinitionService&);
    Status replace_romraider_catalog(
        definitions::ConfigValuesStructure&, std::span<const std::string>);
    Status replace_ecuflash_catalog(
        definitions::ConfigValuesStructure&, std::string_view directory);
    Status replace_definition(
        definitions::EcuCalDefStructure&, const DefinitionCatalog&,
        DefinitionFormat, std::string_view id);
    Status create_definition(std::string_view destination,
                             const DefinitionHeaderInput&);
    Status import_definition(std::string_view source,
                             std::string_view destination,
                             const DefinitionHeaderInput&);
};
```

- [ ] **Step 1: Write failing catalog mapping tests**

Seed all four legacy catalog lists with sentinel values. On success assert
calibration ID, address text, ECU ID, and filename lists match and have equal
lengths. Inject service failure and assert the full original config value is
unchanged.

- [ ] **Step 2: Write failing full-definition mapping tests**

Construct a `RomDefinition` containing identity, all metadata, a 1D map, a 2D
map, axes, scaling, and selections. Assert every corresponding legacy list,
`RomInfo` slot, boolean, and provenance field. Add a helper assertion that all
map-row lists have `NameList.size()` and all scaling-row lists have
`ScalingNameList.size()`.

- [ ] **Step 3: Verify failure**

Run:

```bash
bazel test --config=release //tests:test_legacy_definition_adapter
```

Expected: adapter target is missing.

- [ ] **Step 4: Implement conversion through temporary legacy values**

Build `ConfigValuesStructure next = current` or
`EcuCalDefStructure next = current`, clear only the slice being replaced,
populate every aligned list, validate alignment, then assign
`current = std::move(next)`. Do not expose intermediate mutation.

- [ ] **Step 5: Run adapter and portable tests**

Run:

```bash
bazel test --config=release //tests:test_legacy_definition_adapter \
  //src/backend/definition/...
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add src/backend/definition/legacy_definition_adapter.* \
  src/backend/definition/BUILD.bazel tests/test_legacy_definition_adapter.cpp tests/BUILD.bazel
git commit -m "feat: adapt typed definitions to legacy state"
```

---

### Task 10: Wire catalog and parser wrappers into `FileActions`

**Files:**
- Modify: `src/backend/definitions/file_actions.h`
- Modify: `src/backend/definitions/file_actions.cpp`
- Modify: `src/backend/definitions/file_defs_romraider.cpp`
- Modify: `src/backend/definitions/file_defs_ecuflash.cpp`
- Modify: `src/backend/definitions/BUILD.bazel`
- Modify: `src/ui/desktop/mainwindow.cpp`
- Modify: `src/ui/desktop/settings.cpp`
- Modify: `src/ui/desktop/settings.h`
- Modify: `tests/test_file_actions_parsing.cpp`
- Modify: `tests/test_ecuflash_definition_parsing.cpp`
- Modify: `tests/test_rom_transformations.cpp`

**Interfaces:**
- Consumes: `DefinitionService`, `LegacyDefinitionAdapter`, existing
  `IFileSystem`, `IFileRepository`, and new `IAtomicFileWriter`.
- Produces: unchanged public `FileActions` method signatures.

- [ ] **Step 1: Extend `FileActions` construction without changing callers**

Prefer constructing `QtAtomicFileWriter` at the existing desktop composition
root and passing `IAtomicFileWriter&` beside the current three ports. If all
current `FileActions` call sites cannot be updated in one focused change,
provide one delegating desktop-only constructor and keep dependency ownership
explicit; do not create file ports inside portable code.

Update test fixtures with an in-memory atomic writer.

- [ ] **Step 2: Write wrapper atomicity characterizations**

For each catalog/read wrapper, seed legacy state, invoke a malformed/missing
definition, and assert the wrapper returns its historical pointer/null
convention while leaving state unchanged. On success, compare valid fixtures
with the existing expected legacy fields and logs.

Run:

```bash
bazel test --config=release //tests:test_file_actions_parsing \
  //tests:test_ecuflash_definition_parsing
```

Expected: new strict/atomic expectations fail against legacy implementations.

- [ ] **Step 3: Delegate catalog and read methods**

Replace bodies of:

```text
create_romraider_def_id_list
create_ecuflash_def_id_list
read_romraider_ecu_base_def
read_romraider_ecu_def
read_ecuflash_ecu_def
parse_ecuflash_def_scalings
parse_ecuid_romraider_def_files
parse_ecuid_ecuflash_def_files
```

with service/adapter calls. Translate `Error` to existing `LOG_*` signals and
return conventions in `FileActions`; do not show messages inside the adapter.
Keep any compatibility-only helper private to the Qt target.

- [ ] **Step 4: Run focused wrapper tests**

Run:

```bash
bazel test --config=release //tests:test_file_actions_parsing \
  //tests:test_ecuflash_definition_parsing //tests:test_model_validation
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definitions src/backend/definition/BUILD.bazel \
  src/ui/desktop/mainwindow.cpp tests/test_file_actions_parsing.cpp \
  tests/test_ecuflash_definition_parsing.cpp
git commit -m "refactor: delegate definition parsing from FileActions"
```

---

### Task 11: Move creation/import XML work behind the portable boundary

**Files:**
- Modify: `src/backend/definitions/file_actions.cpp`
- Modify: `src/backend/definitions/file_actions.h`
- Modify: `tests/test_file_actions_parsing.cpp`
- Modify: `tests/BUILD.bazel`

**Interfaces:**
- Consumes: UI-collected `DefinitionHeaderInput`, selected source/destination
  handles, and adapter creation/import methods.
- Produces: existing `create_new_definition_for_rom` and
  `use_existing_definition_for_rom` signatures with Qt-only UI orchestration.

- [ ] **Step 1: Characterize cancellation and successful submission**

Extract the non-visual submission branch into private methods callable by
tests:

```cpp
fastecu::Status submit_new_definition(
    std::string_view destination,
    const fastecu::definition::DefinitionHeaderInput&);
fastecu::Status submit_imported_definition(
    std::string_view source, std::string_view destination,
    const fastecu::definition::DefinitionHeaderInput&);
```

Test that cancellation never calls these methods, successful submission calls
the atomic writer once, and a backend error is logged/translated without
changing config catalog lists.

- [ ] **Step 2: Run and verify tests fail**

Run:

```bash
bazel test --config=release //tests:test_file_actions_parsing
```

Expected: submission seams do not exist and legacy code writes with `QFile`.

- [ ] **Step 3: Keep only UI policy in the public methods**

Retain dialog widgets, field collection, file pickers, retry/cancel loops, and
message translation. Remove `QXmlStreamWriter`, raw line extraction/copying,
and direct definition `QFile` writes. Convert accepted field values to
`DefinitionHeaderInput` and delegate to the private submission methods.

- [ ] **Step 4: Run focused UI-boundary tests**

Run:

```bash
bazel test --config=release //tests:test_file_actions_parsing \
  //src/backend/definition:definition_writer_test
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definitions/file_actions.cpp src/backend/definitions/file_actions.h \
  tests/test_file_actions_parsing.cpp tests/BUILD.bazel
git commit -m "refactor: move definition writes behind portable service"
```

---

### Task 12: Delete legacy parsers and enforce portability

**Files:**
- Delete: `src/backend/definitions/file_defs_romraider.cpp`
- Delete: `src/backend/definitions/file_defs_ecuflash.cpp`
- Modify: `src/backend/definitions/BUILD.bazel`
- Modify: `src/backend/definition/BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`
- Modify: `BUILD.bazel`
- Modify: serial compatibility allowlist source identified by
  `bazel query //:serial_compat_allowlist`
- Modify: `bazel/test_sources.bzl` only if deleted test/source labels are listed

**Interfaces:**
- Produces portable-closure required targets:
  `definition_model`, `romraider_parser`, `ecuflash_parser`,
  `definition_resolver`, `definition_service`, and `definition_writer`.

- [ ] **Step 1: Prove no legacy implementation symbol is needed**

Run:

```bash
rg -n 'QDomDocument|QXmlStreamReader|QXmlStreamWriter|QDirIterator|QFileInfo|QFile ' \
  src/backend/definitions/file_actions.cpp \
  src/backend/definitions/file_defs_romraider.cpp \
  src/backend/definitions/file_defs_ecuflash.cpp
rg -n 'FileActions::(convert_value_format|parse_strict_bool_attribute|add_.*_def_list_item)' \
  src tests
```

Expected: definition parsing/writing hits exist only in files/methods now
superseded. Preserve unrelated menu/logger/calibration Qt use for later slices.

- [ ] **Step 2: Delete obsolete sources and helpers**

Delete both `file_defs_*.cpp` files. Remove declarations used only by those
implementations after confirming no external call site. Keep compatibility
wrappers required by current callers.

- [ ] **Step 3: Extend portable-closure checks**

Add:

```python
ROOT / "src/backend/definition": {
    "definition_model",
    "romraider_parser",
    "ecuflash_parser",
    "definition_resolver",
    "definition_service",
    "definition_writer",
},
```

to `PORTABLE_ROOTS`. Add the new BUILD file to the root target's data/genquery
wiring following existing backend packages. Query the live serial allowlist and
remove only entries whose dependency disappeared; do not use a predicted
count.

- [ ] **Step 4: Run focused portability and source-reference gates**

Run:

```bash
bazel test --config=release //:portable_closure //:serial_compat_allowlist
bazel query 'deps(//src/backend/definition:definition_service)' | rg 'qt|platform'
rg -n 'file_defs_(romraider|ecuflash)\\.cpp' BUILD.bazel bazel src tests
```

Expected: both tests pass; the query grep and deleted-source grep produce no
matches.

- [ ] **Step 5: Commit**

```bash
git add src/backend/definitions src/backend/definition scripts/check-portable-closure.py \
  BUILD.bazel bazel
git commit -m "build: enforce portable definition boundary"
```

---

### Task 13: Run coverage, full gates, and final cleanup

**Files:**
- Modify: tests or production files implicated by failures only.
- Do not create: `docs/coverage-baseline.txt`.

**Interfaces:**
- Consumes: the complete 5d-3 implementation.
- Produces: verified release build, test suite, portability closure, and
  coverage evidence.

- [ ] **Step 1: Run every focused 5d-3 test**

Run:

```bash
bazel test -k --config=release \
  //src/backend/definition/... \
  //tests:test_qt_atomic_file_writer \
  //tests:test_legacy_definition_adapter \
  //tests:test_file_actions_parsing \
  //tests:test_ecuflash_definition_parsing \
  //tests:test_model_validation
```

Expected: all pass.

- [ ] **Step 2: Run the release build and complete test gates**

Run:

```bash
bazel build -k --config=release //:fastecu //tests/...
bazel test -k --config=release //tests/... //:bazel_openssl_wiring \
  //:serial_compat_allowlist //:portable_closure
```

Expected: all requested targets pass. Investigate every failure; do not dismiss
failures as unrelated without reproducing against the pre-task commit.

- [ ] **Step 3: Run formatting/lint gates**

Run:

```bash
prek run --all-files
git diff --check
```

Expected: pass and no whitespace errors.

- [ ] **Step 4: Check coverage and forbidden regressions**

Run the repository's current Sonar/coverage command used by CI, then verify:

```bash
test ! -e docs/coverage-baseline.txt
rg -n 'QDom|QXml|QFile|QDirIterator|QString|QByteArray' src/backend/definition \
  --glob '!legacy_definition_adapter.*'
```

Expected: new code coverage is >=80%; the baseline file is absent; portable
production files contain no Qt identifiers.

- [ ] **Step 5: Inspect the final diff and commit any gate-only corrections**

Run:

```bash
git status --short
git diff --stat HEAD~12
git diff --check
```

Confirm the unrelated untracked files remain untouched. If verification
required corrections:

```bash
git add src/backend/definition src/backend/definitions \
  src/backend/ports src/platform/desktop/common/ports tests \
  scripts/check-portable-closure.py BUILD.bazel bazel
git commit -m "test: complete definition use case verification"
```

If no corrections were needed, do not create an empty commit.
