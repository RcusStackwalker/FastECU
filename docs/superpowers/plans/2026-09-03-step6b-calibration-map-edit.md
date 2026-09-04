# Step 6b — Calibration Map-Edit Use Case — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the calibration map-edit arithmetic out of `src/ui/desktop/menu_actions.cpp` into portable, tested operations under `src/backend/calibration`, leaving the UI with selection extraction, prompting, and repaint.

**Architecture:** A new portable target `//src/backend/calibration:map_edit` holds a byte codec (`read_raw_element` / `encode_scaled_value`), a pure target-resolution rule (`resolve_edit_target`), and four patch-returning edit operations. A new Qt adapter package `//src/ui/desktop/calibration` plucks fields from the `QStringList`-typed `EcuCalDefStructure` into a `MapElementSpec` value view at the call boundary, so the legacy model is not converted. Shared scaling helpers are hoisted out of `calibration_service.cpp`'s anonymous namespace so both the decode and encode sides use one formatter.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest 1.17.0.bcr.2, Qt 6.8.3 (UI side only).

**Spec:** [docs/superpowers/specs/2026-09-03-step6b-calibration-map-edit-design.md](../specs/2026-09-03-step6b-calibration-map-edit-design.md)

## Global Constraints

- **Drain independence is a completion criterion, not a preference.** `git diff --name-only origin/master` must contain no path under `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`. Do not edit `mainwindow.cpp:1028-1424` (`start_ecu_operations`) or `mainwindow.h:58-80` (the flash-dialog include block).
- Backend operations return `fastecu::Result<T>` (`std::expected<T, Error>`), checked with `.has_value()`, never the implicit `operator bool`. Exceptions never cross a port. Do not add `ErrorKind` values — the seven in `src/backend/ports/error.h` are the whole set.
- Portable code uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` from `src/algorithms/protocol/bytes.h`. `QByteArray` is a boundary type only, converted explicitly via `bytes::view()` / `bytes::mutableView()` from `qt_bytes.h` (ADR 0004).
- Every header needs `#pragma once` (enforced by prek).
- New portable targets must be registered in **both** the `genquery` in `BUILD.bazel` and `PORTABLE_ROOTS` in `scripts/check-portable-closure.py`. Registering in one only makes the check pass vacuously.
- Tests are package-owned and co-located (`foo.cpp` + `foo_test.cpp` in the same package). Use `fastecu_portable_gtest` from `bazel/gtest_targets.bzl` for Qt-free targets, `fastecu_gtest` for Qt-linked ones.
- Qt targets list moc'd headers in `MOC_HDRS` and everything else in `normal_hdrs`. The UI adapter declares no `Q_OBJECT`, so its header goes in `normal_hdrs` — matching `//src/ui/desktop/menu:menu_builder`.
- Prefer `std::string_view` by value over `const char*` / `const std::string&`; use `std::format` for message construction; use ranges/views over index loops where it reads better.
- Run `prek run --all-files` before every commit. Run `bazel test --config=release //...` before declaring a PR-sized group of tasks done.
- Cross-document Markdown references are links with human-readable text, not backticked paths — lychee cannot see a path written as inline code.

## Task-to-PR mapping

Tasks compose into the spec's four PRs. Each task ends in its own commit; each PR boundary is where `bazel test --config=release //...` must be green before continuing.

| PR | Tasks |
|---|---|
| **6b-1** byte codec | 1, 2, 3, 4 |
| **6b-2** target resolution and display helpers | 5, 6, 7 |
| **6b-3** edit operations and shim drain | 8, 9, 10, 11, 12 |
| **6b-4** the fixes | 13, 14, 15, 16 |

## File Structure

**Created:**

- `src/backend/calibration/scaling_internal.h` / `.cpp` — the five helpers hoisted out of `calibration_service.cpp`'s anonymous namespace (`format_like_qt_g`, `sign_extend`, `checked_add`, `checked_multiply`, `byte_window_fits`). Internal to the package; not exported past it. Exists because the encode/decode round-trip guarantee depends on both sides using the *same* formatter — duplicating `format_like_qt_g` would let the two drift silently.
- `src/backend/calibration/map_edit.h` / `.cpp` — `MapElementSpec`, `SelectionRange`, `MapDimensions`, `EditTarget`, `CellPatch`/`EditPatch`, `read_raw_element`, `encode_scaled_value`, `resolve_edit_target`, `map_value_decimal_count`, `map_cell_color_scale`, and the four `apply_*` operations.
- `src/backend/calibration/map_edit_test.cpp` — the portable suite for all of the above.
- `src/ui/desktop/calibration/BUILD.bazel`, `map_edit_adapter.h` / `.cpp` — the Qt adapter: builds a `MapElementSpec` from `EcuCalDefStructure` for a resolved `EditTargetKind`, and applies an `EditPatch` back to `FullRomData` and the split cell text. New package, following the `src/ui/desktop/menu/` precedent from 6a-1.
- `src/ui/desktop/calibration/map_edit_adapter_test.cpp` — `fastecu_gtest`, offscreen; asserts field plucking for all three target kinds.

**Modified:**

- `src/backend/calibration/calibration_service.cpp` — helpers move out; behavior unchanged.
- `src/backend/calibration/BUILD.bazel` — new `scaling_internal` and `map_edit` targets.
- `BUILD.bazel`, `scripts/check-portable-closure.py` — register `map_edit`.
- `src/ui/desktop/menu_actions.cpp` — the four edit slots shrink to UI adapters; five helper functions leave; `check_rom_data_value` is deleted.
- `src/ui/desktop/mainwindow.h:356-363` — the five private map-edit declarations.
- `src/ui/desktop/BUILD.bazel` — depend on the new adapter; drop `//src/algorithms/menu:qt_compat`.
- `docs/modularization-plan.md`, `docs/tech-debt.md`, `docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md` — status and the menu-shim ownership amendment.

**Deleted:**

- `src/algorithms/menu/qt_menu_command.h`, `src/algorithms/menu/menu_command_qt_compat_test.cpp`, and the `qt_compat` / `menu_command_qt_compat_test` targets in `src/algorithms/menu/BUILD.bazel`.

---

## A prediction this plan must settle first

Tracing the `union map_data` byte order through both paths produces a result serious enough that it must be **verified by a test before any other work is trusted**, not asserted here.

In `get_rom_data_value` (`menu_actions.cpp:1652-1666`), `byte_value[k]` is filled most-significant-byte-first in *both* endian branches. On a little-endian host — every supported host — the union's `sword_value[0]` / `sdword_value` members therefore read byte-swapped for widths above 1. Unsigned reads dodge this because they bypass the union (`dword_value = data_byte` at `:1669`), and float reads are correct because a big-endian-stored float assembles exactly this way. **Signed multi-byte reads look byte-swapped.**

On the write side (`:1741-1751`) the caller packs the raw value into `dword_value` and passes `float_value` (`:414-416`), so `byte_value[]` holds the host's little-endian representation. The `endian == "little"` branch then writes `rom[addr+k] = byte_value[size-1-k]`, which stores **big**-endian; the else branch stores little-endian. **Both branches look inverted relative to their labels**, and relative to how `decode_scaled_values` reads them back.

If that is right, editing any multi-byte map corrupts it — which is implausible for a tool in use, so the reasoning may have a flaw worth finding. Task 3's round-trip test settles it either way, and it is deliberately the third task rather than the last.

**If the round trip passes for all widths**, the analysis above is wrong; delete this section and carry on. **If it fails**, stop and report before writing any `apply_*` operation: the finding changes 6b-4's priority and belongs in the spec and the tech-debt roadmap immediately. Either way, do not "fix" the byte order inside Task 2 or 3 — those tasks preserve legacy behavior, and the pinning test records whatever is true.

---

## Task 1: Hoist the shared scaling helpers

**Files:**
- Create: `src/backend/calibration/scaling_internal.h`, `src/backend/calibration/scaling_internal.cpp`
- Modify: `src/backend/calibration/calibration_service.cpp:15-216` (remove the five helpers from the anonymous namespace), `src/backend/calibration/BUILD.bazel`

**Interfaces:**
- Consumes: nothing.
- Produces: `fastecu::calibration::internal::format_like_qt_g(double value, int precision) -> std::string`; `sign_extend(std::uint32_t raw, std::uint32_t width) -> std::int32_t`; `checked_add(std::uint64_t, std::uint64_t, std::uint64_t&) -> bool`; `checked_multiply(std::uint64_t, std::uint64_t, std::uint64_t&) -> bool`; `byte_window_fits(bytes::ByteView, std::uint64_t address, std::uint64_t width) -> bool`.

This is a pure move with no behavior change. Its test is the existing 911-line `calibration_service_test.cpp` passing unchanged.

- [ ] **Step 1: Read the five helpers before moving them**

Run: `sed -n '15,100p' src/backend/calibration/calibration_service.cpp`

Copy the bodies verbatim. Do not retype them and do not "improve" them — a changed `format_like_qt_g` changes every decoded value in the application.

- [ ] **Step 2: Create the header**

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "src/algorithms/protocol/bytes.h"

// Helpers shared by the decode side (calibration_service.cpp) and the encode
// side (map_edit.cpp). They live here rather than in either .cpp's anonymous
// namespace because the encode/decode round trip is only meaningful if both
// sides format and sign-extend identically.
//
// Internal to //src/backend/calibration. Not part of any public API.
namespace fastecu::calibration::internal
{

// Qt's QString::number(double) formatting, reproduced: 'g' format, `precision`
// significant digits, trailing zeros stripped.
std::string format_like_qt_g(double value, int precision);

// Sign-extends a `width`-byte raw value to a signed 32-bit value.
std::int32_t sign_extend(std::uint32_t raw, std::uint32_t width);

// Overflow-checked arithmetic. Return false and leave `result` unspecified on
// overflow rather than wrapping, so a bad definition cannot silently produce a
// ~4 GB extent.
bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result);
bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result);

// True when [address, address + width) lies wholly inside `data`.
bool byte_window_fits(bytes::ByteView data, std::uint64_t address, std::uint64_t width);

} // namespace fastecu::calibration::internal
```

- [ ] **Step 3: Create the implementation**

Move the five bodies from `calibration_service.cpp` into `scaling_internal.cpp` verbatim, wrapped in `namespace fastecu::calibration::internal { ... }`. Carry each function's existing comments with it. Add the includes each body needs (`<algorithm>`, `<cmath>`, `<format>`, `<limits>`, `<string>`) plus `#include "src/backend/calibration/scaling_internal.h"` first.

- [ ] **Step 4: Point `calibration_service.cpp` at the header**

Delete the five moved functions from the anonymous namespace. Add `#include "src/backend/calibration/scaling_internal.h"` and, just below the `namespace fastecu::calibration` opening brace, `using namespace internal;` — the twelve existing call sites then compile unchanged.

- [ ] **Step 5: Add the Bazel target**

In `src/backend/calibration/BUILD.bazel`, above `calibration_service`:

```python
cc_library(
    name = "scaling_internal",
    srcs = ["scaling_internal.cpp"],
    hdrs = ["scaling_internal.h"],
    visibility = ["//src/backend/calibration:__pkg__"],
    deps = ["//src/algorithms/protocol"],
)
```

Add `":scaling_internal"` to `calibration_service`'s `deps`.

- [ ] **Step 6: Run the existing suite to prove nothing moved semantically**

Run: `bazel test --config=release //src/backend/calibration:calibration_service_test`
Expected: PASS, with the same test count as before the move.

- [ ] **Step 7: Verify the portable closure still holds**

Run: `bazel test --config=release //:portable_closure`
Expected: PASS. `scaling_internal` is reached through `calibration_service`, which is already a portable root, so no registration change is needed for this target.

- [ ] **Step 8: Commit**

```bash
prek run --all-files
git add src/backend/calibration/scaling_internal.h src/backend/calibration/scaling_internal.cpp \
        src/backend/calibration/calibration_service.cpp src/backend/calibration/BUILD.bazel
git commit -m "refactor(calibration): hoist shared scaling helpers out of the anonymous namespace

Pure move, no behavior change. The encode side (step 6b) needs the same
format_like_qt_g and sign_extend the decode side uses; duplicating them would
let the two drift and quietly break the round trip.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 2: `MapElementSpec` and `read_raw_element`

**Files:**
- Create: `src/backend/calibration/map_edit.h`, `src/backend/calibration/map_edit.cpp`, `src/backend/calibration/map_edit_test.cpp`
- Modify: `src/backend/calibration/BUILD.bazel`, `BUILD.bazel`, `scripts/check-portable-closure.py`

**Interfaces:**
- Consumes: `internal::sign_extend`, `internal::byte_window_fits`, `internal::checked_add`, `internal::checked_multiply` from Task 1; `definition::StorageType`, `definition::storage_byte_size`, `definition::is_unsigned_storage` from `//src/backend/definition:definition_model`.
- Produces: `struct MapElementSpec`; `Result<std::int64_t> read_raw_element(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t index)`.

This task reproduces `get_rom_data_value` (`menu_actions.cpp:1610-1697`) **exactly**, including its three defects. Do not fix anything here.

- [ ] **Step 1: Write the failing test**

Create `src/backend/calibration/map_edit_test.cpp`:

```cpp
#include "src/backend/calibration/map_edit.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace fastecu::calibration
{
namespace
{

MapElementSpec uint8_spec()
{
    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    return spec;
}

std::vector<std::uint8_t> rom_of(std::size_t size)
{
    std::vector<std::uint8_t> rom(size, 0x00);
    return rom;
}

TEST(ReadRawElement, ReadsAnUnsignedByteAtTheIndexedOffset)
{
    auto rom = rom_of(0x40);
    rom[0x12] = 0xAB;

    const auto value = read_raw_element(rom, uint8_spec(), 2);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0xAB);
}

TEST(ReadRawElement, ReportsInternalWhenTheWindowRunsPastTheRom)
{
    auto rom = rom_of(0x11);

    const auto value = read_raw_element(rom, uint8_spec(), 8);

    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().kind, ErrorKind::Internal);
}

} // namespace
} // namespace fastecu::calibration
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`
Expected: FAIL — the target does not exist yet, then once declared, a compile error that `map_edit.h` is missing.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Which of a map's three element runs an edit targets. Declared here rather
// than beside resolve_edit_target (Task 5) because the UI adapter (Task 4)
// needs it to choose which parallel-list group to pluck from, and that lands
// first.
enum class EditTargetKind
{
    MapBody,
    XAxis,
    YAxis,
    Rejected,
};

// One run of editable elements: a map's cells, or one axis's points. The
// non-owning counterpart of calibration_service.h's ElementRun, for the write
// side. string_view fields borrow from the EcuCalDefStructure lists the UI
// adapter reads them out of, which always outlive the call. Never store one.
struct MapElementSpec
{
    std::uint64_t address{0};
    std::optional<definition::StorageType> storage_type;
    std::string_view endian;
    std::string_view to_byte{"x"};
    std::string_view from_byte{"x"};
    // " " means "unset" in the legacy model, not "empty". Compared as text
    // before conversion, exactly as the legacy code does.
    std::string_view min_value{" "};
    std::string_view max_value{" "};
    double coarse_increment{0.0};
    double fine_increment{0.0};
    std::uint32_t x_size{1};
    std::uint32_t y_size{1};
    // The wrx02 relocation inputs. Carried as data rather than read from a
    // global so the rule is testable; see the spec's defect (a).
    std::string_view flash_method;
    std::uint64_t rom_file_size{0};
};

// Byte address of element `index`, including the wrx02 relocation. Exposed
// because the read and write paths apply *different* wrx02 predicates today
// and the difference must be visible and separately testable -- see the
// spec's defect (a). `for_write` selects set_rom_data_value's predicate;
// otherwise get_rom_data_value's is used.
std::uint64_t element_byte_address(const MapElementSpec& spec, std::uint32_t index, bool for_write);

// The raw stored value of element `index`, reproducing get_rom_data_value.
//
// Returns ErrorKind::Internal when the element's window would run past
// rom_data's end; legacy indexed QByteArray::at() unchecked here.
Result<std::int64_t> read_raw_element(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t index);

} // namespace fastecu::calibration
```

- [ ] **Step 4: Write the implementation**

Port `menu_actions.cpp:1610-1697` line by line into `map_edit.cpp`. Three rules for this port:

1. **Replace the union with `std::bit_cast`**, which is the only intentional change. `float` storage assembles its four bytes into a `std::uint32_t` and then `std::bit_cast<float>` — never a union member read.
2. **Preserve the byte-order handling exactly as written**, including filling the intermediate byte array most-significant-first in both endian branches. If that looks wrong, it is — see [the prediction](#a-prediction-this-plan-must-settle-first). Task 3 settles it; this task records it.
3. **Preserve the `int24` gap.** Legacy's signed branch tests only `storagesize == 1`, `2`, and `4`, so a 3-byte signed value falls through every branch and yields an empty `QString`, which callers convert to 0. Reproduce that as an explicit `return 0` with a comment naming it as legacy behavior, not an oversight in the port.

`element_byte_address` carries both predicates:

```cpp
std::uint64_t element_byte_address(const MapElementSpec& spec, std::uint32_t index, bool for_write)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);
    std::uint64_t address = spec.address + std::uint64_t(index) * width;

    // Legacy applies two DIFFERENT wrx02 relocation predicates on the read and
    // write paths. Preserved verbatim and kept visibly side by side; the spec's
    // defect (a) covers the divergence and 6b-4 reconciles them.
    if (spec.flash_method != "wrx02")
    {
        return address;
    }
    constexpr std::uint64_t kSizeThreshold = std::uint64_t(190) * 1024;
    const bool relocate = for_write ? (spec.rom_file_size < kSizeThreshold && address > 0x27FFF)
                                    : (spec.rom_file_size < address);
    return relocate ? address - 0x8000 : address;
}
```

- [ ] **Step 5: Declare the Bazel target**

In `src/backend/calibration/BUILD.bazel`:

```python
cc_library(
    name = "map_edit",
    srcs = ["map_edit.cpp"],
    hdrs = ["map_edit.h"],
    deps = [
        ":scaling_internal",
        "//src/algorithms/expression",
        "//src/algorithms/protocol",
        "//src/backend/definition:definition_model",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "map_edit_test",
    srcs = ["map_edit_test.cpp"],
    deps = [":map_edit"],
)
```

- [ ] **Step 6: Register the target in both closure lists**

In `BUILD.bazel`, add `"//src/backend/calibration:map_edit",` to the `genquery` set beside the existing `//src/backend/calibration:calibration_service` entry (line 93).

In `scripts/check-portable-closure.py:129`, change

```python
    ROOT / "src/backend/calibration": {"calibration_service"},
```

to

```python
    ROOT / "src/backend/calibration": {"calibration_service", "map_edit"},
```

- [ ] **Step 7: Run the tests**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test //:portable_closure`
Expected: PASS.

- [ ] **Step 8: Prove the registration is non-vacuous**

Temporarily add `"@rules_qt//:qt_core"` to `map_edit`'s `deps`, then:

Run: `bazel test --config=release //:portable_closure`
Expected: **FAIL**, naming `map_edit`. Remove the dependency and re-run to confirm PASS.

This is the check 6a-5 hardened against a vacuous scan. A registration that cannot fail is worse than none, because it reads as coverage.

- [ ] **Step 9: Add the remaining codec cases**

Extend `map_edit_test.cpp` with one test per case, all against `read_raw_element`:

- `uint16` / `uint32` big-endian and little-endian.
- `int8` at `0xFF`, expecting `-1`.
- `int16` / `int32` big-endian and little-endian — **assert whatever the implementation actually produces**, and if that is byte-swapped, name the test `PinnedDefect_SignedMultiByteReadsAreByteSwapped` with a comment pointing at the spec.
- `float` storage, big-endian stored, round-tripped through `std::bit_cast`.
- `uint24` reading a 3-byte value correctly.
- `int24` returning 0, named `PinnedDefect_Int24AlwaysReadsAsZero` with a comment naming it as the spec's newly found defect (f).
- `wrx02` relocation: one case where read and write predicates agree, and one 180 KB image with a cell at `0x28000` where they disagree, named `PinnedDefect_Wrx02FixupDiffersBetweenReadAndWrite`, asserting `element_byte_address(spec, i, false) != element_byte_address(spec, i, true)`.

- [ ] **Step 10: Run and commit**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`
Expected: PASS.

```bash
prek run --all-files
git add src/backend/calibration/map_edit.h src/backend/calibration/map_edit.cpp \
        src/backend/calibration/map_edit_test.cpp src/backend/calibration/BUILD.bazel \
        BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat(calibration): extract the map-edit byte read as a portable operation

Ports get_rom_data_value verbatim, including the int24 gap and the wrx02
predicate that differs from the write path's. Both are pinned by tests named
for the defect; 6b-4 fixes them.

Replaces the union type-punning with std::bit_cast -- the one intentional
change, since reading a union member that was never written is UB.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 3: `encode_scaled_value` and the round trip

**Files:**
- Modify: `src/backend/calibration/map_edit.h`, `src/backend/calibration/map_edit.cpp`, `src/backend/calibration/map_edit_test.cpp`

**Interfaces:**
- Consumes: `MapElementSpec`, `read_raw_element`, `element_byte_address` from Task 2; `internal::format_like_qt_g` from Task 1; `expression_evaluate` from `//src/algorithms/expression`.
- Produces: `Result<std::vector<std::uint8_t>> encode_scaled_value(const MapElementSpec& spec, double display_value, int float_precision)`.

**This is the task that settles [the prediction](#a-prediction-this-plan-must-settle-first).** Write the round-trip test before the implementation is finished, and read its failure carefully rather than adjusting the implementation until it passes.

- [ ] **Step 1: Write the failing round-trip test**

```cpp
// The encode/decode round trip is the safety net for every edit operation: if
// encode_scaled_value and read_raw_element disagree about byte order, an edit
// silently writes a different value than the grid displays.
TEST(EncodeScaledValue, RoundTripsThroughReadRawElementForEveryWidth)
{
    struct Case
    {
        definition::StorageType storage;
        std::string_view endian;
        std::int64_t raw;
    };
    const Case cases[] = {
        {definition::StorageType::Uint8, "big", 0xAB},
        {definition::StorageType::Uint16, "big", 0x1234},
        {definition::StorageType::Uint16, "little", 0x1234},
        {definition::StorageType::Uint32, "big", 0x12345678},
        {definition::StorageType::Uint32, "little", 0x12345678},
        {definition::StorageType::Int8, "big", -2},
        {definition::StorageType::Int16, "big", -300},
        {definition::StorageType::Int16, "little", -300},
        {definition::StorageType::Int32, "big", -70000},
    };

    for (const auto& c : cases)
    {
        MapElementSpec spec;
        spec.address = 0x10;
        spec.storage_type = c.storage;
        spec.endian = c.endian;
        spec.to_byte = "x";
        spec.from_byte = "x";

        const auto encoded = encode_scaled_value(spec, double(c.raw), 15);
        ASSERT_TRUE(encoded.has_value()) << to_string(encoded.error().kind);

        std::vector<std::uint8_t> rom(0x40, 0x00);
        std::ranges::copy(*encoded, rom.begin() + 0x10);

        const auto decoded = read_raw_element(rom, spec, 0);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, c.raw) << "storage=" << definition::storage_type_text(c.storage)
                                   << " endian=" << c.endian;
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test --test_output=all`
Expected: FAIL — `encode_scaled_value` is not declared.

- [ ] **Step 3: Write the implementation**

Port the encode half from the three legacy call sites — they share it verbatim (`menu_actions.cpp:335-346`, `:603-614`, `:846-857`):

```cpp
Result<std::vector<std::uint8_t>> encode_scaled_value(const MapElementSpec& spec, double display_value,
                                                     int float_precision)
{
    const std::uint32_t width = definition::storage_byte_size(spec.storage_type);
    const bool is_float = spec.storage_type == definition::StorageType::Float;

    const double encoded =
        expression_evaluate(spec.to_byte, internal::format_like_qt_g(display_value, float_precision), float_precision);

    std::uint32_t raw = 0;
    if (is_float)
    {
        raw = std::bit_cast<std::uint32_t>(float(encoded));
    }
    else
    {
        raw = std::uint32_t(std::llround(encoded));
    }

    std::vector<std::uint8_t> out(width, 0x00);
    const bool little_endian = !is_float && spec.endian == "little";
    for (std::uint32_t k = 0; k < width; ++k)
    {
        const std::uint32_t shift = little_endian ? (8 * k) : (8 * (width - 1 - k));
        out[k] = std::uint8_t((raw >> shift) & 0xFFU);
    }
    return out;
}
```

Note that this writes the byte order the *labels* claim, matching `decode_scaled_values`. Whether that matches what legacy `set_rom_data_value` writes is exactly what Step 5 measures.

- [ ] **Step 4: Run the round-trip test**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test --test_output=all`
Expected: PASS for unsigned and float cases. **The signed multi-byte cases are the ones under test.**

- [ ] **Step 5: Compare against legacy's actual write order**

Add a second test that reproduces `set_rom_data_value`'s loop literally and asserts against `encode_scaled_value`:

```cpp
// set_rom_data_value packs the raw value into a host-native little-endian
// buffer and then indexes it as shown below. This test records what legacy
// actually writes, so any divergence from encode_scaled_value is visible as a
// failing expectation naming both byte sequences rather than as a corrupted
// calibration.
TEST(EncodeScaledValue, MatchesTheLegacyWriteOrder)
{
    MapElementSpec spec;
    spec.storage_type = definition::StorageType::Uint16;
    spec.endian = "big";
    spec.to_byte = "x";

    const auto encoded = encode_scaled_value(spec, 0x1234, 15);
    ASSERT_TRUE(encoded.has_value());

    const auto host_bytes = std::bit_cast<std::array<std::uint8_t, 4>>(std::uint32_t{0x1234});
    std::vector<std::uint8_t> legacy(2);
    for (std::uint32_t k = 0; k < 2; ++k)
    {
        legacy[k] = (spec.endian == "little") ? host_bytes[2 - 1 - k] : host_bytes[k];
    }

    EXPECT_EQ(*encoded, legacy);
}
```

- [ ] **Step 6: Report before proceeding**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test --test_output=all`

**If both tests pass:** the prediction was wrong. Delete [the prediction section](#a-prediction-this-plan-must-settle-first) from this plan, note in the commit that the round trip holds, and continue to Task 4.

**If either fails:** stop. Do not adjust `encode_scaled_value` to match legacy, and do not adjust the test. Record the exact byte sequences from the failure output, rename the legacy-comparison test with a `PinnedDefect_` prefix and an `EXPECT_NE` documenting the divergence, and report to the human partner with: which storage types and endians diverge, and the two byte sequences for each. This finding promotes to a defect in the spec and reorders 6b-4.

- [ ] **Step 7: Commit**

```bash
prek run --all-files
git add src/backend/calibration/map_edit.h src/backend/calibration/map_edit.cpp \
        src/backend/calibration/map_edit_test.cpp
git commit -m "feat(calibration): add encode_scaled_value and pin the encode/decode round trip

encode_scaled_value is the inverse of decode_scaled_values' per-element step,
sharing its formatter so the two cannot drift. The round-trip test across every
storage width and both endians is the safety net for every edit operation built
on top of it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 4: Swap `menu_actions.cpp` onto the codec and delete the dead function

**Files:**
- Create: `src/ui/desktop/calibration/BUILD.bazel`, `src/ui/desktop/calibration/map_edit_adapter.h`, `src/ui/desktop/calibration/map_edit_adapter.cpp`, `src/ui/desktop/calibration/map_edit_adapter_test.cpp`
- Modify: `src/ui/desktop/menu_actions.cpp` (delete `:1610-1753` and `:1805-1850`), `src/ui/desktop/mainwindow.h:356-363`, `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Consumes: `MapElementSpec`, `read_raw_element`, `encode_scaled_value`, `element_byte_address` from Tasks 2-3.
- Produces: `fastecu::ui::MapElementFields` (an owning holder for the plucked `QString` → `std::string` conversions, so the `string_view`s in the `MapElementSpec` it yields stay valid); `MapElementFields collect_map_element_fields(const FileActions::EcuCalDefStructure& def, int map_number, calibration::EditTargetKind kind)`.

Patch application (`apply_patch`) is deliberately **not** here: `EditPatch` does not exist until Task 8, and at this point the edit loops still write cell by cell. It arrives in Task 12.

The adapter owns its strings deliberately. A `MapElementSpec` built from `QString::toStdString()` temporaries would hold dangling views the moment the statement ends — the single most likely way to get this wrong.

- [ ] **Step 1: Write the failing adapter test**

```cpp
#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <QtTest>
#include <gtest/gtest.h>

namespace fastecu::ui
{
namespace
{

FileActions::EcuCalDefStructure two_by_two_def()
{
    FileActions::EcuCalDefStructure def;
    def.NameList << "Timing";
    def.AddressList << "10000";
    def.StorageTypeList << "uint16";
    def.EndianList << "big";
    def.ToByteList << "x";
    def.FromByteList << "x*2";
    def.MinValueList << " ";
    def.MaxValueList << " ";
    def.CoarseIncList << "1.0";
    def.FineIncList << "0.1";
    def.XSizeList << "2";
    def.YSizeList << "2";
    def.FileSize = "196608";
    return def;
}

TEST(MapEditAdapter, PlucksMapBodyFieldsAndKeepsThemAlive)
{
    const auto def = two_by_two_def();

    const auto fields = collect_map_element_fields(def, 0, calibration::EditTargetKind::MapBody);
    const auto spec = fields.spec();

    EXPECT_EQ(spec.address, 0x10000U);
    EXPECT_EQ(spec.storage_type, definition::StorageType::Uint16);
    EXPECT_EQ(spec.endian, "big");
    EXPECT_EQ(spec.from_byte, "x*2");
    EXPECT_DOUBLE_EQ(spec.fine_increment, 0.1);
}

} // namespace
} // namespace fastecu::ui
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/ui/desktop/calibration:map_edit_adapter_test`
Expected: FAIL — package does not exist.

- [ ] **Step 3: Write the adapter**

```cpp
#pragma once

#include <string>

#include "src/backend/calibration/map_edit.h"
#include "src/backend/definitions/ecu_cal_def.h"

namespace fastecu::ui
{

// Owns the std::strings a MapElementSpec's string_views point at. A spec built
// directly from QString::toStdString() temporaries dangles the moment the
// statement ends -- the single easiest way to get this wrong.
class MapElementFields
{
public:
    calibration::MapElementSpec spec() const;

private:
    friend MapElementFields collect_map_element_fields(const FileActions::EcuCalDefStructure&, int,
                                                       calibration::EditTargetKind);
    std::string endian_;
    std::string to_byte_;
    std::string from_byte_;
    std::string min_value_;
    std::string max_value_;
    std::string flash_method_;
    std::uint64_t address_{0};
    std::optional<definition::StorageType> storage_type_;
    double coarse_increment_{0.0};
    double fine_increment_{0.0};
    std::uint32_t x_size_{1};
    std::uint32_t y_size_{1};
    std::uint64_t rom_file_size_{0};
};

// Plucks one element run's fields out of the Qt-typed model. `kind` selects
// between the map-body lists, the XScale* lists, and the YScale* lists -- the
// field-plucking half of what the three duplicated legacy blocks did.
MapElementFields collect_map_element_fields(const FileActions::EcuCalDefStructure& def, int map_number,
                                            calibration::EditTargetKind kind);

} // namespace fastecu::ui
```

`AddressList` entries are hex text; parse with `QString::toUInt(&ok, 16)` and report a bad parse rather than silently yielding 0. `flash_method_` comes from `def.RomInfo.at(FileActions::FlashMethod)` and `rom_file_size_` from `def.FileSize.toUInt()`. A `Rejected` kind is a programming error at this point — the caller resolves before collecting — so assert rather than branch.

- [ ] **Step 4: Write the package BUILD file**

```python
load("//bazel:gtest_targets.bzl", "fastecu_gtest")
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS_NO_WIDGETS", "qt_cc_library")

package(default_visibility = ["//src/ui:__subpackages__"])

# Qt-linked (EcuCalDefStructure is QString/QStringList typed) but declares no
# QObject of its own, so the header goes through normal_hdrs -- matching
# //src/ui/desktop/menu:menu_builder.
qt_cc_library(
    name = "map_edit_adapter",
    srcs = ["map_edit_adapter.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["map_edit_adapter.h"],
    deps = QT_DEPS_NO_WIDGETS + [
        "//src/algorithms/protocol:qt_compat",
        "//src/backend/calibration:map_edit",
        "//src/backend/definitions:ecu_cal_def",
    ],
)

fastecu_gtest(
    name = "map_edit_adapter_test",
    srcs = ["map_edit_adapter_test.cpp"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [":map_edit_adapter"],
)
```

- [ ] **Step 5: Delete the five UI helpers**

From `menu_actions.cpp`, delete `get_rom_data_value` (`:1610-1697`) and `set_rom_data_value` (`:1699-1753`), and delete `check_rom_data_value` (`:1805-1850`) outright — it returns `false` unconditionally and has no caller. Leave `get_mapvalue_decimal_count` and `get_map_cell_colors` for Task 7.

Rewrite the three call sites in `inc_dec_value` and `set_value` to call `read_raw_element` through the adapter, and the four `set_rom_data_value` calls to apply an `encode_scaled_value` result to `FullRomData` via `bytes::mutableView`.

The three duplicated axis-resolution blocks are still in place at this point — Task 6 collapses them. Each of their three branches maps to one `EditTargetKind`, so pass the kind the surrounding branch has already established into `collect_map_element_fields`. Do not attempt the collapse here; keeping this task to the codec swap is what makes its diff reviewable.

From `mainwindow.h`, delete the three matching declarations at `:356-359` and `:362-363`.

- [ ] **Step 6: Wire the dependency**

Add `"//src/ui/desktop/calibration:map_edit_adapter",` to `//src/ui/desktop`'s `deps` in `src/ui/desktop/BUILD.bazel`.

- [ ] **Step 7: Verify the whole build**

Run: `bazel test --config=release //...`
Expected: PASS. This is a PR boundary — every suite must be green, not just the new ones.

- [ ] **Step 8: Verify drain independence**

Run: `git diff --name-only origin/master | grep -E 'src/platform/desktop/common/flash/legacy/|src/ui/desktop/flash/'`
Expected: no output.

- [ ] **Step 9: Commit and open PR 6b-1**

```bash
prek run --all-files
git add -A
git commit -m "refactor(ui,calibration): move the map-edit byte codec out of MainWindow

get_rom_data_value and set_rom_data_value become portable operations reached
through a new //src/ui/desktop/calibration adapter that owns its string
conversions. Deletes check_rom_data_value, which returned false unconditionally
from three empty branches and had no caller.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 5: `resolve_edit_target`

**Files:**
- Modify: `src/backend/calibration/map_edit.h`, `src/backend/calibration/map_edit.cpp`, `src/backend/calibration/map_edit_test.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `struct SelectionRange { int first_row; int first_col; int last_row; int last_col; }`; `struct MapDimensions { std::uint32_t x_size; std::uint32_t y_size; }`; `struct EditTarget { EditTargetKind kind; SelectionRange range; std::uint32_t x_size; }`; `EditTarget resolve_edit_target(const SelectionRange& selection, MapDimensions dims, std::string_view x_scale_type)`. `EditTargetKind` itself was declared in Task 2.

`inc_dec_value:195-259` is the canonical copy — the only one that reads every field the rule governs. `selection` arrives in **widget coordinates**, where row 0 and column 0 are the axis headers; the returned `range` is in **element coordinates**, with the legacy `-1` already applied and each branch's `++` adjustments folded in.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(ResolveEditTarget, LeftColumnSelectionOnAMultiRowMapTargetsTheYAxis)
{
    // Widget column 0 is the Y-axis header column.
    const auto target = resolve_edit_target({.first_row = 1, .first_col = 0, .last_row = 2, .last_col = 0},
                                            {.x_size = 4, .y_size = 4}, "Y Axis");

    EXPECT_EQ(target.kind, EditTargetKind::YAxis);
    // Legacy subtracts 1 from every bound, then adds 1 back to both columns.
    EXPECT_EQ(target.range.first_col, 0);
    EXPECT_EQ(target.range.last_col, 0);
    EXPECT_EQ(target.range.first_row, 0);
    // The Y axis is one element wide regardless of the map's x_size.
    EXPECT_EQ(target.x_size, 1U);
}

TEST(ResolveEditTarget, TopRowSelectionOnAMultiColumnMapTargetsTheXAxis)
{
    const auto target = resolve_edit_target({.first_row = 0, .first_col = 1, .last_row = 0, .last_col = 3},
                                            {.x_size = 4, .y_size = 4}, "X Axis");

    EXPECT_EQ(target.kind, EditTargetKind::XAxis);
    EXPECT_EQ(target.range.first_row, 0);
    EXPECT_EQ(target.x_size, 4U);
}

TEST(ResolveEditTarget, StaticScaleTypesRejectAnAxisEdit)
{
    for (const std::string_view type : {"Static X Axis", "Static Y Axis"})
    {
        const auto target = resolve_edit_target({.first_row = 1, .first_col = 0, .last_row = 2, .last_col = 0},
                                                {.x_size = 4, .y_size = 4}, type);
        EXPECT_EQ(target.kind, EditTargetKind::Rejected) << type;
    }
}

TEST(ResolveEditTarget, SingleColumnMapShiftsRowsBackIntoRange)
{
    // x_size == 1 and a non-static scale type: legacy adds 1 to both rows.
    const auto target = resolve_edit_target({.first_row = 1, .first_col = 1, .last_row = 2, .last_col = 1},
                                            {.x_size = 1, .y_size = 8}, "Y Axis");

    EXPECT_EQ(target.kind, EditTargetKind::MapBody);
    EXPECT_EQ(target.range.first_row, 1);
    EXPECT_EQ(target.range.last_row, 2);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`
Expected: FAIL — `resolve_edit_target` not declared.

- [ ] **Step 3: Implement**

Transcribe `inc_dec_value:195-259`, replacing the three field-swap blocks with the returned `kind` and keeping every offset adjustment and the `map_x_size = 1` override. The static-scale-type early `return` becomes `EditTargetKind::Rejected`.

- [ ] **Step 4: Run to verify it passes**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`
Expected: PASS.

- [ ] **Step 5: Add the remaining cases**

`y_size == 1` shifting columns; a body selection on a map that is neither 1×N nor N×1; a selection at widget row 0 on a map with `x_size == 1` (which falls through to the body branch, because the X-axis branch requires `x_size > 1`); and an empty selection.

- [ ] **Step 6: Commit**

```bash
prek run --all-files
git add src/backend/calibration/map_edit.h src/backend/calibration/map_edit.cpp \
        src/backend/calibration/map_edit_test.cpp
git commit -m "feat(calibration): extract the map/axis edit-target resolution rule

The three-way branch duplicated verbatim across inc_dec_value, set_value and
interpolate_value, extracted once as pure logic. inc_dec_value is canonical: it
is the only copy that reads every field the rule governs.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 6: Collapse the three duplicated blocks in the UI

**Files:**
- Modify: `src/ui/desktop/menu_actions.cpp` (`inc_dec_value:195-259`, `set_value:488-550`, `interpolate_value:689-749`), `src/ui/desktop/calibration/map_edit_adapter.{h,cpp}`

**Interfaces:**
- Consumes: `resolve_edit_target` from Task 5; `collect_map_element_fields` from Task 4.
- Produces: `class ResolvedEdit` with accessors `calibration::MapElementSpec spec() const`, `std::span<const std::string_view> cell_text() const`, `const calibration::SelectionRange& range() const`, `calibration::EditTargetKind kind() const`, `int map_number() const`; and `std::optional<ResolvedEdit> resolve_active_map_edit(QMdiSubWindow *window, const FileActions::EcuCalDefStructure& def)`.

- [ ] **Step 1: Add the bundling helper to the adapter**

```cpp
// Everything an edit operation needs about the active map window, resolved
// once. Owns the MapElementFields and the split cell text so the views handed
// out by spec() and cell_text() stay valid for the caller's whole statement.
class ResolvedEdit
{
public:
    calibration::MapElementSpec spec() const { return fields_.spec(); }
    std::span<const std::string_view> cell_text() const { return cell_text_; }
    const calibration::SelectionRange& range() const { return target_.range; }
    calibration::EditTargetKind kind() const { return target_.kind; }
    int map_number() const { return map_number_; }

private:
    friend std::optional<ResolvedEdit> resolve_active_map_edit(QMdiSubWindow *,
                                                               const FileActions::EcuCalDefStructure&);
    MapElementFields fields_;
    calibration::EditTarget target_;
    std::vector<std::string> owned_cell_text_;
    std::vector<std::string_view> cell_text_;
    int map_number_{0};
};

std::optional<ResolvedEdit> resolve_active_map_edit(QMdiSubWindow *window,
                                                    const FileActions::EcuCalDefStructure& def);
```

`resolve_active_map_edit` reads the subwindow's object name for `(rom_number, map_number)`, finds the `QTableWidget`, converts the first selected range to a widget-coordinate `SelectionRange`, calls `resolve_edit_target`, splits the right one of `MapData` / `XScaleData` / `YScaleData` on `","` into `owned_cell_text_`, and returns `std::nullopt` for a rejected target, an absent window, or an empty selection — the three cases the legacy code handled with a bare `return`.

Note that `spec()` returns by value and its `string_view`s borrow from `fields_`, which the `ResolvedEdit` owns. Callers must keep the `ResolvedEdit` alive across the operation call — which the Task 12 slot shape does naturally, since `edit` is a named local.

- [ ] **Step 2: Rewrite the three call sites**

Each of the three functions loses its ~50-line block, replaced by:

```cpp
auto edit = resolve_active_map_edit(ui->mdiArea->activeSubWindow(), *ecuCalDef[rom_number]);
if (!edit)
{
    return;
}
```

The per-cell loops keep their existing bodies for now; only the resolution changes. Behavior must be identical.

- [ ] **Step 3: Verify no behavior changed**

Run: `bazel test --config=release //...`
Expected: PASS.

- [ ] **Step 4: Confirm the duplication is gone**

Run: `grep -c 'XScaleToByteList' src/ui/desktop/menu_actions.cpp`
Expected: `0` — the field name now appears only in the adapter.

- [ ] **Step 5: Commit**

```bash
prek run --all-files
git add src/ui/desktop/menu_actions.cpp src/ui/desktop/calibration/map_edit_adapter.h \
        src/ui/desktop/calibration/map_edit_adapter.cpp
git commit -m "refactor(ui): collapse three copies of the axis-resolution block onto one rule

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 7: Display helpers

**Files:**
- Modify: `src/backend/calibration/map_edit.{h,cpp}`, `src/backend/calibration/map_edit_test.cpp`, `src/ui/desktop/menu_actions.cpp` (`:1755-1803`), `src/ui/desktop/mainwindow.h:360-361`

**Interfaces:**
- Consumes: nothing.
- Produces: `int map_value_decimal_count(std::string_view value_format)`; `double map_cell_color_scale(double value, double min_value, double max_value)` returning the HSV hue in `[0, 210/360]`, clamped at 0.

`get_map_cell_colors` splits: the hue computation goes portable, the `QColor::setHsvF` / `getRgbF` conversion stays in `menu_actions.cpp`, since packing an RGB int is presentation.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(MapValueDecimalCount, CountsZerosAfterTheDecimalPoint)
{
    EXPECT_EQ(map_value_decimal_count("0.00"), 2);
    EXPECT_EQ(map_value_decimal_count("0.000"), 3);
    EXPECT_EQ(map_value_decimal_count("0"), 0);
    EXPECT_EQ(map_value_decimal_count(""), 0);
}

TEST(MapCellColorScale, MapsTheMinimumToTheTopOfTheHueRange)
{
    // scale_start is 210/360; the legacy formula sends min -> 0 and max ->
    // scale_start.
    EXPECT_DOUBLE_EQ(map_cell_color_scale(0.0, 0.0, 100.0), 0.0);
    EXPECT_DOUBLE_EQ(map_cell_color_scale(100.0, 0.0, 100.0), 210.0 / 360.0);
}

TEST(MapCellColorScale, ClampsBelowTheMinimumToZero)
{
    EXPECT_DOUBLE_EQ(map_cell_color_scale(-50.0, 0.0, 100.0), 0.0);
}
```

- [ ] **Step 2: Run to verify failure, then implement, then run to verify it passes**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`

Transcribe `:1755-1765` and `:1772-1794`. Note `get_map_cell_colors` divides by `(mapMaxValue - mapMinValue)`; when a definition sets them equal that is a division by zero yielding a non-finite hue. Preserve it and add `PinnedDefect_EqualColorBoundsProduceNonFiniteHue` asserting `std::isnan` or `std::isinf` on the result, with a comment. This is a display-only defect; it does not join 6b-4's write-path fixes.

- [ ] **Step 3: Rewire the UI and commit**

`get_map_cell_colors` keeps its name and signature in `menu_actions.cpp` but its body becomes a call to `map_cell_color_scale` plus the `QColor` conversion. `get_mapvalue_decimal_count` is deleted and its two call sites call `map_value_decimal_count`.

Run: `bazel test --config=release //...`
Expected: PASS. This is a PR boundary — open PR 6b-2.

```bash
prek run --all-files
git add -A
git commit -m "refactor(calibration,ui): make the map display helpers portable

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 8: `apply_increment`

**Files:**
- Modify: `src/backend/calibration/map_edit.{h,cpp}`, `src/backend/calibration/map_edit_test.cpp`

**Interfaces:**
- Consumes: `MapElementSpec`, `read_raw_element`, `encode_scaled_value`, `element_byte_address`.
- Produces: `struct CellPatch { std::uint32_t index; std::string display_text; std::uint64_t byte_address; std::vector<std::uint8_t> bytes; }`; `using EditPatch = std::vector<CellPatch>`; `enum class IncrementStep { FineUp, FineDown, CoarseUp, CoarseDown }`; `Result<EditPatch> apply_increment(bytes::ByteView rom_data, const MapElementSpec& spec, std::span<const std::string_view> cell_text, const SelectionRange& range, IncrementStep step, int float_precision)`.

Per the spec's whole-operation failure rule, any per-cell failure fails the call and returns no patch.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(ApplyIncrement, AddsTheCoarseStepToEverySelectedCell)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 10;
    rom[0x11] = 20;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 5.0;
    spec.fine_increment = 1.0;
    spec.x_size = 2;
    spec.y_size = 1;

    const std::string_view cells[] = {"10", "20"};
    const auto patch = apply_increment(rom, spec, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 1},
                                       IncrementStep::CoarseUp, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 2U);
    EXPECT_EQ((*patch)[0].display_text, "15");
    EXPECT_EQ((*patch)[0].bytes, std::vector<std::uint8_t>{15});
    EXPECT_EQ((*patch)[1].display_text, "25");
}

TEST(ApplyIncrement, ClampsToTheDefinitionMaximum)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 250;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 100.0;
    spec.fine_increment = 1.0;
    spec.max_value = "255";
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"250"};
    const auto patch = apply_increment(rom, spec, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                                       IncrementStep::CoarseUp, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ((*patch)[0].display_text, "255");
}

// Spec defect (d), fixed by the extraction: legacy raised a modal inside a
// retry loop that a zero increment could never exit.
TEST(ApplyIncrement, ReportsInvalidConfigWhenTheIncrementIsZero)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.coarse_increment = 0.0;
    spec.fine_increment = 0.0;
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"0"};
    const auto patch = apply_increment(rom, spec, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0},
                                       IncrementStep::CoarseUp, 15);

    ASSERT_FALSE(patch.has_value());
    EXPECT_EQ(patch.error().kind, ErrorKind::InvalidConfig);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`
Expected: FAIL — `apply_increment` not declared.

- [ ] **Step 3: Implement**

Port `inc_dec_value:261-418`, with three changes and no others:

1. The zero-increment check moves **before** the loop and returns `Error{ErrorKind::InvalidConfig, "fine or coarse increment is zero or unset in the definition"}`.
2. The `do { ... } while (rom_data_value == new_rom_data_value)` retry becomes a single pass. With a non-zero increment the legacy loop always exited on its first iteration; the zero case is now rejected up front, so nothing is left for it to retry.
3. Cells are collected into an `EditPatch` instead of written through `set_rom_data_value`.

Keep the min/max clamping and the storage-type saturation and sign-wrap guards (`:347-400`) **exactly as written** — their inconsistency with `set_value` and `paste_value` is the spec's defect (c), fixed in Task 13, not here.

- [ ] **Step 4: Run to verify it passes**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`
Expected: PASS.

- [ ] **Step 5: Add the saturation-guard tests**

One test per guarded storage type asserting that an increment which would overflow leaves the cell at its previous raw value: `uint8` past `0xFF`, `int8` crossing `0x7F`, `int16` crossing `0x7FFF`, and a negative result on unsigned storage.

- [ ] **Step 6: Commit**

```bash
prek run --all-files
git add src/backend/calibration/map_edit.h src/backend/calibration/map_edit.cpp \
        src/backend/calibration/map_edit_test.cpp
git commit -m "feat(calibration): add apply_increment as a patch-returning operation

Fixes the spec's defect (d): the zero-increment warning is now an error
returned once, before the loop, instead of a modal raised inside a retry loop
that a zero increment could never exit.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 9: `apply_set_expression`

**Files:**
- Modify: `src/backend/calibration/map_edit.{h,cpp}`, `src/backend/calibration/map_edit_test.cpp`

**Interfaces:**
- Consumes: everything from Task 8.
- Produces: `Result<EditPatch> apply_set_expression(bytes::ByteView rom_data, const MapElementSpec& spec, std::span<const std::string_view> cell_text, const SelectionRange& range, std::string_view input, int float_precision)`.

`input` is the raw user text from the `QInputDialog`, with commas already replaced by periods by the UI (`set_value:472`). Its grammar is legacy's: a leading `+`, `-`, `*`, or `/` applies that operation to each cell's current value; anything else is parsed as an absolute value assigned to every cell.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(ApplySetExpression, AppliesEachOperatorToEveryCell)
{
    struct Case { std::string_view input; std::string_view expected; };
    const Case cases[] = {
        {"+5", "15"}, {"-5", "5"}, {"*2", "20"}, {"/2", "5"}, {"42", "42"},
    };

    for (const auto& c : cases)
    {
        std::vector<std::uint8_t> rom(0x40, 0x00);
        rom[0x10] = 10;

        MapElementSpec spec;
        spec.address = 0x10;
        spec.storage_type = definition::StorageType::Uint8;
        spec.endian = "big";
        spec.to_byte = "x";
        spec.from_byte = "x";
        spec.x_size = 1;
        spec.y_size = 1;

        const std::string_view cells[] = {"10"};
        const auto patch = apply_set_expression(
            rom, spec, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, c.input, 15);

        ASSERT_TRUE(patch.has_value()) << c.input;
        EXPECT_EQ((*patch)[0].display_text, c.expected) << c.input;
    }
}

TEST(ApplySetExpression, ReportsInvalidConfigOnDivisionByZero)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 10;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"10"};
    const auto patch = apply_set_expression(
        rom, spec, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, "/0", 15);

    ASSERT_FALSE(patch.has_value());
    EXPECT_EQ(patch.error().kind, ErrorKind::InvalidConfig);
}

// Spec defect (c), pinned: set_value clamps to min/max but runs none of the
// storage-type saturation guards apply_increment does. Task 13 fixes this and
// flips the expectation.
TEST(ApplySetExpression, PinnedDefect_DoesNotApplySaturationGuards)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    rom[0x10] = 10;

    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.x_size = 1;
    spec.y_size = 1;

    const std::string_view cells[] = {"10"};
    const auto patch = apply_set_expression(
        rom, spec, cells, {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, "300", 15);

    ASSERT_TRUE(patch.has_value());
    // 300 truncates into a uint8 rather than being rejected as out of range.
    EXPECT_NE((*patch)[0].bytes[0], 10);
}
```

- [ ] **Step 2: Run to verify failure, implement, run to verify it passes**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`

Port `set_value:552-626`. The legacy division-by-zero branch showed a modal and then *continued with the unmodified value*; per the whole-operation failure rule this becomes an `InvalidConfig` error instead. Keep the min/max clamping and the absence of saturation guards.

Legacy parses `+5` as `text.split("+")[1]`, which yields `"5"`. Reproduce that parse rather than inventing a new one — `"+5+3"` splits to `{"", "5", "3"}` and legacy takes `"5"`, silently ignoring the rest.

- [ ] **Step 3: Commit**

```bash
prek run --all-files
git add src/backend/calibration/map_edit.h src/backend/calibration/map_edit.cpp \
        src/backend/calibration/map_edit_test.cpp
git commit -m "feat(calibration): add apply_set_expression

Pins the spec's defect (c): set_value clamps to min/max but runs none of the
saturation guards apply_increment does. Task 13 reconciles them.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 10: `apply_interpolation`

**Files:**
- Modify: `src/backend/calibration/map_edit.{h,cpp}`, `src/backend/calibration/map_edit_test.cpp`

**Interfaces:**
- Consumes: everything from Task 8.
- Produces: `enum class InterpolationMode { Horizontal, Vertical, Bidirectional }`; `Result<EditPatch> apply_interpolation(bytes::ByteView rom_data, const MapElementSpec& spec, std::span<const std::string_view> cell_text, const SelectionRange& range, InterpolationMode mode, int float_precision)`.

This task fixes the spec's defect (e) by construction: the legacy `float cellValue[128][128]` becomes a `std::vector<double>` sized from the selection.

- [ ] **Step 1: Write the failing tests**

```cpp
MapElementSpec linear_uint8_spec(std::uint32_t x_size, std::uint32_t y_size)
{
    MapElementSpec spec;
    spec.address = 0x10;
    spec.storage_type = definition::StorageType::Uint8;
    spec.endian = "big";
    spec.to_byte = "x";
    spec.from_byte = "x";
    spec.x_size = x_size;
    spec.y_size = y_size;
    return spec;
}

TEST(ApplyInterpolation, HorizontalFillsEachRowLinearlyBetweenItsEndpoints)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "99", "99", "30"};

    const auto patch = apply_interpolation(rom, linear_uint8_spec(4, 1), cells,
                                           {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 3},
                                           InterpolationMode::Horizontal, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 4U);
    EXPECT_EQ((*patch)[0].display_text, "0");
    EXPECT_EQ((*patch)[1].display_text, "10");
    EXPECT_EQ((*patch)[2].display_text, "20");
    EXPECT_EQ((*patch)[3].display_text, "30");
}

// Spec defect (e), fixed by the extraction: legacy indexed a fixed
// float[128][128] with the raw selection extent, so any selection wider or
// taller than 128 wrote past the array.
TEST(ApplyInterpolation, HandlesASelectionWiderThanTheLegacyFixedArray)
{
    constexpr std::uint32_t kWidth = 200;
    std::vector<std::uint8_t> rom(0x10 + kWidth, 0x00);
    std::vector<std::string> owned(kWidth, "0");
    owned.front() = "0";
    owned.back() = "199";
    std::vector<std::string_view> cells(owned.begin(), owned.end());

    const auto patch = apply_interpolation(rom, linear_uint8_spec(kWidth, 1), cells,
                                           {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = kWidth - 1},
                                           InterpolationMode::Horizontal, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->size(), kWidth);
    EXPECT_EQ(patch->back().display_text, "199");
}
```

- [ ] **Step 2: Run to verify failure, implement, run to verify it passes**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`

Port `interpolate_value:751-869`, keeping the three modes' formulas exactly — including bidirectional's two-pass structure, which interpolates the left and right edges down the rows first and then each row across. Replace the fixed array with

```cpp
std::vector<double> cell_values(std::size_t(col_count) * row_count, 0.0);
const auto at = [col_count](std::uint32_t col, std::uint32_t row) { return std::size_t(row) * col_count + col; };
```

- [ ] **Step 3: Add vertical and bidirectional tests**

Vertical over a 1×4 selection; bidirectional over a 3×3 selection with all four corners set, asserting the centre cell is the bilinear result.

- [ ] **Step 4: Commit**

```bash
prek run --all-files
git add src/backend/calibration/map_edit.h src/backend/calibration/map_edit.cpp \
        src/backend/calibration/map_edit_test.cpp
git commit -m "feat(calibration): add apply_interpolation over a selection-sized buffer

Fixes the spec's defect (e): interpolate_value indexed a fixed
float[128][128] with the raw selection extent, so a selection wider or taller
than 128 wrote past the array.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 11: `apply_paste`

**Files:**
- Modify: `src/backend/calibration/map_edit.{h,cpp}`, `src/backend/calibration/map_edit_test.cpp`

**Interfaces:**
- Consumes: everything from Task 8.
- Produces: `Result<EditPatch> apply_paste(bytes::ByteView rom_data, const MapElementSpec& spec, std::span<const std::string_view> cell_text, const SelectionRange& range, std::span<const std::vector<std::string_view>> pasted_rows, int float_precision)`.

`pasted_rows` arrives already split by the UI: outer span is rows (clipboard text split on `\n`), inner is columns (split on `\t`).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(ApplyPaste, WritesTheClipboardBlockAnchoredAtTheSelectionCorner)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "0", "0", "0"};
    const std::vector<std::string_view> rows[] = {{"11", "22"}};

    const auto patch = apply_paste(rom, linear_uint8_spec(2, 2), cells,
                                   {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 1}, rows, 15);

    ASSERT_TRUE(patch.has_value());
    ASSERT_EQ(patch->size(), 2U);
    EXPECT_EQ((*patch)[0].display_text, "11");
    EXPECT_EQ((*patch)[1].display_text, "22");
}

TEST(ApplyPaste, DropsCellsThatFallOutsideTheMap)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "0", "0", "0"};
    // Three columns pasted into a two-column map: legacy silently drops the
    // third rather than reporting.
    const std::vector<std::string_view> rows[] = {{"11", "22", "33"}};

    const auto patch = apply_paste(rom, linear_uint8_spec(2, 2), cells,
                                   {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 1}, rows, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ(patch->size(), 2U);
}

// Spec defect (c), pinned: paste applies neither min/max clamping nor
// saturation guards, so arbitrary clipboard text becomes arbitrary ROM bytes.
// Task 13 fixes this and flips the expectation.
TEST(ApplyPaste, PinnedDefect_AppliesNoBoundsCheckingAtAll)
{
    std::vector<std::uint8_t> rom(0x40, 0x00);
    const std::string_view cells[] = {"0", "0", "0", "0"};
    const std::vector<std::string_view> rows[] = {{"9999"}};

    MapElementSpec spec = linear_uint8_spec(2, 2);
    spec.min_value = "0";
    spec.max_value = "255";

    const auto patch = apply_paste(rom, spec, cells,
                                   {.first_row = 0, .first_col = 0, .last_row = 0, .last_col = 0}, rows, 15);

    ASSERT_TRUE(patch.has_value());
    EXPECT_EQ((*patch)[0].display_text, "9999");
}
```

- [ ] **Step 2: Run to verify failure, implement, run to verify it passes**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`

Port `paste_value:976-1004`. Keep the bounds test that drops out-of-map cells (`:981`) and the absence of any value clamping.

- [ ] **Step 3: Commit**

```bash
prek run --all-files
git add src/backend/calibration/map_edit.h src/backend/calibration/map_edit.cpp \
        src/backend/calibration/map_edit_test.cpp
git commit -m "feat(calibration): add apply_paste

Pins the spec's defect (c): paste applies neither clamping nor saturation
guards, so arbitrary clipboard text becomes arbitrary ROM bytes. Task 13 fixes
it. Note that paste also has no axis support -- resolve_edit_target now gives
it one, wired up in Task 12.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 12: Rewire the UI slots and delete the menu shim

**Files:**
- Modify: `src/ui/desktop/menu_actions.cpp` (`:7-142`, `:144-436`, `:438-645`, `:647-887`, `:926-1011`), `src/ui/desktop/mainwindow.h`, `src/ui/desktop/BUILD.bazel`, `src/algorithms/menu/BUILD.bazel`
- Delete: `src/algorithms/menu/qt_menu_command.h`, `src/algorithms/menu/menu_command_qt_compat_test.cpp`

**Interfaces:**
- Consumes: all four `apply_*` operations from Tasks 8-11; `resolve_active_map_edit` and `ResolvedEdit` from Task 6.
- Produces: `void apply_patch(FileActions::EcuCalDefStructure& def, int map_number, calibration::EditTargetKind kind, const calibration::EditPatch& patch)` in the adapter — writes each `CellPatch`'s bytes into `FullRomData` via `bytes::mutableView`, replaces the matching entry of the split cell text, and rejoins into whichever of `MapData` / `XScaleData` / `YScaleData` `kind` names. This is the "write back" half of the three closing statements every legacy edit function shared.

- [ ] **Step 1: Change the slot signatures to take a typed command**

`inc_dec_value(const QString&)` becomes `inc_dec_value(calibration::IncrementStep)`; `interpolate_value(const QString&)` becomes `interpolate_value(calibration::InterpolationMode)`. Update the declarations in `mainwindow.h` and the four dispatch branches in `menu_action_triggered` (`:47-60`), which already has the decoded `MenuCommand` in hand:

```cpp
case MenuCommand::FineIncrement:
    inc_dec_value(calibration::IncrementStep::FineUp);
    break;
case MenuCommand::FineDecrement:
    inc_dec_value(calibration::IncrementStep::FineDown);
    break;
case MenuCommand::CoarseIncrement:
    inc_dec_value(calibration::IncrementStep::CoarseUp);
    break;
case MenuCommand::CoarseDecrement:
    inc_dec_value(calibration::IncrementStep::CoarseDown);
    break;
```

- [ ] **Step 2: Reduce each slot to the five-line shape**

```cpp
void MainWindow::inc_dec_value(calibration::IncrementStep step)
{
    auto edit = resolve_active_map_edit(ui->mdiArea->activeSubWindow(), *ecuCalDef[rom_number]);
    if (!edit)
    {
        return;
    }
    const auto patch = calibration::apply_increment(bytes::view(ecuCalDef[rom_number]->FullRomData), edit->spec(),
                                                    edit->cell_text(), edit->range(), step,
                                                    fileActions->float_precision);
    if (!patch.has_value())
    {
        QMessageBox::warning(this, tr("Set value"), QString::fromStdString(patch.error().detail));
        return;
    }
    apply_patch(*ecuCalDef[rom_number], edit->map_number(), edit->kind(), *patch);
    set_maptablewidget_items();
}
```

Write `apply_patch` in the adapter first — all four slots need it, and it is the only place a `CellPatch`'s `byte_address` reaches `FullRomData`.

`paste_value` gains axis support for free by going through `resolve_active_map_edit`. That is a behavior change: pasting onto a selected axis now edits the axis instead of the map body. Note it in the commit message.

- [ ] **Step 3: Convert the one shim call site**

`menu_action_triggered:9` becomes:

```cpp
switch (menu_command_from_id(action.toStdString()))
```

Then remove `#include "src/algorithms/menu/qt_menu_command.h"` from `menu_actions.cpp:3`.

- [ ] **Step 4: Delete the shim**

```bash
git rm src/algorithms/menu/qt_menu_command.h src/algorithms/menu/menu_command_qt_compat_test.cpp
```

Remove the `qt_compat` and `menu_command_qt_compat_test` targets from `src/algorithms/menu/BUILD.bazel`, and remove `"//src/algorithms/menu:qt_compat",` from `src/ui/desktop/BUILD.bazel:83`.

- [ ] **Step 5: Verify the shim is gone and the build is green**

Run: `bazel query '//src/algorithms/menu:*'`
Expected: `menu_command`, `menu_command_test` — no `qt_compat`, no `menu_command_qt_compat_test`.

Run: `bazel test --config=release //...`
Expected: PASS.

Run: `wc -l src/ui/desktop/menu_actions.cpp`
Expected: under 1400.

Now check the remaining machine-checked completion criteria from the spec:

Run: `grep -nE 'union map_data|FullRomData\[' src/ui/desktop/menu_actions.cpp`
Expected: no output — every union and every raw ROM index is gone from the UI.

Run: `grep -rn check_rom_data_value src/`
Expected: no output.

Run: `bazel query 'somepath(//src/ui/desktop:desktop, //src/algorithms/menu:qt_compat)' 2>&1 | tail -1`
Expected: an error saying the target does not exist — not an empty result, which would also be printed if the query simply found no path.

- [ ] **Step 6: Verify drain independence, then commit and open PR 6b-3**

Run: `git diff --name-only origin/master | grep -E 'src/platform/desktop/common/flash/legacy/|src/ui/desktop/flash/'`
Expected: no output.

```bash
prek run --all-files
git add -A
git commit -m "refactor(ui,algorithms): route map edits through portable operations, drop the menu shim

The four edit slots become resolve -> collect -> call -> apply -> repaint, and
take typed MenuCommand-derived values instead of re-parsing the action string
menu_action_triggered already decoded. That was the sole call site of
//src/algorithms/menu:qt_compat, which is deleted here.

Behavior change: pasting onto a selected axis now edits that axis. paste_value
previously had no axis resolution and wrote into the map body.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 13: Fix defect (c) — uniform bounds enforcement

**Files:**
- Modify: `src/backend/calibration/map_edit.{h,cpp}`, `src/backend/calibration/map_edit_test.cpp`

**Interfaces:**
- Consumes: the four `apply_*` operations.
- Produces: `Result<std::vector<std::uint8_t>> encode_guarded(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t index, double display_value, int float_precision)` — clamp, encode, and apply the saturation and sign-wrap guards in one place.

- [ ] **Step 1: Flip the two pinning tests**

In `ApplySetExpression, PinnedDefect_DoesNotApplySaturationGuards`: rename to `RejectsAValueThatWouldOverflowTheStorageType`, and change the expectation to `ASSERT_FALSE(patch.has_value())` with `EXPECT_EQ(patch.error().kind, ErrorKind::InvalidConfig)`.

In `ApplyPaste, PinnedDefect_AppliesNoBoundsCheckingAtAll`: rename to `ClampsAPastedValueToTheDefinitionMaximum`, and expect `display_text == "255"`.

- [ ] **Step 2: Run to verify the tests now fail**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`
Expected: FAIL on both renamed tests.

- [ ] **Step 3: Extract `encode_guarded` and route all four operations through it**

Lift the clamping and the guard block out of `apply_increment` into `encode_guarded`, then call it from all four operations. `apply_increment`'s behavior must not change — its existing tests are the proof.

- [ ] **Step 4: Run the whole suite**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`
Expected: PASS, including every `apply_increment` test unchanged.

- [ ] **Step 5: Commit**

```bash
prek run --all-files
git add src/backend/calibration/map_edit.h src/backend/calibration/map_edit.cpp \
        src/backend/calibration/map_edit_test.cpp
git commit -m "fix(calibration): apply the same bounds enforcement to every map edit

Spec defect (c). Increment clamped and guarded, set clamped only, and paste
did neither -- so arbitrary clipboard text became arbitrary ROM bytes. All four
operations now encode through one guarded path.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 14: Measure and fix defect (b) — the strided layout

**Files:**
- Modify: `src/backend/calibration/map_edit.{h,cpp}`, `src/backend/calibration/map_edit_test.cpp`, `src/ui/desktop/calibration/map_edit_adapter.cpp`, `docs/tech-debt.md`

**Interfaces:**
- Consumes: `MapElementSpec`.
- Produces: `MapElementSpec` gains `std::uint32_t start_position{1}` and `std::uint32_t interval{1}`.

- [ ] **Step 1: Measure the affected population before changing anything**

The definition corpora live outside this repository. Count maps and axes carrying a non-default `startpos` or `interval`:

```bash
grep -rlE '(startpos|interval)=' ../mmc-definitions/site/xml/ 2>/dev/null | wc -l
grep -rhoE '(startpos|interval)="[^"]*"' ../mmc-definitions/site/xml/ 2>/dev/null | sort | uniq -c | sort -rn | head
```

If the checkout is absent, ask the human partner for a corpus path before proceeding. **Record the counts in the commit message.**

- [ ] **Step 2: Decide from the measurement**

If no definition uses a non-default value, the fix is still correct but unproven against real data — say so in the commit message and land it, since `element_byte_address` diverging from `decode_scaled_values` is a latent trap regardless. If definitions do use it, the counts are the justification.

- [ ] **Step 3: Write the failing test**

```cpp
// Spec defect (b): the edit path used a flat address + index*width layout while
// decode_scaled_values honours start_position and interval, so editing a
// strided map landed on neighbouring data.
TEST(ElementByteAddress, HonoursTheStartPositionAndIntervalStride)
{
    MapElementSpec spec;
    spec.address = 0x100;
    spec.storage_type = definition::StorageType::Uint16;
    spec.start_position = 2;
    spec.interval = 3;

    // addr(j) = 0x100 + (2-1)*2 + j*2*3
    EXPECT_EQ(element_byte_address(spec, 0, false), 0x102U);
    EXPECT_EQ(element_byte_address(spec, 1, false), 0x108U);
    EXPECT_EQ(element_byte_address(spec, 2, false), 0x10EU);
}
```

- [ ] **Step 4: Run to verify failure, implement, run to verify it passes**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`

Add the two fields to `MapElementSpec`, apply the same layout `decode_scaled_values` uses (`calibration_service.cpp:317-332`), including its `start_position == 0` clamp and its `checked_multiply` overflow guards. Populate both fields in `collect_map_element_fields` from `StartPosList` / `IntervalList` and their `XScale*` / `YScale*` variants.

- [ ] **Step 5: Extend the round trip to cover striding**

Add a strided case to `RoundTripsThroughReadRawElementForEveryWidth`, proving encode and decode now agree on layout as well as byte order.

- [ ] **Step 6: Run everything and commit**

Run: `bazel test --config=release //...`
Expected: PASS.

```bash
prek run --all-files
git add -A
git commit -m "fix(calibration): honour startpos and interval on the map-edit path

Spec defect (b). decode_scaled_values lays elements out with the
start_position/interval stride; the edit path used a flat address + index*width,
so editing a strided map wrote onto neighbouring data.

Corpus measurement: replace this line with the actual counts from Step 1 --
how many definition files carry a non-default startpos or interval, and the
distribution of values. Do not commit the sentence unfilled; the measurement
is the justification for changing which ROM byte a write targets.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 15: Measure and fix defect (a) — the `wrx02` relocation

**Files:**
- Modify: `src/backend/calibration/map_edit.{h,cpp}`, `src/backend/calibration/map_edit_test.cpp`

**Interfaces:**
- Consumes: `element_byte_address`.
- Produces: `element_byte_address` loses its `for_write` parameter.

- [ ] **Step 1: Establish which predicate is correct**

The write-side predicate (`rom_file_size < 190*1024 && address > 0x27FFF`) matches `apply_flash_method_padding`'s documented rule (`calibration_service.h:68-80`, `calibration_service.cpp:290-302`): a `wrx02` image under 190 KB has `0x8000` bytes inserted at `0x20000`, so addresses past the insertion point shift by `0x8000`. The read-side predicate (`rom_file_size < address`) has no such correspondence.

Confirm by checking which ROMs declare `wrx02` as their flash method and what `apply_flash_method_padding` does to them. **If the correspondence cannot be confirmed against a real `wrx02` definition, stop and report** — per the spec, an unproven fix here is deferred with the finding recorded, not guessed.

- [ ] **Step 2: Flip the pinning test**

Rename `PinnedDefect_Wrx02FixupDiffersBetweenReadAndWrite` to `AppliesOneWrx02RelocationRuleToReadsAndWrites`, and change `EXPECT_NE` to `EXPECT_EQ`.

- [ ] **Step 3: Run to verify failure, implement, run to verify it passes**

Run: `bazel test --config=release //src/backend/calibration:map_edit_test`

Drop the `for_write` parameter and keep only the padding-consistent predicate. Update the three call sites.

- [ ] **Step 4: Run everything and commit**

Run: `bazel test --config=release //...`
Expected: PASS.

```bash
prek run --all-files
git add -A
git commit -m "fix(calibration): apply one wrx02 relocation rule to reads and writes

Spec defect (a). get_rom_data_value tested rom_file_size < address while
set_rom_data_value tested rom_file_size < 190*1024 && address > 0x27FFF, so a
180 KB image read a cell from 0x28000 and wrote the edit to 0x20000. Keeps the
write-side predicate, the one that matches apply_flash_method_padding.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```

---

## Task 16: Close out the step in the documentation

**Files:**
- Modify: `docs/modularization-plan.md`, `docs/tech-debt.md`, `docs/superpowers/specs/2026-08-08-step5-tail-flash-drain-design.md`, `docs/superpowers/specs/2026-09-03-step6b-calibration-map-edit-design.md`

- [ ] **Step 1: Amend the flash-drain design's completion criteria**

Remove `//src/algorithms/menu:qt_compat` from the list of shims the drain deletes (`:33`), replacing it with a sentence recording that step 6b deleted it, because its only consumer was `menu_actions.cpp` and the drain never owned that file.

- [ ] **Step 2: Update the modularization plan**

Under "6. Finish the thin desktop shell", add a `6b` entry mirroring 6a's: what shipped, the four PRs, the guard state, and the explicit non-goal that `EcuCalDefStructure` remains Qt-typed. Update the Status section's step-6 line and the `:qt_compat` shim count in "Verified Current Baseline" — three shims become two (`protocol`, `protocol/ssm`).

- [ ] **Step 3: Update the tech-debt roadmap**

Under "P1: Separate UI from application logic", strike the completed action "Move calibration/map commands out of `menu_actions.cpp` into headless model operations" and update the `menu_actions.cpp` line count. Add any defect deferred by Task 14 or 15 as a new action with its measurement recorded. Under "Coverage growth sequence", mark "Calibration-map edit, interpolation, undo/redo, and bounds behavior without widgets" as covered except undo/redo.

- [ ] **Step 4: Update the 6b spec**

Add the defect subsections discovered during implementation — at minimum `(f) int24 always reads as zero`, and whatever Task 3's round trip established — so the spec remains the argued record rather than a snapshot of what was known before the code was read.

- [ ] **Step 5: Verify links and commit, opening PR 6b-4**

Run: `prek run --all-files`
Expected: PASS, including lychee.

Run: `bazel test --config=release //...`
Expected: PASS.

```bash
git add docs/
git commit -m "docs: record step 6b complete and settle the menu-shim ownership

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01WmoEf2w7mX6cBkzTGjC75J"
```
