# Step 6m-1: Portable Calibration Session Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a portable `CalibrationSession`, `RomOpenUseCase`, and `CalibrationWorkspace` that reproduce the legacy ROM-open behavior on typed values. Wire them into `DesktopComposition` without migrating any consumer.

**Architecture:**
- A new portable package, `//src/backend/calibration/session`, holds the session value type, a narrow `IDefinitionCatalogs` port, the open use case, and the workspace that owns sessions by stable `SessionId`.
- The use case is `FileActions::open_subaru_rom_file` re-expressed over `DefinitionService`, `ConfigSession`, and the existing calibration functions.
- `FileActions` implements `IDefinitionCatalogs` with its existing `build_definition_catalog`, so the catalog sources stay exactly as they are today.
- Nothing in the UI changes in this slice.

**Tech Stack:** C++23 (`std::expected`, `std::format`), Bazel (version in `.bazelversion`), GoogleTest/GoogleMock (`fastecu_portable_gtest`), Qt 6 + QtTest (`fastecu_qttest`) for the composition test only, prek, `gh stack`.

**Spec:** [Step 6m — Definition and calibration sessions](../specs/2026-09-28-step6m-calibration-sessions-design.md)

**Scope of this plan:** PR 6m-1 only. 6m-2 and 6m-3 get their own plans, written after 6m-1 merges, because their code is written against this slice's real interfaces and against `MainWindow` as it stands then.

## Global Constraints

- The new package is portable: no Qt, threads, or direct filesystem access. Every new `cc_library` is registered by name under `"src/backend/calibration/session"` in `bazel/portable_targets.bzl`.
- Backend results are checked with `.has_value()`, never `operator bool`. `ErrorKind` is closed; use existing values only (`InvalidConfig`, `Internal`).
- Decoded cell text uses float precision `15`. The legacy `"v1,v2,...,"` format (trailing comma after every value) and the `" "` default for an absent axis are preserved exactly.
- Log lines keep the legacy format `"<operation> [<ErrorKind>]: <detail>"`. Operator notice text is copied verbatim from the legacy code, as quoted in each task.
- Flash-method padding runs **after** the file-size label is taken from the unpadded length and **before** ROM-size validation.
- The `qt_layer` ratchet only shrinks. This slice adds no entry.
- No wire behavior, definition XML schema, ROM file format, or hardware support changes. No bench checklist changes.
- Tests use synthetic ROM bytes only; never real ROM content.
- Markdown cross-references are links with readable text, not backticked paths.
- The work lands as a `gh stack`:
  - `feat/6m-calibration-sessions` (6m-0: spec and this plan, already committed)
  - → `refactor/step6m-1-calibration-session` (Tasks 1–6)
- Every PR passes `bazel build -k --config=release //...`, `bazel test -k --config=release //...`, `prek run --all-files`, and `bazel run --config=release //:clang_tidy_report_changed`. Windows/macOS/Linux CI is a required gate. Report any CI you cannot run locally as pending, not passing.
- Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

## Review Focus

- **A ROM whose definition file was deleted after the catalog was built.** Expected: the ROM still opens without a definition, and the operator sees `Ecu definitions file: Unable to open ECU definition file <source> for reading`. Pinned by `RomOpenDefinitions.MissingDefinitionFileOpensWithoutDefinitionAndNotifies` (Task 4).
- **An ECU read whose reported ROM ID is not recognized by `match_rom`, but is a definition ID in the catalog.** Expected: legacy falls back to loading that ID, so the definition is found. Pinned by `RomOpenDefinitions.EcuReportedIdIsUsedWhenMatchingFails` (Task 4).
- **A definition that addresses beyond the end of the ROM.** Expected: the ROM opens, the definition header is kept, every map is dropped, and the size notice is shown. Pinned by `RomOpenDefinitions.SizeRejectionKeepsHeaderAndDropsMaps` (Task 4).
- **A padded-family ROM (`sub_ecu_denso_mc68hc16y5_02`).** Expected: the file-size label shows the unpadded size, while the session's bytes are padded. Pinned by `RomOpenBasics.PaddingFollowsTheUnpaddedSizeLabel` (Task 3).
- **A stale `SessionId` after close.** Expected: `find` returns `nullptr`, `close` fails, and IDs are never reused. Pinned by `CalibrationWorkspaceTest.ClosedIdsAreNeverReused` (Task 5).

---

## File Structure

| File | Change | Task |
|---|---|---|
| `src/backend/calibration/calibration_service.{h,cpp}`, `calibration_service_test.cpp` | Expose `compute_one_map_cell_values` | 1 |
| `src/backend/calibration/session/BUILD.bazel` | Create | 2 (grown in 3, 5) |
| `src/backend/calibration/session/calibration_session.{h,cpp}`, `calibration_session_test.cpp` | Create | 2 |
| `bazel/portable_targets.bzl` | Register session targets | 2, 3, 5 |
| `src/backend/calibration/session/definition_catalogs.h` | Create (`IDefinitionCatalogs`) | 3 |
| `src/backend/calibration/session/testing/{BUILD.bazel,fake_definition_catalogs.h}` | Create (testonly fake) | 3 |
| `src/backend/calibration/session/rom_open.{h,cpp}`, `rom_open_test.cpp` | Create | 3, 4 |
| `src/backend/calibration/session/calibration_workspace.{h,cpp}`, `calibration_workspace_test.cpp` | Create | 5 |
| `src/backend/definitions/file_actions.h`, `BUILD.bazel` | Implement `IDefinitionCatalogs` | 6 |
| `apps/desktop/desktop_composition.{h,cpp}`, `BUILD.bazel`, `desktop_composition_test.cpp` | Own the workspace | 6 |
| Spec | Record the interface decisions made while planning | 0 (done with this plan) |

---

## PR 6m-1 — Portable session

### Task 1: Single-map decode entry point

**Files:**
- Modify: `src/backend/calibration/calibration_service.h`, `src/backend/calibration/calibration_service.cpp:88-160,336-359`
- Test: `src/backend/calibration/calibration_service_test.cpp`

**Interfaces:**
- Produces: `Result<MapCellValues> fastecu::calibration::compute_one_map_cell_values(const definition::RomDefinition&, const definition::CalibrationMap&, bytes::ByteView rom_data, int float_precision)`. This is today's anonymous-namespace `compute_one_map`, made public and renamed. `compute_map_cell_values` calls it unchanged.

- [ ] **Step 1: Create the 6m-1 branch**

```bash
git switch feat/6m-calibration-sessions
git switch -c refactor/step6m-1-calibration-session
```

- [ ] **Step 2: Write the failing tests**

Append after the existing `ComputeMapCellValues` tests in `src/backend/calibration/calibration_service_test.cpp`. They reuse the file's `one_map_definition` helper.

```cpp
TEST(ComputeOneMapCellValues, MatchesTheWholeDefinitionEntry)
{
    definition::RomDefinition rom = one_map_definition(0);
    definition::CalibrationMap second = rom.maps.at(0);
    second.name = "Second";
    second.address = 1;
    rom.maps.push_back(second);
    const std::vector<std::uint8_t> data{5, 6, 7, 8};

    const auto whole = compute_map_cell_values(rom, data, 15);
    ASSERT_THAT(whole, fastecu::testing::IsOk());

    for (std::size_t index = 0; index < rom.maps.size(); ++index)
    {
        const auto one = compute_one_map_cell_values(rom, rom.maps.at(index), data, 15);
        ASSERT_THAT(one, fastecu::testing::IsOk());
        EXPECT_EQ(one->map_data, whole->at(index).map_data) << index;
        EXPECT_EQ(one->x_axis_data, whole->at(index).x_axis_data) << index;
        EXPECT_EQ(one->y_axis_data, whole->at(index).y_axis_data) << index;
    }
    EXPECT_EQ(whole->at(1).map_data, "6,7,8,");
}

TEST(ComputeOneMapCellValues, ReportsTheErrorTheWholeDefinitionStores)
{
    definition::RomDefinition rom = one_map_definition(0xF0000000);
    const std::vector<std::uint8_t> data{5, 6, 7};

    const auto one = compute_one_map_cell_values(rom, rom.maps.at(0), data, 15);
    const auto whole = compute_map_cell_values(rom, data, 15);

    ASSERT_THAT(one, fastecu::testing::IsErr(ErrorKind::Internal));
    ASSERT_THAT(whole, fastecu::testing::IsOk());
    ASSERT_TRUE(whole->at(0).error.has_value());
    EXPECT_EQ(one.error(), *whole->at(0).error);
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/calibration:calibration_service_test`
Expected: FAIL to compile with `use of undeclared identifier 'compute_one_map_cell_values'`.

- [ ] **Step 4: Expose the function**

In `calibration_service.h`, directly above `compute_map_cell_values`, add:

```cpp
// One entry of compute_map_cell_values: the same decode for a single map, for
// callers that decode on demand. A failure here is exactly the error the
// whole-definition function stores in that map's `error` field.
Result<MapCellValues> compute_one_map_cell_values(const definition::RomDefinition& rom_definition,
                                                  const definition::CalibrationMap& map, bytes::ByteView rom_data,
                                                  int float_precision);
```

In `calibration_service.cpp`:
1. Cut the whole `compute_one_map` definition (it starts at line 88, inside the anonymous namespace that closes at line 160).
2. Paste it after the anonymous namespace's closing `} // namespace`, renamed `compute_one_map_cell_values` with the signature above.
3. In `compute_map_cell_values`, change the call to:

```cpp
        auto computed = compute_one_map_cell_values(rom_definition, map, rom_data, float_precision);
```

Helpers used by the moved function stay in the anonymous namespace above it; no other change is needed.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/calibration:all`
Expected: PASS, including every pre-existing `ComputeMapCellValues` test unchanged.

- [ ] **Step 6: Commit**

```bash
git add src/backend/calibration/calibration_service.h src/backend/calibration/calibration_service.cpp src/backend/calibration/calibration_service_test.cpp
git commit -m "refactor(calibration): expose single-map cell decode (6m-1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: `CalibrationSession` value type

**Files:**
- Create: `src/backend/calibration/session/BUILD.bazel`
- Create: `src/backend/calibration/session/calibration_session.h`, `calibration_session.cpp`, `calibration_session_test.cpp`
- Modify: `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `compute_one_map_cell_values` (Task 1).
- Produces, in namespace `fastecu::calibration`:
  - `inline constexpr int kCellFloatPrecision = 15;`
  - `enum class SessionId : std::uint64_t {};`
  - `enum class RomOrigin { File, EcuRead };`
  - `struct RomSource { std::string display_name; std::string path; RomOrigin origin; }`
  - `struct ResolvedDefinition { definition::DefinitionFormat format; std::string id; definition::RomDefinition definition; }`
  - `struct RomProtocolInfo { std::string flash_method, checksum_module, mcu_type, kernel_path, kernel_start_address, rom_id, file_size_label; std::size_t unpadded_size; }`
  - `struct SessionContents { RomSource source; std::vector<std::uint8_t> rom; std::optional<ResolvedDefinition> definition; RomProtocolInfo protocol; }`
  - `class CalibrationSession`, with:
    - `CalibrationSession(SessionId, SessionContents)`
    - `id()`, `source()`, `rom() -> bytes::ByteView`, `definition() -> const ResolvedDefinition*`
    - `protocol()`, `set_protocol(RomProtocolInfo)`, `dirty()`
    - `decode_map(std::size_t) const -> Result<MapCellValues>`
    - `write_bytes(std::uint64_t offset, bytes::ByteView) -> Status`

- [ ] **Step 1: Write the failing tests**

Create `src/backend/calibration/session/calibration_session_test.cpp`:

```cpp
#include "src/backend/calibration/session/calibration_session.h"

#include <cstdint>
#include <limits>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{

using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

definition::RomDefinition fuel_definition()
{
    definition::RomDefinition rom{.format = definition::DefinitionFormat::EcuFlash};
    rom.scalings.push_back(definition::Scaling{.name = "Raw", .from_byte = "x"});
    definition::CalibrationMap map;
    map.name = "Fuel";
    map.type = "2D";
    map.address = 2;
    map.x_size = 3;
    map.storage_type = definition::StorageType::Uint8;
    map.endian = "big";
    map.scaling_name = "Raw";
    rom.maps.push_back(map);
    return rom;
}

SessionContents contents_with_definition()
{
    return SessionContents{
        .source = {.display_name = "a.bin", .path = "/cal/a.bin", .origin = RomOrigin::File},
        .rom = {0, 0, 5, 6, 7, 0},
        .definition = ResolvedDefinition{.format = definition::DefinitionFormat::EcuFlash,
                                         .id = "TEST",
                                         .definition = fuel_definition()},
        .protocol = {.flash_method = "proto_a"},
    };
}

TEST(CalibrationSessionTest, ExposesWhatItWasBuiltFrom)
{
    const CalibrationSession session(SessionId{7}, contents_with_definition());

    EXPECT_EQ(session.id(), SessionId{7});
    EXPECT_EQ(session.source().display_name, "a.bin");
    EXPECT_EQ(session.source().origin, RomOrigin::File);
    EXPECT_EQ(session.rom().size(), 6U);
    ASSERT_NE(session.definition(), nullptr);
    EXPECT_EQ(session.definition()->id, "TEST");
    EXPECT_EQ(session.protocol().flash_method, "proto_a");
    EXPECT_FALSE(session.dirty());
}

TEST(CalibrationSessionTest, DecodesAMapFromTheCurrentBytes)
{
    const CalibrationSession session(SessionId{1}, contents_with_definition());

    const auto values = session.decode_map(0);

    ASSERT_THAT(values, IsOk());
    EXPECT_EQ(values->map_data, "5,6,7,");
    EXPECT_EQ(values->x_axis_data, " ");
    EXPECT_EQ(values->y_axis_data, " ");
}

TEST(CalibrationSessionTest, DecodeMatchesTheWholeDefinitionDecode)
{
    const CalibrationSession session(SessionId{1}, contents_with_definition());

    const auto whole =
        compute_map_cell_values(session.definition()->definition, session.rom(), kCellFloatPrecision);
    const auto one = session.decode_map(0);

    ASSERT_THAT(whole, IsOk());
    ASSERT_THAT(one, IsOk());
    EXPECT_EQ(one->map_data, whole->at(0).map_data);
}

TEST(CalibrationSessionTest, DecodeRejectsAnOutOfRangeIndex)
{
    const CalibrationSession session(SessionId{1}, contents_with_definition());

    EXPECT_THAT(session.decode_map(1), IsErr(ErrorKind::InvalidConfig));
}

TEST(CalibrationSessionTest, DecodeWithoutADefinitionFails)
{
    SessionContents contents = contents_with_definition();
    contents.definition.reset();
    const CalibrationSession session(SessionId{1}, std::move(contents));

    EXPECT_EQ(session.definition(), nullptr);
    EXPECT_THAT(session.decode_map(0), IsErr(ErrorKind::InvalidConfig));
}

TEST(CalibrationSessionTest, WrittenBytesAreWhatTheNextDecodeSees)
{
    CalibrationSession session(SessionId{1}, contents_with_definition());
    const std::vector<std::uint8_t> patch{9, 8};

    ASSERT_THAT(session.write_bytes(3, patch), IsOk());

    EXPECT_TRUE(session.dirty());
    EXPECT_EQ(session.decode_map(0)->map_data, "5,9,8,");
}

TEST(CalibrationSessionTest, WriteReachingTheLastByteSucceeds)
{
    CalibrationSession session(SessionId{1}, contents_with_definition());
    const std::vector<std::uint8_t> patch{0xAA};

    ASSERT_THAT(session.write_bytes(5, patch), IsOk());
    EXPECT_EQ(session.rom()[5], 0xAA);
}

TEST(CalibrationSessionTest, WritePastTheEndChangesNothing)
{
    CalibrationSession session(SessionId{1}, contents_with_definition());
    const std::vector<std::uint8_t> before(session.rom().begin(), session.rom().end());
    const std::vector<std::uint8_t> patch{1, 2};

    EXPECT_THAT(session.write_bytes(5, patch), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(session.write_bytes(std::numeric_limits<std::uint64_t>::max(), patch),
                IsErr(ErrorKind::InvalidConfig));

    EXPECT_EQ(std::vector<std::uint8_t>(session.rom().begin(), session.rom().end()), before);
    EXPECT_FALSE(session.dirty());
}

TEST(CalibrationSessionTest, ProtocolInfoCanBeReplaced)
{
    CalibrationSession session(SessionId{1}, contents_with_definition());

    session.set_protocol(RomProtocolInfo{.flash_method = "proto_b", .kernel_path = "/k/b.bin"});

    EXPECT_EQ(session.protocol().flash_method, "proto_b");
    EXPECT_EQ(session.protocol().kernel_path, "/k/b.bin");
    EXPECT_FALSE(session.dirty());
}

} // namespace
} // namespace fastecu::calibration
```

Create `src/backend/calibration/session/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")

# Portable calibration sessions: typed ROM + definition ownership that
# replaces the legacy EcuCalDefStructure (step 6m).
package(default_visibility = [
    "//apps/desktop:__pkg__",
    "//src/backend:__subpackages__",
    "//src/platform:__subpackages__",
    "//src/ui:__subpackages__",
])

cc_library(
    name = "calibration_session",
    srcs = ["calibration_session.cpp"],
    hdrs = ["calibration_session.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/backend/calibration:calibration_service",
        "//src/backend/definition:definition_model",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "calibration_session_test",
    srcs = ["calibration_session_test.cpp"],
    deps = [
        ":calibration_session",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/calibration/session:calibration_session_test`
Expected: FAIL, because `calibration_session.h` does not exist.

- [ ] **Step 3: Write the header**

Create `src/backend/calibration/session/calibration_session.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/calibration/calibration_service.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// The precision legacy formatted every decoded cell with
// (FileActions::float_precision).
inline constexpr int kCellFloatPrecision = 15;

// Identifies one open calibration within a workspace. Never reused, so a UI
// element still holding the ID of a closed session finds nothing rather than
// a different ROM.
enum class SessionId : std::uint64_t
{
};

enum class RomOrigin
{
    File,
    EcuRead,
};

struct RomSource
{
    // Basename of `path`; "default.bin" when the path has none.
    std::string display_name;
    std::string path;
    RomOrigin origin{RomOrigin::File};

    bool operator==(const RomSource&) const = default;
};

struct ResolvedDefinition
{
    definition::DefinitionFormat format{definition::DefinitionFormat::EcuFlash};
    std::string id;
    definition::RomDefinition definition;

    bool operator==(const ResolvedDefinition&) const = default;
};

// What the flash, checksum and ROM-info views need that is not part of the
// definition. Replaces the legacy FlashMethod/Kernel/KernelStartAddr/McuType/
// RomId fields and the protocol-derived RomInfo slots.
struct RomProtocolInfo
{
    std::string flash_method; // after alias resolution
    std::string checksum_module;
    std::string mcu_type;
    std::string kernel_path;
    std::string kernel_start_address;
    std::string rom_id;
    std::string file_size_label; // "<unpadded bytes / 1024>kb"
    std::size_t unpadded_size{0};

    bool operator==(const RomProtocolInfo&) const = default;
};

struct SessionContents
{
    RomSource source;
    std::vector<std::uint8_t> rom;
    // nullopt: opened without a definition ("continue without definition
    // file"), a modeled state rather than placeholder rows.
    std::optional<ResolvedDefinition> definition;
    RomProtocolInfo protocol;
};

// One open ROM. Its bytes are the only truth for map values: there is no
// decoded-value cache, so a view decodes on demand and an edit writes bytes.
class CalibrationSession
{
  public:
    CalibrationSession(SessionId id, SessionContents contents);

    SessionId id() const;
    const RomSource& source() const;
    bytes::ByteView rom() const;
    const ResolvedDefinition *definition() const;
    const RomProtocolInfo& protocol() const;
    void set_protocol(RomProtocolInfo protocol);
    // True once any write_bytes succeeded.
    bool dirty() const;

    // Cells and axes of definition()->definition.maps[map_index], decoded from
    // the current bytes. InvalidConfig for an index past the last map or a
    // session without a definition; otherwise the decode's own result.
    Result<MapCellValues> decode_map(std::size_t map_index) const;

    // The only mutation of the bytes. The whole of `data` must land inside the
    // image; a write that would not is rejected and changes nothing.
    Status write_bytes(std::uint64_t offset, bytes::ByteView data);

  private:
    SessionId id_;
    SessionContents contents_;
    bool dirty_{false};
};

} // namespace fastecu::calibration
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/calibration/session/calibration_session.cpp`:

```cpp
#include "src/backend/calibration/session/calibration_session.h"

#include <algorithm>
#include <format>
#include <utility>

namespace fastecu::calibration
{

CalibrationSession::CalibrationSession(SessionId id, SessionContents contents)
    : id_(id), contents_(std::move(contents))
{
}

SessionId CalibrationSession::id() const
{
    return id_;
}

const RomSource& CalibrationSession::source() const
{
    return contents_.source;
}

bytes::ByteView CalibrationSession::rom() const
{
    return contents_.rom;
}

const ResolvedDefinition *CalibrationSession::definition() const
{
    return contents_.definition.has_value() ? &*contents_.definition : nullptr;
}

const RomProtocolInfo& CalibrationSession::protocol() const
{
    return contents_.protocol;
}

void CalibrationSession::set_protocol(RomProtocolInfo protocol)
{
    contents_.protocol = std::move(protocol);
}

bool CalibrationSession::dirty() const
{
    return dirty_;
}

Result<MapCellValues> CalibrationSession::decode_map(std::size_t map_index) const
{
    if (!contents_.definition.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "calibration session has no definition");
    }
    const definition::RomDefinition& rom_definition = contents_.definition->definition;
    if (map_index >= rom_definition.maps.size())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("map index {} is past the definition's {} maps", map_index,
                                rom_definition.maps.size()));
    }
    return compute_one_map_cell_values(rom_definition, rom_definition.maps[map_index], contents_.rom,
                                       kCellFloatPrecision);
}

Status CalibrationSession::write_bytes(std::uint64_t offset, bytes::ByteView data)
{
    const std::uint64_t size = contents_.rom.size();
    // Written as two comparisons so a huge offset cannot wrap the sum.
    if (offset > size || data.size() > size - offset)
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("write of {} bytes at 0x{:x} is outside the {}-byte image", data.size(), offset,
                                size));
    }
    std::ranges::copy(data, contents_.rom.begin() + static_cast<std::ptrdiff_t>(offset));
    dirty_ = true;
    return {};
}

} // namespace fastecu::calibration
```

In `bazel/portable_targets.bzl`, add a new entry after `"src/backend/calibration"`:

```python
    "src/backend/calibration/session": [
        "calibration_session",
    ],
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/calibration/session:all && bazel build --config=release //:portable_closure`
Expected: PASS; `portable_closure` builds.

- [ ] **Step 6: Commit**

```bash
git add src/backend/calibration/session bazel/portable_targets.bzl
git commit -m "feat(calibration): add portable calibration session (6m-1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: `RomOpenUseCase` — reading, adoption, protocol info, and padding

This task builds the open sequence with definitions disabled. Task 4 adds definition matching into the same function.

**Files:**
- Create: `src/backend/calibration/session/definition_catalogs.h`
- Create: `src/backend/calibration/session/testing/BUILD.bazel`, `src/backend/calibration/session/testing/fake_definition_catalogs.h`
- Create: `src/backend/calibration/session/rom_open.h`, `rom_open.cpp`, `rom_open_test.cpp`
- Modify: `src/backend/calibration/session/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `CalibrationSession` types (Task 2); `read_rom`, `backup_rom`, and `apply_flash_method_padding` from `calibration_service.h`; `config::ConfigSession` (`settings()`, `effective_paths()`, `vehicles()`, `selected_vehicle()`, `select_by_protocol_name()`); `config::protocol_field_or_placeholder`.
- Produces:
  - `class IDefinitionCatalogs { virtual Result<definition::DefinitionCatalog> catalog(definition::DefinitionFormat) = 0; }` in `definition_catalogs.h`.
  - `struct ReadImage { std::vector<std::uint8_t> rom; std::string filename, rom_id, protocol_name, kernel_path, kernel_start_address; }`.
  - `struct RomOpenOutcome { SessionContents contents; bool vehicle_selected; bool size_rejected; }`.
  - `class RomOpenUseCase`, with:
    - `RomOpenUseCase(IDefinitionCatalogs&, definition::DefinitionService&, IFileRepository&, IFileSystem&, IEventSink&, config::ConfigSession&)`
    - `Result<RomOpenOutcome> open_file(std::string_view path)`
    - `Result<RomOpenOutcome> adopt_read_image(ReadImage)`
  - Testonly `fastecu::calibration::testing::FakeDefinitionCatalogs` with public `entries`, `errors`, and `calls`.

- [ ] **Step 1: Write the port and the fake**

Create `src/backend/calibration/session/definition_catalogs.h`:

```cpp
#pragma once

#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Where a ROM open finds definitions to match and load. The desktop's
// implementation is FileActions, which builds catalogs from the configured
// sources and the definitions authored this session; step 6n replaces it.
class IDefinitionCatalogs
{
  public:
    virtual ~IDefinitionCatalogs() = default;
    // Called at most once per format per open.
    virtual Result<definition::DefinitionCatalog> catalog(definition::DefinitionFormat format) = 0;
};

} // namespace fastecu::calibration
```

Create `src/backend/calibration/session/testing/fake_definition_catalogs.h`:

```cpp
#pragma once

#include <map>
#include <utility>
#include <vector>

#include "src/backend/calibration/session/definition_catalogs.h"

namespace fastecu::calibration::testing
{

class FakeDefinitionCatalogs : public IDefinitionCatalogs
{
  public:
    Result<definition::DefinitionCatalog> catalog(definition::DefinitionFormat format) override
    {
        calls.push_back(format);
        if (auto error = errors.find(format); error != errors.end())
        {
            return std::unexpected(error->second);
        }
        auto found = entries.find(format);
        return definition::DefinitionCatalog::create(
            found == entries.end() ? std::vector<definition::DefinitionIndexEntry>{} : found->second);
    }

    std::map<definition::DefinitionFormat, std::vector<definition::DefinitionIndexEntry>> entries;
    std::map<definition::DefinitionFormat, Error> errors;
    std::vector<definition::DefinitionFormat> calls;
};

} // namespace fastecu::calibration::testing
```

Create `src/backend/calibration/session/testing/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")

# Session-package-owned test doubles.
package(default_visibility = [
    "//apps/desktop:__pkg__",
    "//src/backend:__subpackages__",
    "//src/ui:__subpackages__",
])

cc_library(
    name = "fake_definition_catalogs",
    testonly = True,
    hdrs = ["fake_definition_catalogs.h"],
    deps = ["//src/backend/calibration/session:definition_catalogs"],
)
```

- [ ] **Step 2: Write the failing tests**

Create `src/backend/calibration/session/rom_open_test.cpp`. The fixture is shared with Task 4, which appends to this file.

```cpp
#include "src/backend/calibration/session/rom_open.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/testing/fake_definition_catalogs.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using definition::DefinitionFormat;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

// A synthetic 64-byte image: "TESTROM" at 0x10, 0x2A at 0x20.
std::vector<std::uint8_t> synthetic_rom()
{
    std::vector<std::uint8_t> rom(64, 0);
    const std::string id = "TESTROM";
    std::ranges::copy(id, rom.begin() + 0x10);
    rom[0x20] = 0x2A;
    return rom;
}

class RomOpenTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg.initialize(), IsOk());
    }

    void put_rom(const std::string& path, std::vector<std::uint8_t> rom)
    {
        cfg.file_repository.files[path] = std::move(rom);
    }

    std::vector<std::string> notices() const
    {
        return cfg.events.notices;
    }

    std::vector<std::string> log_text() const
    {
        std::vector<std::string> text;
        for (const auto& [level, message] : cfg.events.logs)
        {
            text.push_back(message);
        }
        return text;
    }

    config::testing::ConfigSessionFixture cfg;
    InMemoryAtomicFileWriter writer;
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    testing::FakeDefinitionCatalogs catalogs;
    RomOpenUseCase opener{catalogs,        definitions, cfg.file_repository, cfg.file_system,
                          cfg.events,   cfg.session};
};

class RomOpenBasics : public RomOpenTest
{
};

TEST_F(RomOpenBasics, OpensAFileWithoutDefinitionsWhenBothFormatsAreDisabled)
{
    put_rom("/cal/dir/a.bin", synthetic_rom());

    const auto outcome = opener.open_file("/cal/dir/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.source, (RomSource{"a.bin", "/cal/dir/a.bin", RomOrigin::File}));
    EXPECT_EQ(outcome->contents.rom, synthetic_rom());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(catalogs.calls, IsEmpty());
    EXPECT_FALSE(outcome->size_rejected);
}

TEST_F(RomOpenBasics, APathWithoutABasenameIsShownAsDefaultBin)
{
    put_rom("/cal/dir/", synthetic_rom());

    const auto outcome = opener.open_file("/cal/dir/");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.source.display_name, "default.bin");
}

TEST_F(RomOpenBasics, AnEmptyPathIsRejected)
{
    EXPECT_THAT(opener.open_file(""), IsErr(ErrorKind::InvalidConfig));
}

TEST_F(RomOpenBasics, AnUnreadableFileFailsWithTheLegacyNotice)
{
    cfg.file_repository.read_errors["/cal/a.bin"] = Error{ErrorKind::Disconnected, "gone"};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsErr(ErrorKind::Disconnected));
    EXPECT_THAT(notices(), Contains("Calibration file: Unable to open calibration file for reading"));
    EXPECT_THAT(log_text(), Contains("Unable to open calibration file [Disconnected]: gone"));
}

TEST_F(RomOpenBasics, FileOpenDerivesProtocolInfoFromTheSelectedVehicle)
{
    // Row 0 (proto_a: checksum yes, mcu SH7058) is selected at startup, and
    // an empty flash method matches no vehicle.
    put_rom("/cal/a.bin", std::vector<std::uint8_t>(3 * 1024 + 5, 0));

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    const RomProtocolInfo& protocol = outcome->contents.protocol;
    EXPECT_FALSE(outcome->vehicle_selected);
    EXPECT_EQ(protocol.flash_method, "");
    EXPECT_EQ(protocol.checksum_module, "checksum"); // "checksum" + flash_method minus 3 chars
    EXPECT_EQ(protocol.mcu_type, "SH7058");
    EXPECT_EQ(protocol.file_size_label, "3kb");
    EXPECT_EQ(protocol.unpadded_size, 3U * 1024U + 5U);
    EXPECT_EQ(protocol.rom_id, "");
    EXPECT_EQ(*cfg.session.selected_row(), 0U);
}

TEST_F(RomOpenBasics, AdoptingAReadImageBacksItUpAndSelectsItsProtocol)
{
    ReadImage image{
        .rom = synthetic_rom(),
        .filename = "A2WC522N2026-09-28_10h00m00s.bin",
        .rom_id = "A2WC522N",
        .protocol_name = "proto_b",
        .kernel_path = "/kernels/b.bin",
        .kernel_start_address = "0x0",
    };

    const auto outcome = opener.adopt_read_image(std::move(image));

    ASSERT_THAT(outcome, IsOk());
    const std::string backup = cfg.session.effective_paths().calibration_files_directory + "read.bin";
    EXPECT_EQ(cfg.file_repository.files.at(backup), synthetic_rom());
    EXPECT_EQ(outcome->contents.source.origin, RomOrigin::EcuRead);
    EXPECT_EQ(outcome->contents.source.display_name, "A2WC522N2026-09-28_10h00m00s.bin");
    EXPECT_TRUE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg.session.selected_row(), 1U);
    const RomProtocolInfo& protocol = outcome->contents.protocol;
    EXPECT_EQ(protocol.flash_method, "proto_b");
    EXPECT_EQ(protocol.rom_id, "A2WC522N");
    EXPECT_EQ(protocol.checksum_module, "Not implemented yet"); // proto_b checksum n/a
    EXPECT_EQ(protocol.mcu_type, "M32R");
    EXPECT_EQ(protocol.kernel_path, "/kernels/b.bin");
    EXPECT_EQ(protocol.kernel_start_address, "0x0");
}

TEST_F(RomOpenBasics, AdoptionNeedsAFilenameAndBytes)
{
    EXPECT_THAT(opener.adopt_read_image(ReadImage{.rom = synthetic_rom()}), IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(opener.adopt_read_image(ReadImage{.filename = "x.bin"}), IsErr(ErrorKind::InvalidConfig));
    EXPECT_TRUE(cfg.file_repository.write_calls.empty());
}

TEST_F(RomOpenBasics, AFailedBackupDoesNotFailTheAdoption)
{
    const std::string backup = cfg.session.effective_paths().calibration_files_directory + "read.bin";
    cfg.file_repository.write_errors[backup] = Error{ErrorKind::Disconnected, "disk full"};

    EXPECT_THAT(opener.adopt_read_image(ReadImage{.rom = synthetic_rom(), .filename = "x.bin"}), IsOk());
}

TEST_F(RomOpenBasics, PaddingFollowsTheUnpaddedSizeLabel)
{
    const auto outcome = opener.adopt_read_image(ReadImage{
        .rom = std::vector<std::uint8_t>(0x100, 0x11),
        .filename = "x.bin",
        .protocol_name = "sub_ecu_denso_mc68hc16y5_02",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.file_size_label, "0kb");
    EXPECT_EQ(outcome->contents.protocol.unpadded_size, 0x100U);
    EXPECT_EQ(outcome->contents.rom.size(), 0x28000U); // zero-extended to 0x20000, then 0x8000 of 0xFF
}

TEST_F(RomOpenBasics, NoCatalogIsRequestedWhileTheEcuFlashDirectoryIsEmpty)
{
    // EcuFlash is primary by default but needs a directory to be consulted.
    cfg.session.settings().use_ecuflash_definitions = "enabled";
    put_rom("/cal/a.bin", synthetic_rom());

    ASSERT_THAT(opener.open_file("/cal/a.bin"), IsOk());
    EXPECT_THAT(catalogs.calls, IsEmpty());
}

} // namespace
} // namespace fastecu::calibration
```

Append to `src/backend/calibration/session/BUILD.bazel`:

```python
cc_library(
    name = "definition_catalogs",
    hdrs = ["definition_catalogs.h"],
    deps = [
        "//src/backend/definition:definition_model",
        "//src/backend/ports",
    ],
)

cc_library(
    name = "rom_open",
    srcs = ["rom_open.cpp"],
    hdrs = ["rom_open.h"],
    deps = [
        ":calibration_session",
        ":definition_catalogs",
        "//src/backend/calibration:calibration_service",
        "//src/backend/config:config_session",
        "//src/backend/definition:definition_model",
        "//src/backend/definition:definition_service",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "rom_open_test",
    srcs = ["rom_open_test.cpp"],
    deps = [
        ":rom_open",
        "//src/backend/calibration/session/testing:fake_definition_catalogs",
        "//src/backend/config/testing:config_session_fixture",
        "//src/backend/ports/testing:in_memory_atomic_file_writer",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/calibration/session:rom_open_test`
Expected: FAIL, because `rom_open.h` does not exist.

- [ ] **Step 4: Write the header**

Create `src/backend/calibration/session/rom_open.h`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/definition_catalogs.h"
#include "src/backend/config/config_session.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// An image just read off an ECU, handed over only after the read succeeded.
struct ReadImage
{
    std::vector<std::uint8_t> rom;
    // Required. The caller names it (flash::read_image_filename), as
    // MainWindow already does before every legacy open.
    std::string filename;
    // As reported by the read; a definition match replaces it.
    std::string rom_id;
    // The protocol selected when the read ran.
    std::string protocol_name;
    std::string kernel_path;
    std::string kernel_start_address;
};

struct RomOpenOutcome
{
    SessionContents contents;
    // ConfigSession::select_by_protocol_name found the ROM's flash method, so
    // the selected vehicle may have changed. The UI refreshes its protocol
    // display when this is set.
    bool vehicle_selected{false};
    // The definition addresses beyond the image: its header is kept and its
    // maps are dropped, as legacy's NameList.clear() did.
    bool size_rejected{false};
};

// FileActions::open_subaru_rom_file on typed values. Only reading the image
// can fail the open; every later problem degrades to a definition-less or
// map-less session, with the legacy log lines and notices.
class RomOpenUseCase
{
  public:
    RomOpenUseCase(IDefinitionCatalogs& catalogs, definition::DefinitionService& definitions,
                   IFileRepository& files, IFileSystem& file_system, IEventSink& events,
                   config::ConfigSession& config);

    Result<RomOpenOutcome> open_file(std::string_view path);
    Result<RomOpenOutcome> adopt_read_image(ReadImage image);

  private:
    struct Seed
    {
        RomSource source;
        std::vector<std::uint8_t> rom;
        std::string rom_id;
        std::string flash_method;
        std::string kernel_path;
        std::string kernel_start_address;
    };

    RomOpenOutcome finish(Seed seed);
    std::optional<ResolvedDefinition> find_definition(std::span<const std::uint8_t> rom, std::string& rom_id);
    std::optional<ResolvedDefinition> try_format(definition::DefinitionFormat format,
                                                 std::span<const std::uint8_t> rom, std::string& rom_id);
    std::string resolve_alias(const std::string& flash_method);
    void log_error(std::string_view operation, const Error& error);

    IDefinitionCatalogs& catalogs_;
    definition::DefinitionService& definitions_;
    IFileRepository& files_;
    IFileSystem& file_system_;
    IEventSink& events_;
    config::ConfigSession& config_;
};

} // namespace fastecu::calibration
```

- [ ] **Step 5: Write the implementation (definitions not yet consulted)**

Create `src/backend/calibration/session/rom_open.cpp`. `find_definition` returns `std::nullopt` in this task; Task 4 replaces its body.

```cpp
#include "src/backend/calibration/session/rom_open.h"

#include <format>
#include <utility>

#include "src/backend/calibration/calibration_service.h"

namespace fastecu::calibration
{
namespace
{

// QFileInfo::fileName() on the '/'-separated paths Qt's file dialogs return,
// with legacy's fallback for a path that names no file.
std::string display_name(std::string_view path)
{
    const std::size_t slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return name.empty() ? std::string{"default.bin"} : std::string{name};
}

// Legacy: QString(flash_method).remove(0, 3).insert(0, "checksum").
std::string checksum_module_for(const std::string& flash_method)
{
    return "checksum" + (flash_method.size() > 3 ? flash_method.substr(3) : std::string{});
}

} // namespace

RomOpenUseCase::RomOpenUseCase(IDefinitionCatalogs& catalogs, definition::DefinitionService& definitions,
                               IFileRepository& files, IFileSystem& file_system, IEventSink& events,
                               config::ConfigSession& config)
    : catalogs_(catalogs), definitions_(definitions), files_(files), file_system_(file_system), events_(events),
      config_(config)
{
}

Result<RomOpenOutcome> RomOpenUseCase::open_file(std::string_view path)
{
    if (path.empty())
    {
        return fail(ErrorKind::InvalidConfig, "open_file called with no filename");
    }
    Result<std::vector<std::uint8_t>> rom = read_rom(path, files_);
    if (!rom.has_value())
    {
        log_error("Unable to open calibration file", rom.error());
        events_.notice("Calibration file: Unable to open calibration file for reading");
        return std::unexpected(rom.error());
    }
    return finish(Seed{
        .source = {.display_name = display_name(path), .path = std::string{path}, .origin = RomOrigin::File},
        .rom = std::move(*rom),
    });
}

Result<RomOpenOutcome> RomOpenUseCase::adopt_read_image(ReadImage image)
{
    if (image.filename.empty())
    {
        return fail(ErrorKind::InvalidConfig, "adopt_read_image called with no filename");
    }
    if (image.rom.empty())
    {
        return fail(ErrorKind::InvalidConfig, "adopt_read_image called with an empty image");
    }
    // Fire-and-forget, as legacy: a failed backup must not fail the open.
    backup_rom(image.rom, config_.effective_paths().calibration_files_directory + "read.bin", files_);
    return finish(Seed{
        .source = {.display_name = display_name(image.filename),
                   .path = image.filename,
                   .origin = RomOrigin::EcuRead},
        .rom = std::move(image.rom),
        .rom_id = std::move(image.rom_id),
        .flash_method = std::move(image.protocol_name),
        .kernel_path = std::move(image.kernel_path),
        .kernel_start_address = std::move(image.kernel_start_address),
    });
}

RomOpenOutcome RomOpenUseCase::finish(Seed seed)
{
    RomOpenOutcome outcome;
    std::string rom_id = std::move(seed.rom_id);
    std::optional<ResolvedDefinition> definition = find_definition(seed.rom, rom_id);

    // A loaded definition replaces the seed's flash method and checksum
    // module, as legacy's wholesale RomInfo replacement did; only then is the
    // flash-method alias resolved.
    std::string flash_method = std::move(seed.flash_method);
    std::string checksum_module;
    if (definition.has_value())
    {
        flash_method = resolve_alias(definition->definition.metadata.flash_method);
        checksum_module = definition->definition.metadata.checksum_module;
    }

    outcome.vehicle_selected = config_.select_by_protocol_name(flash_method);
    const config::ResolvedCarModel *vehicle = config_.selected_vehicle();
    const std::string selected_checksum =
        vehicle != nullptr ? config::protocol_field_or_placeholder(*vehicle, &config::ProtocolEntry::checksum)
                           : std::string{};
    if (selected_checksum == "yes")
    {
        checksum_module = checksum_module_for(flash_method);
    }
    else if (selected_checksum == "n/a")
    {
        checksum_module = "Not implemented yet";
    }
    else if (selected_checksum == "no")
    {
        checksum_module = "No checksums";
    }

    const std::size_t unpadded_size = seed.rom.size();
    std::vector<std::uint8_t> rom = apply_flash_method_padding(std::move(seed.rom), flash_method);

    if (definition.has_value())
    {
        const Status size_ok = validate_rom_size(definition->definition, rom.size());
        if (!size_ok.has_value())
        {
            log_error("Error in expected ROM size", size_ok.error());
            events_.notice("File size error: Error in expected ROM size!");
            definition->definition.maps.clear();
            outcome.size_rejected = true;
        }
    }

    outcome.contents = SessionContents{
        .source = std::move(seed.source),
        .rom = std::move(rom),
        .definition = std::move(definition),
        .protocol =
            RomProtocolInfo{
                .flash_method = flash_method,
                .checksum_module = std::move(checksum_module),
                .mcu_type = vehicle != nullptr
                                ? config::protocol_field_or_placeholder(*vehicle, &config::ProtocolEntry::mcu)
                                : std::string{},
                .kernel_path = std::move(seed.kernel_path),
                .kernel_start_address = std::move(seed.kernel_start_address),
                .rom_id = std::move(rom_id),
                .file_size_label = std::format("{}kb", unpadded_size / 1024),
                .unpadded_size = unpadded_size,
            },
    };
    return outcome;
}

std::optional<ResolvedDefinition> RomOpenUseCase::find_definition(std::span<const std::uint8_t> /*rom*/,
                                                                  std::string& /*rom_id*/)
{
    return std::nullopt; // Task 4
}

std::optional<ResolvedDefinition> RomOpenUseCase::try_format(definition::DefinitionFormat /*format*/,
                                                             std::span<const std::uint8_t> /*rom*/,
                                                             std::string& /*rom_id*/)
{
    return std::nullopt; // Task 4
}

std::string RomOpenUseCase::resolve_alias(const std::string& flash_method)
{
    return flash_method; // Task 4
}

void RomOpenUseCase::log_error(std::string_view operation, const Error& error)
{
    events_.log(LogLevel::Error, std::format("{} [{}]: {}", operation, to_string(error.kind), error.detail));
}

} // namespace fastecu::calibration
```

The three `// Task 4` bodies are replaced in the very next task, on the same branch before any push. They exist only so this task's tests compile and run.

Register the new targets in `bazel/portable_targets.bzl`:

```python
    "src/backend/calibration/session": [
        "calibration_session",
        "definition_catalogs",
        "rom_open",
    ],
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/calibration/session:all && bazel build --config=release //:portable_closure`
Expected: PASS.

If `PaddingFollowsTheUnpaddedSizeLabel` fails on the size, read `apply_flash_method_padding` in `calibration_service.h`. Its documented result for a 0x100-byte input is 0x20000 zero-extended bytes plus 0x8000 of 0xFF = 0x28000. Fix the test only if the documented contract says otherwise.

- [ ] **Step 7: Commit**

```bash
git add src/backend/calibration/session bazel/portable_targets.bzl
git commit -m "feat(calibration): open and adopt ROM images into session contents (6m-1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4: `RomOpenUseCase` — definition matching, loading, aliasing, and size validation

**Files:**
- Modify: `src/backend/calibration/session/rom_open.cpp`
- Test: `src/backend/calibration/session/rom_open_test.cpp`

**Interfaces:**
- Consumes: `DefinitionService::match_rom`, `DefinitionService::load`, `DefinitionCatalog::find`, `IFileSystem::exists`, `validate_rom_size`.
- Produces: no new names. The Task 3 stubs gain their real behavior.

Legacy precedence, reproduced exactly (`file_actions.cpp:522-562`):

```text
if ((primary == "ecuflash" || use_romraider != "enabled") && ecuflash_dir non-empty):
    if use_ecuflash == "enabled":                      try EcuFlash
    if no definition yet && use_romraider == "enabled": try RomRaider
else if (primary == "romraider" && romraider_files non-empty):
    if use_romraider == "enabled":                     try RomRaider
    if no definition yet && use_ecuflash == "enabled":  try EcuFlash
```

Each attempt runs these steps:
1. Build the catalog. On failure, log `Unable to match <Fmt> definition`.
2. Run `match_rom`. On success, the matched definition ID replaces `rom_id` and `"<Fmt> cal id <id> found"` is logged at Debug. On failure, log `Unable to match <Fmt> definition` and keep `rom_id`.
3. If `rom_id` is empty or not in the catalog, the attempt ends silently.
4. Otherwise `load`. On failure, log `Unable to read <Fmt> definition <id>`. If the entry's source file no longer exists, also notify `Ecu definitions file: Unable to open ECU definition file <source> for reading`.

Alias resolution runs only when a definition loaded. It uses the first vehicle whose comma-split `alias` field contains the flash method, logging `Alias: <method>` and `Protocol: <name>` at Debug.

- [ ] **Step 1: Write the failing tests**

Append inside the anonymous namespace of `rom_open_test.cpp`, before its closing `} // namespace`:

```cpp
// A one-map EcuFlash definition identified by "TESTROM" at 0x10, whose
// flash method is proto_a's alias. Its map is the single byte at 0x20.
constexpr std::string_view kTestDefinition = R"xml(
<rom>
  <romid><xmlid>TESTROM</xmlid><internalidaddress>10</internalidaddress>
    <internalidstring>TESTROM</internalidstring><make>Subaru</make>
    <flashmethod>alias_a</flashmethod><checksummodule>def-module</checksummodule></romid>
  <scaling name="Raw" toexpr="x" frexpr="x" format="%d" storagetype="uint8" endian="big"/>
  <table name="Idle" address="20" type="1D" scaling="Raw" storagetype="uint8"/>
</rom>)xml";

definition::DefinitionIndexEntry test_entry(DefinitionFormat format, std::string source = "/defs/test.xml")
{
    return definition::DefinitionIndexEntry{
        .format = format,
        .definition_id = "TESTROM",
        .internal_id = "TESTROM",
        .internal_id_address = 0x10,
        .internal_id_encoding = definition::IdEncoding::Ascii,
        .source = std::move(source),
    };
}

class RomOpenDefinitions : public RomOpenTest
{
  protected:
    void SetUp() override
    {
        RomOpenTest::SetUp();
        cfg.put("/defs/test.xml", kTestDefinition);
        cfg.file_system.files["/defs/test.xml"] = {};
        put_rom("/cal/a.bin", synthetic_rom());
    }

    void enable_ecuflash_primary()
    {
        auto& settings = cfg.session.settings();
        settings.primary_definition_base = "ecuflash";
        settings.use_ecuflash_definitions = "enabled";
        settings.ecuflash_definition_files_directory = "/defs/";
    }

    void enable_romraider_primary()
    {
        auto& settings = cfg.session.settings();
        settings.primary_definition_base = "romraider";
        settings.use_romraider_definitions = "enabled";
        settings.romraider_definition_files = {"/defs/test.xml"};
    }
};

TEST_F(RomOpenDefinitions, MatchesLoadsAndDecodesFromThePrimaryFormat)
{
    enable_ecuflash_primary();
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->format, DefinitionFormat::EcuFlash);
    EXPECT_EQ(outcome->contents.definition->id, "TESTROM");
    EXPECT_EQ(outcome->contents.protocol.rom_id, "TESTROM");
    EXPECT_THAT(log_text(), Contains("EcuFlash cal id TESTROM found"));

    const CalibrationSession session(SessionId{1}, outcome->contents);
    const auto idle = session.decode_map(0);
    ASSERT_THAT(idle, IsOk());
    EXPECT_EQ(idle->map_data, "42,");
}

TEST_F(RomOpenDefinitions, ADefinitionsFlashMethodAliasSelectsItsVehicle)
{
    enable_ecuflash_primary();
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.flash_method, "proto_a"); // alias_a -> proto_a
    EXPECT_TRUE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg.session.selected_row(), 2U); // last row using proto_a
    EXPECT_THAT(log_text(), Contains("Alias: alias_a"));
    EXPECT_THAT(log_text(), Contains("Protocol: proto_a"));
    // proto_a's checksum is "yes": legacy's label drops three characters.
    EXPECT_EQ(outcome->contents.protocol.checksum_module, "checksumto_a");
}

TEST_F(RomOpenDefinitions, AnUnaliasedFlashMethodIsKept)
{
    enable_ecuflash_primary();
    std::string text{kTestDefinition};
    text.replace(text.find("alias_a"), 7, "unknown_method");
    cfg.put("/defs/test.xml", text);
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(outcome->contents.protocol.flash_method, "unknown_method");
    EXPECT_FALSE(outcome->vehicle_selected);
    EXPECT_EQ(*cfg.session.selected_row(), 0U);
}

TEST_F(RomOpenDefinitions, TheSecondaryFormatIsTriedWhenThePrimaryFindsNothing)
{
    enable_ecuflash_primary();
    cfg.session.settings().use_romraider_definitions = "enabled";
    cfg.session.settings().primary_definition_base = "ecuflash";
    catalogs.entries[DefinitionFormat::RomRaider] = {test_entry(DefinitionFormat::RomRaider)};
    cfg.put("/defs/test.xml", R"xml(<roms><rom><romid><xmlid>TESTROM</xmlid>
        <internalidaddress>10</internalidaddress><internalidstring>TESTROM</internalidstring></romid></rom></roms>)xml");

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_EQ(catalogs.calls, (std::vector{DefinitionFormat::EcuFlash, DefinitionFormat::RomRaider}));
    EXPECT_THAT(log_text(), Contains(HasSubstr("Unable to match EcuFlash definition")));
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->format, DefinitionFormat::RomRaider);
}

TEST_F(RomOpenDefinitions, RomRaiderPrimaryIsTriedFirst)
{
    enable_romraider_primary();
    cfg.session.settings().use_ecuflash_definitions = "enabled";

    ASSERT_THAT(opener.open_file("/cal/a.bin"), IsOk());

    EXPECT_EQ(catalogs.calls, (std::vector{DefinitionFormat::RomRaider, DefinitionFormat::EcuFlash}));
}

TEST_F(RomOpenDefinitions, APrimaryHitSkipsTheSecondary)
{
    enable_ecuflash_primary();
    cfg.session.settings().use_romraider_definitions = "enabled";
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    ASSERT_THAT(opener.open_file("/cal/a.bin"), IsOk());

    EXPECT_EQ(catalogs.calls, (std::vector{DefinitionFormat::EcuFlash}));
}

TEST_F(RomOpenDefinitions, ACatalogFailureIsLoggedAndTheRomStillOpens)
{
    enable_ecuflash_primary();
    catalogs.errors[DefinitionFormat::EcuFlash] = Error{ErrorKind::Disconnected, "no dir"};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(log_text(), Contains("Unable to match EcuFlash definition [Disconnected]: no dir"));
}

TEST_F(RomOpenDefinitions, EcuReportedIdIsUsedWhenMatchingFails)
{
    enable_ecuflash_primary();
    definition::DefinitionIndexEntry entry = test_entry(DefinitionFormat::EcuFlash);
    entry.internal_id = "NOT-IN-ROM";
    catalogs.entries[DefinitionFormat::EcuFlash] = {entry};

    const auto outcome = opener.adopt_read_image(ReadImage{
        .rom = synthetic_rom(),
        .filename = "x.bin",
        .rom_id = "TESTROM",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->id, "TESTROM");
    EXPECT_EQ(outcome->contents.protocol.rom_id, "TESTROM");
}

TEST_F(RomOpenDefinitions, AnIdOutsideTheCatalogEndsTheAttemptSilently)
{
    enable_ecuflash_primary();
    definition::DefinitionIndexEntry entry = test_entry(DefinitionFormat::EcuFlash);
    entry.internal_id = "NOT-IN-ROM";
    catalogs.entries[DefinitionFormat::EcuFlash] = {entry};

    const auto outcome = opener.adopt_read_image(ReadImage{
        .rom = synthetic_rom(),
        .filename = "x.bin",
        .rom_id = "OTHER",
        .protocol_name = "proto_b",
    });

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.protocol.rom_id, "OTHER");
    EXPECT_THAT(notices(), IsEmpty());
}

TEST_F(RomOpenDefinitions, MissingDefinitionFileOpensWithoutDefinitionAndNotifies)
{
    enable_ecuflash_primary();
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash, "/defs/gone.xml")};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(log_text(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(notices(),
                Contains("Ecu definitions file: Unable to open ECU definition file /defs/gone.xml for reading"));
}

TEST_F(RomOpenDefinitions, AnUnparseableDefinitionThatExistsIsLoggedWithoutANotice)
{
    enable_ecuflash_primary();
    cfg.put("/defs/test.xml", "<rom><romid>");
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};

    const auto outcome = opener.open_file("/cal/a.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_FALSE(outcome->contents.definition.has_value());
    EXPECT_THAT(log_text(), Contains(HasSubstr("Unable to read EcuFlash definition TESTROM")));
    EXPECT_THAT(notices(), IsEmpty());
}

TEST_F(RomOpenDefinitions, SizeRejectionKeepsHeaderAndDropsMaps)
{
    enable_ecuflash_primary();
    catalogs.entries[DefinitionFormat::EcuFlash] = {test_entry(DefinitionFormat::EcuFlash)};
    std::vector<std::uint8_t> short_rom = synthetic_rom();
    short_rom.resize(0x20); // the map at 0x20 is now past the end
    put_rom("/cal/short.bin", short_rom);

    const auto outcome = opener.open_file("/cal/short.bin");

    ASSERT_THAT(outcome, IsOk());
    EXPECT_TRUE(outcome->size_rejected);
    ASSERT_TRUE(outcome->contents.definition.has_value());
    EXPECT_EQ(outcome->contents.definition->definition.identity.xml_id, "TESTROM");
    EXPECT_THAT(outcome->contents.definition->definition.maps, IsEmpty());
    EXPECT_THAT(notices(), Contains("File size error: Error in expected ROM size!"));
    EXPECT_THAT(log_text(), Contains(HasSubstr("Error in expected ROM size")));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/calibration/session:rom_open_test`
Expected: FAIL. The `RomOpenDefinitions.*` tests report no definition, and `catalogs.calls` is empty. The `RomOpenBasics.*` tests still pass.

- [ ] **Step 3: Replace the three stubs**

In `rom_open.cpp`, add `#include <ranges>` and `#include <string_view>`, then add to the anonymous namespace:

```cpp
std::string_view format_name(definition::DefinitionFormat format)
{
    return format == definition::DefinitionFormat::EcuFlash ? "EcuFlash" : "RomRaider";
}

// Legacy: QString(alias).split(",").contains(flash_method).
bool alias_list_contains(std::string_view aliases, std::string_view flash_method)
{
    for (const auto part : std::views::split(aliases, ','))
    {
        if (std::string_view{part.begin(), part.end()} == flash_method)
        {
            return true;
        }
    }
    return false;
}
```

Replace the three stub bodies:

```cpp
std::optional<ResolvedDefinition> RomOpenUseCase::find_definition(std::span<const std::uint8_t> rom,
                                                                  std::string& rom_id)
{
    using definition::DefinitionFormat;
    const config::AppConfig& settings = config_.settings();
    const bool ecuflash_enabled = settings.use_ecuflash_definitions == "enabled";
    const bool romraider_enabled = settings.use_romraider_definitions == "enabled";

    std::optional<ResolvedDefinition> found;
    if ((settings.primary_definition_base == "ecuflash" || !romraider_enabled) &&
        !settings.ecuflash_definition_files_directory.empty())
    {
        if (ecuflash_enabled)
        {
            found = try_format(DefinitionFormat::EcuFlash, rom, rom_id);
        }
        if (!found.has_value() && romraider_enabled)
        {
            found = try_format(DefinitionFormat::RomRaider, rom, rom_id);
        }
    }
    else if (settings.primary_definition_base == "romraider" && !settings.romraider_definition_files.empty())
    {
        if (romraider_enabled)
        {
            found = try_format(DefinitionFormat::RomRaider, rom, rom_id);
        }
        if (!found.has_value() && ecuflash_enabled)
        {
            found = try_format(DefinitionFormat::EcuFlash, rom, rom_id);
        }
    }
    return found;
}

std::optional<ResolvedDefinition> RomOpenUseCase::try_format(definition::DefinitionFormat format,
                                                             std::span<const std::uint8_t> rom, std::string& rom_id)
{
    const std::string match_operation = std::format("Unable to match {} definition", format_name(format));
    Result<definition::DefinitionCatalog> catalog = catalogs_.catalog(format);
    if (!catalog.has_value())
    {
        log_error(match_operation, catalog.error());
        return std::nullopt;
    }

    Result<definition::DefinitionIndexEntry> match = definitions_.match_rom(*catalog, rom);
    if (match.has_value())
    {
        rom_id = match->definition_id;
        events_.log(LogLevel::Debug, std::format("{} cal id {} found", format_name(format), rom_id));
    }
    else
    {
        // Legacy keeps the previous ID -- for an ECU read, the one the ECU
        // reported -- and still tries to load it below.
        log_error(match_operation, match.error());
    }

    if (rom_id.empty())
    {
        return std::nullopt;
    }
    auto entry = catalog->find(format, rom_id);
    if (!entry.has_value())
    {
        return std::nullopt;
    }
    const std::string source = entry->get().source;

    Result<definition::RomDefinition> loaded = definitions_.load(*catalog, format, rom_id);
    if (!loaded.has_value())
    {
        log_error(std::format("Unable to read {} definition {}", format_name(format), rom_id), loaded.error());
        if (!source.empty() && !file_system_.exists(source))
        {
            events_.notice(
                std::format("Ecu definitions file: Unable to open ECU definition file {} for reading", source));
        }
        return std::nullopt;
    }
    return ResolvedDefinition{.format = format, .id = rom_id, .definition = std::move(*loaded)};
}

std::string RomOpenUseCase::resolve_alias(const std::string& flash_method)
{
    for (const config::ResolvedCarModel& vehicle : config_.vehicles())
    {
        const std::string aliases = config::protocol_field_or_placeholder(vehicle, &config::ProtocolEntry::alias);
        if (alias_list_contains(aliases, flash_method))
        {
            events_.log(LogLevel::Debug, std::format("Alias: {}", flash_method));
            events_.log(LogLevel::Debug, std::format("Protocol: {}", vehicle.protocol_name));
            return vehicle.protocol_name;
        }
    }
    return flash_method;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/calibration/session:all`
Expected: PASS.

Diagnose these two failure modes against the parsers; do not loosen assertions:
- **`MatchesLoadsAndDecodesFromThePrimaryFormat` fails in `load`.** Check the EcuFlash attribute names against `ecuflash_parser_test.cpp` (`ParsesMetadataGlobalScalingsAndNestedAxes`) and correct `kTestDefinition`.
- **`TheSecondaryFormatIsTriedWhenThePrimaryFindsNothing` fails in `load`.** Check the RomRaider fixture against `romraider_parser_test.cpp` in the same way.

- [ ] **Step 5: Mutation-check the two legacy quirks**

Make two temporary edits, one at a time. Confirm that each makes a named test fail, then revert it:
1. In `try_format`, change the `else` branch to also `return std::nullopt;` after logging. `EcuReportedIdIsUsedWhenMatchingFails` must fail.
2. In `finish`, compute the file-size label from `rom.size()` after padding. `PaddingFollowsTheUnpaddedSizeLabel` must fail.

Run after each edit: `bazel test --config=release //src/backend/calibration/session:rom_open_test`

- [ ] **Step 6: Commit**

```bash
git add src/backend/calibration/session/rom_open.cpp src/backend/calibration/session/rom_open_test.cpp
git commit -m "feat(calibration): match, load and validate definitions on ROM open (6m-1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 5: `CalibrationWorkspace`

**Files:**
- Create: `src/backend/calibration/session/calibration_workspace.h`, `calibration_workspace.cpp`, `calibration_workspace_test.cpp`
- Modify: `src/backend/calibration/session/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `RomOpenUseCase`, `ReadImage`, `RomOpenOutcome` (Task 3); `CalibrationSession` (Task 2).
- Produces:
  - `struct OpenedSession { SessionId id; bool vehicle_selected; bool size_rejected; }`.
  - `class CalibrationWorkspace`, with:
    - `explicit CalibrationWorkspace(RomOpenUseCase&)`
    - `Result<OpenedSession> open_file(std::string_view)`
    - `Result<OpenedSession> adopt_read_image(ReadImage)`
    - `Status close(SessionId)`
    - `CalibrationSession *find(SessionId)` and `const CalibrationSession *find(SessionId) const`
    - `std::vector<SessionId> ids() const`, in open order
  - Pointers returned by `find` stay valid until that session is closed or the workspace is destroyed.

- [ ] **Step 1: Write the failing tests**

Create `src/backend/calibration/session/calibration_workspace_test.cpp`:

```cpp
#include "src/backend/calibration/session/calibration_workspace.h"

#include <cstdint>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/calibration/session/testing/fake_definition_catalogs.h"
#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::calibration
{
namespace
{

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

class CalibrationWorkspaceTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_THAT(cfg.initialize(), IsOk());
        cfg.file_repository.files["/cal/a.bin"] = std::vector<std::uint8_t>(16, 0xA);
        cfg.file_repository.files["/cal/b.bin"] = std::vector<std::uint8_t>(16, 0xB);
    }

    config::testing::ConfigSessionFixture cfg;
    InMemoryAtomicFileWriter writer;
    definition::DefinitionService definitions{cfg.file_system, cfg.file_repository, writer};
    testing::FakeDefinitionCatalogs catalogs;
    RomOpenUseCase opener{catalogs,      definitions, cfg.file_repository, cfg.file_system,
                          cfg.events, cfg.session};
    CalibrationWorkspace workspace{opener};
};

TEST_F(CalibrationWorkspaceTest, StartsEmpty)
{
    EXPECT_THAT(workspace.ids(), IsEmpty());
    EXPECT_EQ(workspace.find(SessionId{1}), nullptr);
}

TEST_F(CalibrationWorkspaceTest, OpenedSessionsAreFoundInOpenOrder)
{
    const auto a = workspace.open_file("/cal/a.bin");
    const auto b = workspace.open_file("/cal/b.bin");

    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(b, IsOk());
    EXPECT_NE(a->id, b->id);
    EXPECT_THAT(workspace.ids(), ElementsAre(a->id, b->id));
    ASSERT_NE(workspace.find(a->id), nullptr);
    EXPECT_EQ(workspace.find(a->id)->source().display_name, "a.bin");
    EXPECT_EQ(workspace.find(b->id)->rom()[0], 0xB);
}

TEST_F(CalibrationWorkspaceTest, AFailedOpenLeavesTheWorkspaceUnchanged)
{
    const auto a = workspace.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());

    EXPECT_THAT(workspace.open_file("/cal/missing.bin"), ::testing::Not(IsOk()));
    EXPECT_THAT(workspace.adopt_read_image(ReadImage{.filename = "x.bin"}), IsErr(ErrorKind::InvalidConfig));

    EXPECT_THAT(workspace.ids(), ElementsAre(a->id));
}

TEST_F(CalibrationWorkspaceTest, AdoptedImagesBecomeSessions)
{
    const auto read = workspace.adopt_read_image(
        ReadImage{.rom = std::vector<std::uint8_t>(8, 0x5), .filename = "r.bin", .protocol_name = "proto_b"});

    ASSERT_THAT(read, IsOk());
    EXPECT_TRUE(read->vehicle_selected);
    ASSERT_NE(workspace.find(read->id), nullptr);
    EXPECT_EQ(workspace.find(read->id)->source().origin, RomOrigin::EcuRead);
}

TEST_F(CalibrationWorkspaceTest, ClosingKeepsOtherSessionsAndTheirPointers)
{
    const auto a = workspace.open_file("/cal/a.bin");
    const auto b = workspace.open_file("/cal/b.bin");
    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(b, IsOk());
    const CalibrationSession *b_before = workspace.find(b->id);

    ASSERT_THAT(workspace.close(a->id), IsOk());

    EXPECT_EQ(workspace.find(a->id), nullptr);
    EXPECT_EQ(workspace.find(b->id), b_before);
    EXPECT_THAT(workspace.ids(), ElementsAre(b->id));
}

TEST_F(CalibrationWorkspaceTest, ClosedIdsAreNeverReused)
{
    const auto a = workspace.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());
    ASSERT_THAT(workspace.close(a->id), IsOk());

    const auto again = workspace.open_file("/cal/a.bin");

    ASSERT_THAT(again, IsOk());
    EXPECT_NE(again->id, a->id);
    EXPECT_EQ(workspace.find(a->id), nullptr);
    EXPECT_THAT(workspace.close(a->id), IsErr(ErrorKind::InvalidConfig));
}

TEST_F(CalibrationWorkspaceTest, SessionsAreMutableThroughTheWorkspace)
{
    const auto a = workspace.open_file("/cal/a.bin");
    ASSERT_THAT(a, IsOk());
    const std::vector<std::uint8_t> patch{0xFF};

    ASSERT_THAT(workspace.find(a->id)->write_bytes(0, patch), IsOk());

    const CalibrationWorkspace& view = workspace;
    EXPECT_EQ(view.find(a->id)->rom()[0], 0xFF);
    EXPECT_TRUE(view.find(a->id)->dirty());
}

} // namespace
} // namespace fastecu::calibration
```

Append to `src/backend/calibration/session/BUILD.bazel`:

```python
cc_library(
    name = "calibration_workspace",
    srcs = ["calibration_workspace.cpp"],
    hdrs = ["calibration_workspace.h"],
    deps = [
        ":calibration_session",
        ":rom_open",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "calibration_workspace_test",
    srcs = ["calibration_workspace_test.cpp"],
    deps = [
        ":calibration_workspace",
        "//src/backend/calibration/session/testing:fake_definition_catalogs",
        "//src/backend/config/testing:config_session_fixture",
        "//src/backend/ports/testing:in_memory_atomic_file_writer",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test --config=release //src/backend/calibration/session:calibration_workspace_test`
Expected: FAIL, because `calibration_workspace.h` does not exist.

- [ ] **Step 3: Write the header and implementation**

Create `src/backend/calibration/session/calibration_workspace.h`:

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

struct OpenedSession
{
    SessionId id{};
    bool vehicle_selected{false};
    bool size_rejected{false};
};

// The open calibrations, replacing MainWindow's fixed array of 100 raw
// pointers. A session enters only after its open or adoption succeeded, so a
// failed or cancelled read never occupies an entry. Owned by the desktop
// composition, which outlives every window that holds a SessionId.
class CalibrationWorkspace
{
  public:
    explicit CalibrationWorkspace(RomOpenUseCase& opener);

    CalibrationWorkspace(const CalibrationWorkspace&) = delete;
    CalibrationWorkspace& operator=(const CalibrationWorkspace&) = delete;

    Result<OpenedSession> open_file(std::string_view path);
    Result<OpenedSession> adopt_read_image(ReadImage image);
    // InvalidConfig for an ID that is not open.
    Status close(SessionId id);

    // nullptr for an ID that is not open -- an expected outcome for a UI
    // element outliving its session. Pointers stay valid until that session
    // is closed.
    CalibrationSession *find(SessionId id);
    const CalibrationSession *find(SessionId id) const;
    std::vector<SessionId> ids() const;

  private:
    OpenedSession insert(RomOpenOutcome outcome);

    RomOpenUseCase& opener_;
    std::uint64_t next_id_{1};
    std::vector<std::unique_ptr<CalibrationSession>> sessions_;
};

} // namespace fastecu::calibration
```

Create `src/backend/calibration/session/calibration_workspace.cpp`:

```cpp
#include "src/backend/calibration/session/calibration_workspace.h"

#include <algorithm>
#include <format>
#include <utility>

namespace fastecu::calibration
{

CalibrationWorkspace::CalibrationWorkspace(RomOpenUseCase& opener) : opener_(opener)
{
}

Result<OpenedSession> CalibrationWorkspace::open_file(std::string_view path)
{
    Result<RomOpenOutcome> outcome = opener_.open_file(path);
    if (!outcome.has_value())
    {
        return std::unexpected(outcome.error());
    }
    return insert(std::move(*outcome));
}

Result<OpenedSession> CalibrationWorkspace::adopt_read_image(ReadImage image)
{
    Result<RomOpenOutcome> outcome = opener_.adopt_read_image(std::move(image));
    if (!outcome.has_value())
    {
        return std::unexpected(outcome.error());
    }
    return insert(std::move(*outcome));
}

Status CalibrationWorkspace::close(SessionId id)
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->id() == id; });
    if (found == sessions_.end())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("no open calibration session {}", static_cast<std::uint64_t>(id)));
    }
    sessions_.erase(found);
    return {};
}

CalibrationSession *CalibrationWorkspace::find(SessionId id)
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->id() == id; });
    return found == sessions_.end() ? nullptr : found->get();
}

const CalibrationSession *CalibrationWorkspace::find(SessionId id) const
{
    const auto found = std::ranges::find_if(sessions_, [id](const auto& session) { return session->id() == id; });
    return found == sessions_.end() ? nullptr : found->get();
}

std::vector<SessionId> CalibrationWorkspace::ids() const
{
    std::vector<SessionId> result;
    result.reserve(sessions_.size());
    for (const auto& session : sessions_)
    {
        result.push_back(session->id());
    }
    return result;
}

OpenedSession CalibrationWorkspace::insert(RomOpenOutcome outcome)
{
    const SessionId id{next_id_++};
    sessions_.push_back(std::make_unique<CalibrationSession>(id, std::move(outcome.contents)));
    return OpenedSession{
        .id = id,
        .vehicle_selected = outcome.vehicle_selected,
        .size_rejected = outcome.size_rejected,
    };
}

} // namespace fastecu::calibration
```

Register in `bazel/portable_targets.bzl`:

```python
    "src/backend/calibration/session": [
        "calibration_session",
        "calibration_workspace",
        "definition_catalogs",
        "rom_open",
    ],
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/calibration/session:all && bazel build --config=release //:portable_closure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/backend/calibration/session bazel/portable_targets.bzl
git commit -m "feat(calibration): add calibration workspace with stable session ids (6m-1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 6: Composition owns the workspace; `FileActions` supplies catalogs

**Files:**
- Modify: `src/backend/definitions/file_actions.h`, `src/backend/definitions/BUILD.bazel` (target `definitions`)
- Modify: `apps/desktop/desktop_composition.h`, `apps/desktop/desktop_composition.cpp`, `apps/desktop/BUILD.bazel` (target `composition`)
- Test: `apps/desktop/desktop_composition_test.cpp`

**Interfaces:**
- Consumes: `IDefinitionCatalogs`, `RomOpenUseCase`, `CalibrationWorkspace`.
- Produces:
  - `FileActions` publicly implements `fastecu::calibration::IDefinitionCatalogs`. `catalog(format)` returns its existing `build_definition_catalog(format)`.
  - `DesktopComposition` privately owns:
    - `std::unique_ptr<fastecu::definition::DefinitionService> definition_service_`
    - `std::unique_ptr<fastecu::calibration::RomOpenUseCase> rom_open_`
    - `std::unique_ptr<fastecu::calibration::CalibrationWorkspace> calibration_workspace_`
  - These are built only on successful startup and destroyed before `file_actions_`. `MainWindowServices` is **not** changed in this slice; 6m-2 adds the field when `MainWindow` first uses it.

- [ ] **Step 1: Write the failing composition tests**

In `apps/desktop/desktop_composition_test.cpp`:
1. Add `#include "src/backend/calibration/session/calibration_workspace.h"` beside the other includes.
2. Add these two slots next to `failedStartupBuildsNoServicesAndPerformsNoEcuIo`:

```cpp
    void workspaceOpensARomFromDisk()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        QVERIFY(composition.started());
        QVERIFY(composition.calibration_workspace_);

        const QString rom_path = root.filePath("synthetic.bin");
        QFile rom{rom_path};
        QVERIFY(rom.open(QIODevice::WriteOnly));
        QCOMPARE(rom.write(QByteArray(2048, '\x5A')), qint64{2048});
        rom.close();

        const auto opened = composition.calibration_workspace_->open_file(rom_path.toStdString());

        QVERIFY(opened.has_value());
        const auto *session = composition.calibration_workspace_->find(opened->id);
        QVERIFY(session != nullptr);
        QCOMPARE(session->source().display_name, std::string("synthetic.bin"));
        QCOMPARE(session->protocol().file_size_label, std::string("2kb"));
        QCOMPARE(session->rom().size(), std::size_t{2048});
    }

    void failedStartupBuildsNoWorkspace()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString config_dir = root.path() + "/";
        QVERIFY(QDir{}.mkpath(config_dir));
        QVERIFY(writeFile(config_dir + "protocols.cfg", "<config name=\"FastECU\"><protocols/><car_models/></config>"));

        DesktopComposition composition{{}, {}, root.path()};

        QVERIFY(!composition.started());
        QVERIFY(!composition.calibration_workspace_);
        QVERIFY(!composition.rom_open_);
    }
```

Before writing `failedStartupBuildsNoWorkspace`, read the existing `failedStartupBuildsNoServicesAndPerformsNoEcuIo` (around line 136). Copy exactly how it makes startup fail (its `config_dir` and `protocols.cfg` setup) rather than the approximation above; the two tests must fail startup the same way.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: FAIL to compile with `no member named 'calibration_workspace_' in 'DesktopComposition'`.

- [ ] **Step 3: Implement the catalog port on `FileActions`**

In `src/backend/definitions/file_actions.h`:
1. Add `#include "src/backend/calibration/session/definition_catalogs.h"`.
2. Change the class head to `class FileActions : public fastecu::calibration::IDefinitionCatalogs`.
3. Add in its public section:

```cpp
    // IDefinitionCatalogs: the same catalogs the legacy open path builds, so a
    // session open matches against exactly what FileActions would.
    fastecu::Result<fastecu::definition::DefinitionCatalog>
    catalog(fastecu::definition::DefinitionFormat format) override
    {
        return build_definition_catalog(format);
    }
```

If `FileActions` already declares a virtual destructor or is `final`, keep that. If it declares no destructor, the base's virtual destructor suffices.

In `src/backend/definitions/BUILD.bazel`, add `"//src/backend/calibration/session:definition_catalogs"` to the `definitions` target's `deps`.

- [ ] **Step 4: Wire the composition**

In `apps/desktop/desktop_composition.h`, add includes:

```cpp
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/definition/definition_service.h"
```

Add these private members directly after `std::unique_ptr<FileActions> file_actions_;`. Declaration order places them after `file_actions_` and `config_`, so members that reference those are declared later:

```cpp
    std::unique_ptr<fastecu::definition::DefinitionService> definition_service_;
    std::unique_ptr<fastecu::calibration::RomOpenUseCase> rom_open_;
    std::unique_ptr<fastecu::calibration::CalibrationWorkspace> calibration_workspace_;
```

In `desktop_composition.cpp`, immediately after `file_actions_` is constructed, add:

```cpp
    definition_service_ =
        std::make_unique<fastecu::definition::DefinitionService>(file_system_, file_repository_, file_writer_);
    rom_open_ = std::make_unique<fastecu::calibration::RomOpenUseCase>(
        *file_actions_, *definition_service_, file_repository_, file_system_, file_action_events_, config_);
    calibration_workspace_ = std::make_unique<fastecu::calibration::CalibrationWorkspace>(*rom_open_);
```

In the destructor, immediately before `file_actions_.reset(); // before config_, which it references`, add:

```cpp
    // The workspace's opener borrows file_actions_ as its catalog source.
    calibration_workspace_.reset();
    rom_open_.reset();
    definition_service_.reset();
```

In `apps/desktop/BUILD.bazel`, add to the `composition` target's `deps`:

```python
        "//src/backend/calibration/session:calibration_workspace",
        "//src/backend/calibration/session:rom_open",
        "//src/backend/definition:definition_service",
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //apps/desktop:all //src/backend/definitions:all //src/backend/calibration/...`
Expected: PASS.

- [ ] **Step 6: Run the full gates**

Run:
```bash
bazel build -k --config=release //...
bazel test -k --config=release //...
prek run --all-files
bazel run --config=release //:clang_tidy_report_changed
```
Expected: all pass. Fix every clang-tidy finding in new code rather than suppressing it. If `//tests:serial_backend_tests` fails intermittently on Windows CI, that is the known flake: rerun it and do not change code for it.

- [ ] **Step 7: Commit**

```bash
git add src/backend/definitions/file_actions.h src/backend/definitions/BUILD.bazel apps/desktop
git commit -m "feat(desktop): compose the calibration workspace (6m-1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 7: Publish the stack

**Files:** none.

- [ ] **Step 1: Confirm with the user before pushing.** Pushing publishes the branches. Ask first unless the user has already authorized pushing in this session.

- [ ] **Step 2: Submit as a stack**

```bash
gh stack init   # if the stack is not yet initialized, with feat/6m-calibration-sessions as the base branch above master
gh stack submit --auto
```

Expected: two PRs.
- **6m-0** (`feat/6m-calibration-sessions` → `master`) holds the spec and this plan.
- **6m-1** (`refactor/step6m-1-calibration-session` → `feat/6m-calibration-sessions`) holds Tasks 1–6.

Each PR body lists the preserved legacy behaviors that are pinned and states that no consumer is migrated yet. It ends with:

```
🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

- [ ] **Step 3: Report CI.** Report local gate results as run. Report platform CI as pending until it finishes, with a link to it.
