# Native Protocol and Vehicle Catalog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `resources/shared/config/protocols.cfg` with a compile-time protocol and vehicle catalog in `src/backend/config`, passed to every consumer as a value, with the file's data defects fixed and every protocol reachable.

**Architecture:** A portable `catalog` target defines `ProtocolSpec`, `VehicleSpec` and a `Catalog` view with lookups and a consistency check. `builtin_catalog` holds the data, generated once from the file and then fixed commit by commit while a temporary parity test documents each difference. `ConfigSession` receives a `const Catalog&`; flash and EEPROM requests carry the selected `ProtocolSpec` instead of re-reading a file. The saved selection becomes a stable `vehicle_id`, a startup gate asks for a vehicle when none is saved, and the file, its loaders and its pinning tests are deleted last.

**Tech Stack:** C++23, Bazel (Gazelle-managed BUILD files), GoogleTest/gmock, pugixml (parity test only), Qt 6 in the UI and platform layers.

**Spec:** [native protocol catalog design](../specs/2026-10-04-native-protocol-catalog-design.md), including its "Amendments from planning" section. Read both before starting.

## Global Constraints

- Work on branch `feat/native-protocol-catalog`. The branch lands as one pull request; intermediate commits are reviewable but are not release candidates (between Task 7 and Task 8 the session reads the catalog while flash still reads `protocols.cfg`).
- C++23. `bazel test --config=release //...` passes at the end of every task.
- `src/backend/**` and `src/algorithms/**` stay portable: no Qt, no threads, no filesystem. Run `scripts/android-cross-compile.sh` when `ANDROID_NDK_HOME` is set; otherwise run its reachability query: `bazel query "filter('^//src/platform/', deps(//src/backend/... + //src/algorithms/...))"` must print nothing.
- Warnings are errors. Never add `#pragma` suppressions, `-Wno-*`, `/constexpr:steps` or `-treat_warnings_as_errors` to committed files.
- BUILD files are Gazelle-managed: after adding, renaming or deleting sources run `python3 scripts/gazelle_check.py --fix`, review the diff, and keep hand-written attributes (`data`, `env`, `qt`, `visibility`, `size`). No new whole-rule `# keep`.
- Visibility uses the groups in `//bazel/layers`; a one-off package literal carries a comment saying why.
- Backend operations return `fastecu::Result<T>` / `Status`, checked with `.has_value()`; the `ErrorKind` set is closed.
- Constexpr variables are `kCamelCase`; follow the surrounding file's naming otherwise (`snake_case` functions in `src/backend`, `camelCase` helpers in `flash_workflow.cpp` and its test).
- Tests are GoogleTest, co-located with their package (`fastecu_portable_gtest` for portable packages); cross-package checks live in `tests/`.
- Markdown cross-document references are links with readable text.
- No ECU I/O in any test. Never relax an address-window guard or a plan validation; the only plan edits allowed are removing the MC68 `_04` names (Task 5) and, if Task 9 finds one, aligning a rejection's error kind to `Unsupported`.
- `prek run --all-files` passes before each commit. End each commit message with the attribution lines your session's system reminder specifies.

## Review Focus

1. A stale `protocols.cfg` left in a config directory is ignored: never read, never an error (pinned in Task 7).
2. Upgrading from a version whose `fastecu.cfg` carries only `protocol_id` yields no selection and one prompt, not a wrong vehicle (pinned in Task 10).
3. After the first-launch choice is saved, an in-app restart (`RESTART_CODE`) does not prompt again (pinned in Task 10).
4. Cancelling the startup prompt exits with code 0 before `MainWindow` or any ECU I/O exists (pinned in Task 10).
5. Selecting the newly offered MUT/DMA vehicle sets log protocol `MUT_DMA` and offers no flash operation (pinned in Task 7).

---

## File map

| File | Responsibility | Task |
| --- | --- | --- |
| `src/backend/config/catalog.{h,cpp}` | `ProtocolSpec`, `VehicleSpec`, `ChecksumSupport`, `Catalog` lookups, consistency checks | 1 |
| `src/backend/config/catalog_test.cpp` | Lookup and consistency behavior on synthetic catalogs | 1 |
| `src/backend/config/builtin_catalog.{h,cpp}` | The shipped data and `builtin_catalog()` | 2–5 |
| `src/backend/config/builtin_catalog_test.cpp` | Known-defect ratchet, then one pin per data fix | 2–5 |
| `src/backend/config/builtin_catalog_parity_test.cpp` | Temporary: catalog vs. `protocols.cfg`, differences = documented fixes | 2–5, deleted in 11 |
| `src/backend/checksum/dispatch.{h,cpp}` | New `has_route()` | 6 |
| `tests/catalog_consistency_test.cpp` | Checksum flags vs. routes, kernel names vs. bundle, MCUs vs. flash models | 6 |
| `src/backend/config/config_session.{h,cpp}`, `testing/config_session_fixture.h` | Session over a `Catalog` | 7, 10 |
| `src/backend/calibration/session/rom_open.cpp` | Alias and checksum lookups through the session | 7 |
| `src/ui/desktop/config_fields.{h,cpp}` and UI consumers | Display helpers over `VehicleSpec` | 7 |
| `src/platform/desktop/common/flash/flash_workflow.{h,cpp}` | Request carries a `ProtocolSpec` | 8 |
| `src/backend/flash/eeprom/eeprom_read_plan.{h,cpp}` | Plan from a `ProtocolSpec` | 8 |
| `src/platform/desktop/common/flash/builtin_catalog_capability_test.cpp` | Capabilities agree with the workflows | 9 |
| `src/backend/config/app_config.{h,cpp}` | `vehicle_id` replaces `protocol_id` | 10 |
| `apps/desktop/startup_vehicle_gate.{h,cpp}` | Ask for a vehicle before `MainWindow` | 10 |
| `docs/adr/0019-compile-the-protocol-catalog-into-the-backend.md` | The decision record | 11 |

---

### Task 1: Catalog types, lookups and consistency checks

**Files:**
- Create: `src/backend/config/catalog.h`
- Create: `src/backend/config/catalog.cpp`
- Create: `src/backend/config/catalog_test.cpp`
- Modify: `src/backend/config/BUILD.bazel` (Gazelle output plus one `visibility`)

**Interfaces:**
- Consumes: nothing.
- Produces (namespace `fastecu::config`, header `src/backend/config/catalog.h`, target `//src/backend/config:catalog`):
  - `enum class ChecksumSupport { Corrected, Missing, None };`
  - `std::string_view checksum_flag(ChecksumSupport)` → `"yes"`, `"n/a"`, `"no"`
  - `struct ProtocolSpec { std::string_view name, alias, ecu, mcu, mode; ChecksumSupport checksum; bool read, test_write, write; std::string_view flash_transport, log_transport, log_protocol, kernel; std::optional<std::uint32_t> kernel_load_address; std::string_view description; }` (members in exactly this order; defaulted `operator==`)
  - `std::string kernel_load_address_text(const ProtocolSpec&)` → `"0xFFFF3000"` or `""`
  - `struct VehicleSpec { std::string_view id, make, model, version, type, kw, hp, fuel, year; const ProtocolSpec *protocol; }`
  - `consteval const ProtocolSpec *protocol_in(std::span<const ProtocolSpec>, std::string_view name)`
  - `constexpr bool catalog_references_resolve(std::span<const ProtocolSpec>, std::span<const VehicleSpec>)`
  - `class Catalog` with `constexpr Catalog(std::span<const ProtocolSpec>, std::span<const VehicleSpec>)`, `protocols()`, `vehicles()`, `const ProtocolSpec *find_protocol(std::string_view) const`, `std::optional<std::size_t> find_vehicle(std::string_view id) const`, `std::optional<std::size_t> last_vehicle_for_protocol(std::string_view) const`, `const VehicleSpec *first_vehicle_for_alias(std::string_view) const`
  - `std::vector<std::string> catalog_problems(const Catalog&)`

- [ ] **Step 1: Write the failing test**

Create `src/backend/config/catalog_test.cpp`:

```cpp
#include "src/backend/config/catalog.h"

#include <array>
#include <cstddef>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::config::Catalog;
using fastecu::config::catalog_problems;
using fastecu::config::catalog_references_resolve;
using fastecu::config::checksum_flag;
using fastecu::config::ChecksumSupport;
using fastecu::config::kernel_load_address_text;
using fastecu::config::protocol_in;
using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

constexpr auto kProtocols = std::to_array<ProtocolSpec>({
    {.name = "proto_a",
     .alias = "alias_a",
     .mcu = "SH7058",
     .checksum = ChecksumSupport::Corrected,
     .read = true,
     .kernel = "a.bin",
     .kernel_load_address = 0xFFFF3000U},
    {.name = "proto_b", .alias = "shared", .mcu = "M32R"},
    {.name = "proto_c", .alias = "shared", .mcu = "SH7055"},
});

// Rows 0 and 2 share proto_a. "shared" is the alias of proto_b and proto_c;
// proto_c's vehicle (row 1) comes before proto_b's (row 3).
constexpr auto kVehicles = std::to_array<VehicleSpec>({
    {.id = "subaru-impreza", .make = "Subaru", .model = "Impreza", .protocol = protocol_in(kProtocols, "proto_a")},
    {.id = "mitsubishi-colt", .make = "Mitsubishi", .model = "Colt", .protocol = protocol_in(kProtocols, "proto_c")},
    {.id = "subaru-forester", .make = "Subaru", .model = "Forester", .protocol = protocol_in(kProtocols, "proto_a")},
    {.id = "subaru-legacy", .make = "Subaru", .model = "Legacy", .protocol = protocol_in(kProtocols, "proto_b")},
});

constexpr Catalog kCatalog{kProtocols, kVehicles};

static_assert(protocol_in(kProtocols, "proto_b") == &kProtocols[1]);
static_assert(protocol_in(kProtocols, "absent") == nullptr);
static_assert(catalog_references_resolve(kProtocols, kVehicles));

TEST(ChecksumFlag, SpellsTheLegacyFlagText)
{
    EXPECT_EQ(checksum_flag(ChecksumSupport::Corrected), "yes");
    EXPECT_EQ(checksum_flag(ChecksumSupport::Missing), "n/a");
    EXPECT_EQ(checksum_flag(ChecksumSupport::None), "no");
}

TEST(KernelLoadAddressText, IsUnpaddedUppercaseHexOrEmpty)
{
    EXPECT_EQ(kernel_load_address_text(kProtocols[0]), "0xFFFF3000");
    EXPECT_EQ(kernel_load_address_text(ProtocolSpec{.kernel_load_address = 0x20000U}), "0x20000");
    EXPECT_EQ(kernel_load_address_text(kProtocols[1]), "");
}

TEST(Catalog, FindsAProtocolByName)
{
    EXPECT_EQ(kCatalog.find_protocol("proto_c"), &kProtocols[2]);
    EXPECT_EQ(kCatalog.find_protocol("absent"), nullptr);
}

TEST(Catalog, FindsAVehicleRowById)
{
    EXPECT_EQ(kCatalog.find_vehicle("subaru-forester"), std::optional<std::size_t>(2));
    EXPECT_EQ(kCatalog.find_vehicle("absent"), std::nullopt);
    EXPECT_EQ(kCatalog.find_vehicle(""), std::nullopt);
}

TEST(Catalog, ProtocolLookupTakesTheLastMatchingRow)
{
    EXPECT_EQ(kCatalog.last_vehicle_for_protocol("proto_a"), std::optional<std::size_t>(2));
    EXPECT_EQ(kCatalog.last_vehicle_for_protocol("proto_b"), std::optional<std::size_t>(3));
    EXPECT_EQ(kCatalog.last_vehicle_for_protocol("absent"), std::nullopt);
}

TEST(Catalog, AliasLookupTakesTheFirstMatchingVehicle)
{
    EXPECT_EQ(kCatalog.first_vehicle_for_alias("shared"), &kVehicles[1]);
    EXPECT_EQ(kCatalog.first_vehicle_for_alias("alias_a"), &kVehicles[0]);
    EXPECT_EQ(kCatalog.first_vehicle_for_alias("absent"), nullptr);
}

TEST(Catalog, AnEmptyFlashMethodMatchesNoAlias)
{
    static constexpr auto kUnaliased = std::to_array<ProtocolSpec>({{.name = "plain"}});
    static constexpr auto kOne =
        std::to_array<VehicleSpec>({{.id = "one", .protocol = protocol_in(kUnaliased, "plain")}});
    EXPECT_EQ((Catalog{kUnaliased, kOne}.first_vehicle_for_alias("")), nullptr);
}

TEST(CatalogProblems, AConsistentCatalogHasNone)
{
    EXPECT_THAT(catalog_problems(kCatalog), IsEmpty());
}

constexpr auto kBrokenProtocols = std::to_array<ProtocolSpec>({
    {.name = "dup"},
    {.name = "dup"},
    {.name = ""},
    {.name = "lonely", .alias = "a,b", .kernel_load_address = 0x1000U},
});
constexpr auto kForeignProtocols = std::to_array<ProtocolSpec>({{.name = "foreign"}});
constexpr auto kBrokenVehicles = std::to_array<VehicleSpec>({
    {.id = "Upper", .protocol = &kBrokenProtocols[0]},
    {.id = "twin", .protocol = &kBrokenProtocols[1]},
    {.id = "twin", .protocol = &kBrokenProtocols[2]},
    {.id = "orphan"},
    {.id = "stranger", .protocol = &kForeignProtocols[0]},
});

TEST(CatalogProblems, ReportsEachInconsistencyOnce)
{
    EXPECT_FALSE(catalog_references_resolve(kBrokenProtocols, kBrokenVehicles));
    EXPECT_THAT(catalog_problems(Catalog{kBrokenProtocols, kBrokenVehicles}),
                UnorderedElementsAre("duplicate protocol name 'dup'", "protocol name is empty",
                                     "protocol 'lonely' alias 'a,b' contains ','",
                                     "protocol 'lonely' has a kernel load address but no kernel",
                                     "protocol 'lonely' has no vehicle",
                                     "vehicle id 'Upper' is not lowercase [a-z0-9-]",
                                     "duplicate vehicle id 'twin'", "vehicle 'orphan' has no protocol",
                                     "vehicle 'stranger' protocol is not in this catalog"));
}

} // namespace
```

- [ ] **Step 2: Run Gazelle and the test to verify it fails**

Run: `python3 scripts/gazelle_check.py --fix && bazel test --config=release //src/backend/config:catalog_test`
Expected: FAIL — the build cannot find `src/backend/config/catalog.h`.

- [ ] **Step 3: Write the header**

Create `src/backend/config/catalog.h`:

```cpp
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastecu::config
{

// How a protocol's ROM checksum is handled before a write. protocols.cfg
// spelled these "yes", "n/a" and "no".
enum class ChecksumSupport
{
    Corrected, // a checksum module corrects the image
    Missing,   // a module should exist but does not: warn before writing
    None,      // the family has no checksum: write without a warning
};

// The legacy flag text ChecksumSelection still takes: "yes", "n/a" or "no".
std::string_view checksum_flag(ChecksumSupport support);

// One flash and logging protocol, as compile-time data.
struct ProtocolSpec
{
    std::string_view name;  // unique within a catalog
    std::string_view alias; // one token a definition's flash method may use; empty when none
    std::string_view ecu;
    std::string_view mcu;
    std::string_view mode;
    ChecksumSupport checksum = ChecksumSupport::None;
    bool read = false;
    bool test_write = false;
    bool write = false;
    std::string_view flash_transport; // comma-separated; the UI splits it
    std::string_view log_transport;   // comma-separated; the UI splits it
    std::string_view log_protocol;
    std::string_view kernel; // a file in the kernel directory; empty when none is uploaded
    std::optional<std::uint32_t> kernel_load_address;
    std::string_view description;

    bool operator==(const ProtocolSpec&) const = default;
};

// The load address as protocols.cfg spelled it -- "0x" then unpadded
// uppercase hex, e.g. "0xFFFF3000" -- or "" when there is none.
std::string kernel_load_address_text(const ProtocolSpec& protocol);

// One vehicle the operator can select.
struct VehicleSpec
{
    std::string_view id; // saved in fastecu.cfg: never changed, never reused
    std::string_view make;
    std::string_view model;
    std::string_view version;
    std::string_view type;
    std::string_view kw;
    std::string_view hp;
    std::string_view fuel;
    std::string_view year;
    // Into the same catalog's protocols; never null in a consistent catalog.
    const ProtocolSpec *protocol = nullptr;
};

// The protocol named `name`, for wiring VehicleSpec::protocol in constant
// data; nullptr when `protocols` has none, which catalog_references_resolve
// then rejects.
consteval const ProtocolSpec *protocol_in(std::span<const ProtocolSpec> protocols, std::string_view name)
{
    const auto found = std::ranges::find(protocols, name, &ProtocolSpec::name);
    return found == protocols.end() ? nullptr : &*found;
}

// The checks cheap enough to static_assert on every supported compiler:
// every vehicle has a protocol, every protocol has a vehicle, and no protocol
// has a kernel load address without a kernel. catalog_problems() checks
// these and the rest.
constexpr bool catalog_references_resolve(std::span<const ProtocolSpec> protocols,
                                          std::span<const VehicleSpec> vehicles)
{
    const auto has_protocol = [](const VehicleSpec& vehicle) { return vehicle.protocol != nullptr; };
    const auto reachable = [vehicles](const ProtocolSpec& protocol)
    {
        return std::ranges::any_of(vehicles,
                                   [&protocol](const VehicleSpec& vehicle) { return vehicle.protocol == &protocol; });
    };
    const auto kernel_named = [](const ProtocolSpec& protocol)
    { return !protocol.kernel_load_address.has_value() || !protocol.kernel.empty(); };
    return std::ranges::all_of(vehicles, has_protocol) && std::ranges::all_of(protocols, reachable) &&
           std::ranges::all_of(protocols, kernel_named);
}

// A protocol and vehicle table the application runs against. Holds views:
// the arrays it is built from must outlive it.
class Catalog
{
  public:
    constexpr Catalog(std::span<const ProtocolSpec> protocols, std::span<const VehicleSpec> vehicles) noexcept
        : protocols_(protocols), vehicles_(vehicles)
    {
    }

    constexpr std::span<const ProtocolSpec> protocols() const noexcept
    {
        return protocols_;
    }
    // Presentation order; a vehicle's row is its position.
    constexpr std::span<const VehicleSpec> vehicles() const noexcept
    {
        return vehicles_;
    }

    // nullptr when no protocol is named `name`.
    const ProtocolSpec *find_protocol(std::string_view name) const;
    // The row of the vehicle whose id is `id`.
    std::optional<std::size_t> find_vehicle(std::string_view id) const;
    // The LAST row whose protocol is named `protocol_name`: the rule ROM open
    // has always used to select a vehicle from a flash method.
    std::optional<std::size_t> last_vehicle_for_protocol(std::string_view protocol_name) const;
    // The FIRST vehicle whose protocol's alias is `flash_method`: the rule a
    // definition's flash method has always been resolved by. An empty
    // `flash_method` matches nothing.
    const VehicleSpec *first_vehicle_for_alias(std::string_view flash_method) const;

  private:
    std::span<const ProtocolSpec> protocols_;
    std::span<const VehicleSpec> vehicles_;
};

// Every way `catalog` is inconsistent, one message each; empty when it is
// consistent. Covers catalog_references_resolve, plus duplicate protocol
// names and vehicle ids, vehicle id spelling, comma-separated aliases, and
// vehicles whose protocol belongs to another catalog.
std::vector<std::string> catalog_problems(const Catalog& catalog);

} // namespace fastecu::config
```

- [ ] **Step 4: Write the implementation**

Create `src/backend/config/catalog.cpp`:

```cpp
#include "src/backend/config/catalog.h"

#include <format>
#include <utility>

namespace fastecu::config
{
namespace
{

bool is_vehicle_id(std::string_view id)
{
    return !id.empty() &&
           std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

void add_protocol_problems(const Catalog& catalog, std::vector<std::string>& problems)
{
    const std::span<const ProtocolSpec> protocols = catalog.protocols();
    for (std::size_t index = 0; index < protocols.size(); ++index)
    {
        const ProtocolSpec& protocol = protocols[index];
        const std::span<const ProtocolSpec> earlier = protocols.first(index);
        if (protocol.name.empty())
        {
            problems.emplace_back("protocol name is empty");
        }
        else if (std::ranges::find(earlier, protocol.name, &ProtocolSpec::name) != earlier.end())
        {
            problems.push_back(std::format("duplicate protocol name '{}'", protocol.name));
        }
        if (protocol.alias.contains(','))
        {
            problems.push_back(std::format("protocol '{}' alias '{}' contains ','", protocol.name, protocol.alias));
        }
        if (protocol.kernel_load_address.has_value() && protocol.kernel.empty())
        {
            problems.push_back(std::format("protocol '{}' has a kernel load address but no kernel", protocol.name));
        }
        if (std::ranges::none_of(catalog.vehicles(),
                                 [&protocol](const VehicleSpec& vehicle) { return vehicle.protocol == &protocol; }))
        {
            problems.push_back(std::format("protocol '{}' has no vehicle", protocol.name));
        }
    }
}

void add_vehicle_problems(const Catalog& catalog, std::vector<std::string>& problems)
{
    const std::span<const VehicleSpec> vehicles = catalog.vehicles();
    for (std::size_t index = 0; index < vehicles.size(); ++index)
    {
        const VehicleSpec& vehicle = vehicles[index];
        const std::span<const VehicleSpec> earlier = vehicles.first(index);
        if (!is_vehicle_id(vehicle.id))
        {
            problems.push_back(std::format("vehicle id '{}' is not lowercase [a-z0-9-]", vehicle.id));
        }
        else if (std::ranges::find(earlier, vehicle.id, &VehicleSpec::id) != earlier.end())
        {
            problems.push_back(std::format("duplicate vehicle id '{}'", vehicle.id));
        }
        if (vehicle.protocol == nullptr)
        {
            problems.push_back(std::format("vehicle '{}' has no protocol", vehicle.id));
        }
        else if (std::ranges::none_of(catalog.protocols(), [&vehicle](const ProtocolSpec& protocol)
                                      { return &protocol == vehicle.protocol; }))
        {
            problems.push_back(std::format("vehicle '{}' protocol is not in this catalog", vehicle.id));
        }
    }
}

} // namespace

std::string_view checksum_flag(ChecksumSupport support)
{
    switch (support)
    {
    case ChecksumSupport::Corrected:
        return "yes";
    case ChecksumSupport::Missing:
        return "n/a";
    case ChecksumSupport::None:
        return "no";
    }
    std::unreachable();
}

std::string kernel_load_address_text(const ProtocolSpec& protocol)
{
    return protocol.kernel_load_address.has_value() ? std::format("0x{:X}", *protocol.kernel_load_address)
                                                    : std::string{};
}

const ProtocolSpec *Catalog::find_protocol(std::string_view name) const
{
    const auto found = std::ranges::find(protocols_, name, &ProtocolSpec::name);
    return found == protocols_.end() ? nullptr : &*found;
}

std::optional<std::size_t> Catalog::find_vehicle(std::string_view id) const
{
    const auto found = std::ranges::find(vehicles_, id, &VehicleSpec::id);
    if (id.empty() || found == vehicles_.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::ranges::distance(vehicles_.begin(), found));
}

std::optional<std::size_t> Catalog::last_vehicle_for_protocol(std::string_view protocol_name) const
{
    std::optional<std::size_t> last;
    for (std::size_t row = 0; row < vehicles_.size(); ++row)
    {
        if (vehicles_[row].protocol != nullptr && vehicles_[row].protocol->name == protocol_name)
        {
            last = row;
        }
    }
    return last;
}

const VehicleSpec *Catalog::first_vehicle_for_alias(std::string_view flash_method) const
{
    if (flash_method.empty())
    {
        return nullptr;
    }
    const auto found = std::ranges::find_if(vehicles_, [flash_method](const VehicleSpec& vehicle)
                                            { return vehicle.protocol != nullptr && vehicle.protocol->alias == flash_method; });
    return found == vehicles_.end() ? nullptr : &*found;
}

std::vector<std::string> catalog_problems(const Catalog& catalog)
{
    std::vector<std::string> problems;
    add_protocol_problems(catalog, problems);
    add_vehicle_problems(catalog, problems);
    return problems;
}

} // namespace fastecu::config
```

This design was compiled and run against the real `protocols.cfg` data with clang `-Wall -Wextra -Werror` during planning; the broken-catalog messages above are its actual output.

- [ ] **Step 5: Regenerate BUILD and set the library's visibility**

Run: `python3 scripts/gazelle_check.py --fix`

Gazelle adds a `cc_library(name = "catalog")` and a `fastecu_portable_gtest(name = "catalog_test")`. Add this `visibility` to the library, matching `config_session` (the composition root will construct a session from a catalog):

```starlark
cc_library(
    name = "catalog",
    srcs = ["catalog.cpp"],
    hdrs = ["catalog.h"],
    # The package default plus the composition root, which hands a catalog
    # to the configuration session.
    visibility = [
        "//bazel/layers:apps_desktop",
        "//bazel/layers:backend_and_above",
    ],
)
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/config:catalog_test`
Expected: PASS (10 tests).

- [ ] **Step 7: Commit**

```bash
prek run --files src/backend/config/catalog.h src/backend/config/catalog.cpp src/backend/config/catalog_test.cpp src/backend/config/BUILD.bazel
git add src/backend/config/catalog.h src/backend/config/catalog.cpp src/backend/config/catalog_test.cpp src/backend/config/BUILD.bazel
git commit -m "feat(config): add the compile-time protocol and vehicle catalog types"
```

---

### Task 2: The built-in catalog, generated faithfully from protocols.cfg

**Files:**
- Create: `src/backend/config/builtin_catalog.h`
- Create: `src/backend/config/builtin_catalog.cpp`
- Create: `src/backend/config/builtin_catalog_test.cpp`
- Create: `src/backend/config/builtin_catalog_parity_test.cpp` (temporary; deleted in Task 11)
- Modify: `src/backend/config/BUILD.bazel`

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces: `const Catalog& fastecu::config::builtin_catalog();` in `src/backend/config/builtin_catalog.h`, target `//src/backend/config:builtin_catalog`, visible to `//bazel/layers:apps_desktop` and `//bazel/layers:tests` (later tasks add two test packages). Inside `builtin_catalog.cpp`, anonymous-namespace arrays `kProtocols` and `kVehicles` — later tasks edit entries in them.

- [ ] **Step 1: Save the one-off generator outside the repository**

Write this to a scratch location outside the repository (for example `$TMPDIR/generate_catalog.py`). It is never committed.

```python
#!/usr/bin/env python3
"""One-off: print the C++ catalog arrays for a protocols.cfg-shaped XML file.

Usage: generate_catalog.py <file.xml> [Prefix]

Prints k<Prefix>Protocols and k<Prefix>Vehicles (default kProtocols/kVehicles).

Faithful: every <protocol> and <car_model> in file order, the six unused
ecu_id_*/cal_id_* fields dropped, nothing else changed. A car model whose
protocol is absent gets protocol_in(...) like any other; protocol_in yields
nullptr for it. Vehicle ids follow the spec's rule, computed from the values
the data fixes will produce, so they never change after this run.
"""
import re
import sys
import xml.etree.ElementTree as ET

CHECKSUM = {"yes": "ChecksumSupport::Corrected", "n/a": "ChecksumSupport::Missing", "no": "ChecksumSupport::None"}
RENAMED = {"sub_ecu_unisia_jecs_92": "sub_ecu_unisia_jecs_m3779x", "sub_ecu_unisia_jecs_97": "sub_ecu_unisia_jecs_m3775x"}


def text(node, tag):
    value = node.findtext(tag)
    return "" if value is None else value


def lit(value):
    assert '"' not in value and "\\" not in value and value.isascii(), value
    return f'"{value}"'


def slug(*parts):
    return re.sub(r"[^a-z0-9]+", "-", " ".join(parts).lower()).strip("-")


def vehicle_id(make, model, version, year, protocol):
    year = "2011" if year == "20011" else year
    protocol = RENAMED.get(protocol, protocol)
    return slug(make, model.strip(), version.strip(), year) + "--" + slug(protocol)


def protocol_entry(p):
    fields = [f".name = {lit(p.get('name'))}"]
    if p.get("alias"):
        fields.append(f".alias = {lit(p.get('alias'))}")
    for tag in ("ecu", "mcu", "mode"):
        fields.append(f".{tag} = {lit(text(p, tag))}")
    fields.append(f".checksum = {CHECKSUM[text(p, 'checksum')]}")
    for tag in ("read", "test_write", "write"):
        fields.append(f".{tag} = {'true' if text(p, tag) == 'yes' else 'false'}")
    for tag in ("flash_transport", "log_transport", "log_protocol"):
        fields.append(f".{tag} = {lit(text(p, tag))}")
    if text(p, "kernel"):
        fields.append(f".kernel = {lit(text(p, 'kernel'))}")
    if text(p, "kernel_addr"):
        fields.append(f".kernel_load_address = 0x{int(text(p, 'kernel_addr'), 16):X}U")
    fields.append(f".description = {lit(text(p, 'description'))}")
    return "    {" + ",\n     ".join(fields) + "},"


def vehicle_entry(c, protocols_name):
    values = {tag: text(c, tag) for tag in ("make", "model", "version", "type", "kw", "hp", "fuel", "year", "protocol")}
    fields = [f".id = {lit(vehicle_id(values['make'], values['model'], values['version'], values['year'], values['protocol']))}"]
    for tag in ("make", "model", "version", "type", "kw", "hp", "fuel", "year"):
        fields.append(f".{tag} = {lit(values[tag])}")
    fields.append(f".protocol = protocol_in({protocols_name}, {lit(values['protocol'])})")
    return "    {" + ",\n     ".join(fields) + "},"


def main():
    root = ET.parse(sys.argv[1]).getroot()
    prefix = sys.argv[2] if len(sys.argv) > 2 else ""
    protocols_name = f"k{prefix}Protocols"
    vehicles_name = f"k{prefix}Vehicles"
    protocols = root.find("protocols")
    car_models = root.find("car_models")
    print(f"constexpr auto {protocols_name} = std::to_array<ProtocolSpec>({{")
    for p in protocols.findall("protocol"):
        print(protocol_entry(p))
    print("});")
    print()
    print(f"constexpr auto {vehicles_name} = std::to_array<VehicleSpec>({{")
    for c in car_models.findall("car_model"):
        print(vehicle_entry(c, protocols_name))
    print("});")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Write the failing tests**

Create `src/backend/config/builtin_catalog_test.cpp`:

```cpp
#include "src/backend/config/builtin_catalog.h"

#include <array>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace
{

using fastecu::config::builtin_catalog;
using fastecu::config::catalog_problems;
using ::testing::UnorderedElementsAreArray;

// Every inconsistency protocols.cfg carried. Each data fix removes its lines;
// the reachability fix (Task 5) empties the list.
constexpr auto kKnownDefects = std::to_array<std::string_view>({
    "protocol 'sub_ecu_denso_mc68hc16y5_04' has no vehicle",
    "protocol 'sub_ecu_denso_mc68hc16y5_04_ecutek' has no vehicle",
    "protocol 'sub_ecu_denso_sh7055_02_ecutek' has no vehicle",
    "protocol 'sub_ecu_denso_sh7055_04_cobb' has no vehicle",
    "protocol 'sub_ecu_denso_sh7058_cobb' has no vehicle",
    "protocol 'sub_ecu_denso_sh7058_can_cobb' has no vehicle",
    "protocol 'mitsu_ecu_m32r_kline_mut_dma' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7055_kline' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7058_kline' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7055_densocan' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7058_densocan' has no vehicle",
    "protocol 'sub_ecu_eeprom_denso_sh7058_can_diesel' has no vehicle",
    "protocol 'sub_ecu_unisia_jecs_m3779x' has a kernel load address but no kernel",
    "protocol 'sub_ecu_unisia_jecs_m3779x' has no vehicle",
    "protocol 'sub_ecu_unisia_jecs_m3775x' has a kernel load address but no kernel",
    "protocol 'sub_ecu_unisia_jecs_m3775x' has no vehicle",
    "vehicle 'subaru-legacy-2-0-a-t-1990--sub-ecu-unisia-jecs-m3779x' has no protocol",
    "vehicle 'subaru-legacy-2-0-a-t-1990--sub-ecu-unisia-jecs-m3775x' has no protocol",
});

TEST(BuiltinCatalog, ListsEveryProtocolsCfgEntry)
{
    EXPECT_EQ(builtin_catalog().protocols().size(), 63U);
    EXPECT_EQ(builtin_catalog().vehicles().size(), 65U);
}

TEST(BuiltinCatalog, HasExactlyTheKnownDefects)
{
    EXPECT_THAT(catalog_problems(builtin_catalog()), UnorderedElementsAreArray(kKnownDefects));
}

} // namespace
```

Create `src/backend/config/builtin_catalog_parity_test.cpp`:

```cpp
#include "src/backend/config/builtin_catalog.h"

#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pugixml.hpp>

// Temporary: removed with protocols.cfg. Compares the built-in catalog with
// the file it was generated from; every difference must be a documented data
// fix (see the catalog spec's "Data fixes"), listed below in the order the
// fixes were applied.
namespace
{

using fastecu::config::builtin_catalog;
using fastecu::config::checksum_flag;
using fastecu::config::kernel_load_address_text;
using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;
using ::testing::UnorderedElementsAreArray;

std::string child_text(pugi::xml_node node, const char *tag)
{
    return node.child(tag).text().as_string();
}

std::string yes_no(bool value)
{
    return value ? "yes" : "no";
}

// The catalog keeps only "yes" as true; protocols.cfg also spelled false "n/a".
std::string file_flag(pugi::xml_node node, const char *tag)
{
    return yes_no(child_text(node, tag) == "yes");
}

std::string file_address(pugi::xml_node node)
{
    const std::string text = child_text(node, "kernel_addr");
    return text.empty() ? std::string{} : std::format("0x{:X}", std::stoul(text, nullptr, 16));
}

void compare(std::vector<std::string>& out, std::string_view what, std::string_view field, std::string_view file,
             std::string_view catalog)
{
    if (file != catalog)
    {
        out.push_back(std::format("{}: {} file '{}' catalog '{}'", what, field, file, catalog));
    }
}

std::vector<std::string> protocol_differences(pugi::xml_node protocols)
{
    std::vector<std::string> out;
    for (pugi::xml_node node : protocols.children("protocol"))
    {
        const std::string name = node.attribute("name").as_string();
        const std::string what = std::format("protocol {}", name);
        const ProtocolSpec *spec = builtin_catalog().find_protocol(name);
        if (spec == nullptr)
        {
            out.push_back(what + ": missing from catalog");
            continue;
        }
        compare(out, what, "alias", node.attribute("alias").as_string(), spec->alias);
        compare(out, what, "ecu", child_text(node, "ecu"), spec->ecu);
        compare(out, what, "mcu", child_text(node, "mcu"), spec->mcu);
        compare(out, what, "mode", child_text(node, "mode"), spec->mode);
        compare(out, what, "checksum", child_text(node, "checksum"), checksum_flag(spec->checksum));
        compare(out, what, "read", file_flag(node, "read"), yes_no(spec->read));
        compare(out, what, "test_write", file_flag(node, "test_write"), yes_no(spec->test_write));
        compare(out, what, "write", file_flag(node, "write"), yes_no(spec->write));
        compare(out, what, "flash_transport", child_text(node, "flash_transport"), spec->flash_transport);
        compare(out, what, "log_transport", child_text(node, "log_transport"), spec->log_transport);
        compare(out, what, "log_protocol", child_text(node, "log_protocol"), spec->log_protocol);
        compare(out, what, "kernel", child_text(node, "kernel"), spec->kernel);
        compare(out, what, "kernel_addr", file_address(node), kernel_load_address_text(*spec));
        compare(out, what, "description", child_text(node, "description"), spec->description);
    }
    for (const ProtocolSpec& spec : builtin_catalog().protocols())
    {
        if (!protocols.find_child_by_attribute("protocol", "name", std::string(spec.name).c_str()))
        {
            out.push_back(std::format("protocol {}: not in file", spec.name));
        }
    }
    return out;
}

std::vector<std::string> vehicle_differences(pugi::xml_node car_models)
{
    std::vector<std::string> out;
    const auto vehicles = builtin_catalog().vehicles();
    std::size_t row = 0;
    for (pugi::xml_node node : car_models.children("car_model"))
    {
        const std::string what = std::format("vehicle {}", row);
        if (row >= vehicles.size())
        {
            out.push_back(what + ": missing from catalog");
            ++row;
            continue;
        }
        const VehicleSpec& vehicle = vehicles[row];
        compare(out, what, "make", child_text(node, "make"), vehicle.make);
        compare(out, what, "model", child_text(node, "model"), vehicle.model);
        compare(out, what, "version", child_text(node, "version"), vehicle.version);
        compare(out, what, "type", child_text(node, "type"), vehicle.type);
        compare(out, what, "kw", child_text(node, "kw"), vehicle.kw);
        compare(out, what, "hp", child_text(node, "hp"), vehicle.hp);
        compare(out, what, "fuel", child_text(node, "fuel"), vehicle.fuel);
        compare(out, what, "year", child_text(node, "year"), vehicle.year);
        compare(out, what, "protocol", child_text(node, "protocol"),
                vehicle.protocol != nullptr ? vehicle.protocol->name : std::string_view{});
        ++row;
    }
    for (; row < vehicles.size(); ++row)
    {
        out.push_back(std::format("vehicle {}: not in file ({})", row, vehicles[row].id));
    }
    return out;
}

TEST(BuiltinCatalogParity, DiffersFromProtocolsCfgOnlyByTheDocumentedFixes)
{
    const char *path = std::getenv("PROTOCOLS_CFG_PATH");
    ASSERT_NE(path, nullptr);
    pugi::xml_document doc;
    ASSERT_TRUE(doc.load_file(path)) << path;
    const pugi::xml_node config = doc.child("config");

    const std::vector<std::string> expected_protocol_differences{};
    const std::vector<std::string> expected_vehicle_differences{
        // Faithful generation: these two rows name protocols the file does not
        // define, so they have none.
        "vehicle 1: protocol file 'sub_ecu_unisia_jecs_92' catalog ''",
        "vehicle 2: protocol file 'sub_ecu_unisia_jecs_97' catalog ''",
    };

    EXPECT_THAT(protocol_differences(config.child("protocols")),
                UnorderedElementsAreArray(expected_protocol_differences));
    EXPECT_THAT(vehicle_differences(config.child("car_models")),
                UnorderedElementsAreArray(expected_vehicle_differences));
}

} // namespace
```

- [ ] **Step 3: Write the header and generate the data**

Create `src/backend/config/builtin_catalog.h`:

```cpp
#pragma once
#include "src/backend/config/catalog.h"

namespace fastecu::config
{

// The protocols and vehicles this build supports. Only the desktop
// composition root hands it on; everything else is given a Catalog.
const Catalog& builtin_catalog();

} // namespace fastecu::config
```

Generate the arrays:

```bash
python3 "$TMPDIR/generate_catalog.py" resources/shared/config/protocols.cfg > "$TMPDIR/catalog_arrays.inc"
grep -c '\.name = ' "$TMPDIR/catalog_arrays.inc"   # expect 63
grep -c '\.id = ' "$TMPDIR/catalog_arrays.inc"     # expect 65
```

Create `src/backend/config/builtin_catalog.cpp` with this frame, pasting the whole of `catalog_arrays.inc` where marked:

```cpp
#include "src/backend/config/builtin_catalog.h"

#include <array>

namespace fastecu::config
{
namespace
{

// Generated once from the retired protocols.cfg and maintained by hand
// since. The vehicle order is the chooser's unsorted order, and alias
// resolution takes the first match in it: append new vehicles at the end.
// A vehicle id is saved in fastecu.cfg; never change or reuse one.

// <paste catalog_arrays.inc here: kProtocols, then kVehicles>

constexpr Catalog kBuiltinCatalog{kProtocols, kVehicles};

} // namespace

const Catalog& builtin_catalog()
{
    return kBuiltinCatalog;
}

} // namespace fastecu::config
```

There is deliberately no `static_assert` yet: the faithful data has two vehicles without a protocol. Task 5 adds it.

- [ ] **Step 4: Regenerate BUILD and add the hand-written attributes**

Run: `python3 scripts/gazelle_check.py --fix`

Then make the three new targets read as follows (keep whatever `deps` Gazelle produced if they differ only in order):

```starlark
cc_library(
    name = "builtin_catalog",
    srcs = ["builtin_catalog.cpp"],
    hdrs = ["builtin_catalog.h"],
    # Only the composition root hands the built-in catalog on; tests that
    # check it against other packages name it too.
    visibility = [
        "//bazel/layers:apps_desktop",
        "//bazel/layers:tests",
    ],
    deps = [":catalog"],
)

fastecu_portable_gtest(
    name = "builtin_catalog_test",
    srcs = ["builtin_catalog_test.cpp"],
    deps = [
        ":builtin_catalog",
        "@googletest//:gtest",
    ],
)

# Temporary: compares the catalog with the protocols.cfg it was generated from.
fastecu_portable_gtest(
    name = "builtin_catalog_parity_test",
    srcs = ["builtin_catalog_parity_test.cpp"],
    data = ["//resources/shared:config/protocols.cfg"],
    env = {
        "PROTOCOLS_CFG_PATH": "$(location //resources/shared:config/protocols.cfg)",
    },
    deps = [
        ":builtin_catalog",
        "@googletest//:gtest",
        "@pugixml",
    ],
)
```

`//resources/shared:config/protocols.cfg` is already exported to `//src/backend/config:__pkg__`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/config:builtin_catalog_test //src/backend/config:builtin_catalog_parity_test`
Expected: PASS. If the parity test reports any difference other than the two listed lines, the generation is wrong: fix the generator, regenerate, and do not edit the expectation.

- [ ] **Step 6: Commit**

```bash
prek run --files src/backend/config/builtin_catalog.h src/backend/config/builtin_catalog.cpp src/backend/config/builtin_catalog_test.cpp src/backend/config/builtin_catalog_parity_test.cpp src/backend/config/BUILD.bazel
git add src/backend/config/builtin_catalog.h src/backend/config/builtin_catalog.cpp src/backend/config/builtin_catalog_test.cpp src/backend/config/builtin_catalog_parity_test.cpp src/backend/config/BUILD.bazel
git commit -m "feat(config): generate the built-in catalog from protocols.cfg"
```

---

### Task 3: Mechanical data fixes

Five commits, one per fix. For each: edit `builtin_catalog.cpp`, add a pinning test to `builtin_catalog_test.cpp`, update the expectations in `builtin_catalog_parity_test.cpp` (and `kKnownDefects` where stated), run both tests, commit.

**Files:**
- Modify: `src/backend/config/builtin_catalog.cpp`
- Modify: `src/backend/config/builtin_catalog_test.cpp`
- Modify: `src/backend/config/builtin_catalog_parity_test.cpp`

**Interfaces:**
- Consumes: `builtin_catalog()`, `Catalog::find_protocol`, `Catalog::find_vehicle`.
- Produces: corrected data only.

Add these helpers to the anonymous namespace of `builtin_catalog_test.cpp` before the first fix test (with `#include <cstdint>`, `#include <optional>`, `#include <string>` and `using fastecu::config::ProtocolSpec;`):

```cpp
const ProtocolSpec& protocol_named(std::string_view name)
{
    const ProtocolSpec *protocol = builtin_catalog().find_protocol(name);
    if (protocol == nullptr)
    {
        ADD_FAILURE() << "no built-in protocol " << name;
        static constexpr ProtocolSpec kAbsent{};
        return kAbsent;
    }
    return *protocol;
}
```

The test command for every fix is:

Run: `bazel test --config=release //src/backend/config:builtin_catalog_test //src/backend/config:builtin_catalog_parity_test`

- [ ] **Step 1: Fix 1 — the SH7055 TCU kernel names the bundled file**

Test (append to `builtin_catalog_test.cpp`):

```cpp
TEST(BuiltinCatalogFixes, Sh7055TcuKernelNamesTheBundledFile)
{
    EXPECT_EQ(protocol_named("sub_tcu_denso_sh7055_can").kernel, "ssmk_tcu_can_sh7055_35.bin");
}
```

Run the test command. Expected: FAIL (`ssmk_tcu_can_SH7055_35.bin`).

Edit: in `kProtocols`, entry `.name = "sub_tcu_denso_sh7055_can"`, change `.kernel = "ssmk_tcu_can_SH7055_35.bin"` to `.kernel = "ssmk_tcu_can_sh7055_35.bin"`.

Parity: add to `expected_protocol_differences`:

```cpp
        // Fix 1: the bundled file is lowercase; case-sensitive filesystems failed the read.
        "protocol sub_tcu_denso_sh7055_can: kernel file 'ssmk_tcu_can_SH7055_35.bin' catalog "
        "'ssmk_tcu_can_sh7055_35.bin'",
```

Run the test command. Expected: PASS. Commit:

```bash
git add src/backend/config/builtin_catalog.cpp src/backend/config/builtin_catalog_test.cpp src/backend/config/builtin_catalog_parity_test.cpp
git commit -m "fix(config): name the bundled SH7055 TCU kernel with its real case"
```

- [ ] **Step 2: Fix 2 — the two Legacy 1990 A/T vehicles reach the renamed Unisia Jecs protocols**

Test:

```cpp
TEST(BuiltinCatalogFixes, Legacy1990RowsReachTheRenamedUnisiaJecsProtocols)
{
    const auto vehicles = builtin_catalog().vehicles();
    ASSERT_GE(vehicles.size(), 3U);
    ASSERT_NE(vehicles[1].protocol, nullptr);
    ASSERT_NE(vehicles[2].protocol, nullptr);
    EXPECT_EQ(vehicles[1].protocol->name, "sub_ecu_unisia_jecs_m3779x");
    EXPECT_EQ(vehicles[2].protocol->name, "sub_ecu_unisia_jecs_m3775x");
}
```

Run the test command. Expected: FAIL (null protocols).

Edit: in `kVehicles`, entry `.id = "subaru-legacy-2-0-a-t-1990--sub-ecu-unisia-jecs-m3779x"`, change `.protocol = protocol_in(kProtocols, "sub_ecu_unisia_jecs_92")` to `.protocol = protocol_in(kProtocols, "sub_ecu_unisia_jecs_m3779x")`; entry `.id = "subaru-legacy-2-0-a-t-1990--sub-ecu-unisia-jecs-m3775x"`, change `"sub_ecu_unisia_jecs_97"` to `"sub_ecu_unisia_jecs_m3775x"`.

Parity: replace the two `expected_vehicle_differences` lines with:

```cpp
        // Fix 2: upstream 90f11ae9 renamed these protocols without updating the vehicles.
        "vehicle 1: protocol file 'sub_ecu_unisia_jecs_92' catalog 'sub_ecu_unisia_jecs_m3779x'",
        "vehicle 2: protocol file 'sub_ecu_unisia_jecs_97' catalog 'sub_ecu_unisia_jecs_m3775x'",
```

Known defects: remove these four lines from `kKnownDefects`:

```
"protocol 'sub_ecu_unisia_jecs_m3779x' has no vehicle",
"protocol 'sub_ecu_unisia_jecs_m3775x' has no vehicle",
"vehicle 'subaru-legacy-2-0-a-t-1990--sub-ecu-unisia-jecs-m3779x' has no protocol",
"vehicle 'subaru-legacy-2-0-a-t-1990--sub-ecu-unisia-jecs-m3775x' has no protocol",
```

Run the test command. Expected: PASS. Commit with message `fix(config): point the Legacy 1990 vehicles at the renamed Unisia Jecs protocols`.

- [ ] **Step 3: Fix 3 — vehicle text typos**

Tests (add `#include <algorithm>`):

```cpp
std::string_view trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

TEST(BuiltinCatalogFixes, VehicleTextHasNoStrayWhitespace)
{
    for (const auto& vehicle : builtin_catalog().vehicles())
    {
        for (std::string_view field : {vehicle.make, vehicle.model, vehicle.version, vehicle.type, vehicle.kw,
                                       vehicle.hp, vehicle.fuel, vehicle.year})
        {
            EXPECT_EQ(field, trimmed(field)) << vehicle.id;
        }
    }
}

TEST(BuiltinCatalogFixes, Sh7059DieselDensoCanYearIs2011)
{
    const auto row = builtin_catalog().find_vehicle(
        "subaru-all-sh7059-denso-can-diesel-models-sh7059-2011--sub-ecu-denso-sh7059-diesel-densocan");
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(builtin_catalog().vehicles()[*row].year, "2011");
}
```

Run the test command. Expected: FAIL.

Edit `kVehicles`:
- `.id = "subaru-legacy-gt-2-0-5mt-2003--sub-ecu-denso-sh7055-04"`: `.version = "2.0 5MT "` → `.version = "2.0 5MT"`
- `.id = "subaru-legacy-gt-2-0-5mt-2003--sub-ecu-denso-sh7055-04-ecutek"`: same change
- `.id = "subaru-legacy-gt-2-0-5mt-2006--sub-ecu-denso-sh7058-ecutek"`: same change
- `.id = "subaru-all-sh7059-denso-can-diesel-models-sh7059-2011--sub-ecu-denso-sh7059-diesel-densocan"`: `.year = "20011"` → `.year = "2011"`

Parity: add to `expected_vehicle_differences`:

```cpp
        // Fix 3: typos.
        "vehicle 10: version file '2.0 5MT ' catalog '2.0 5MT'",
        "vehicle 35: year file '20011' catalog '2011'",
        "vehicle 39: version file '2.0 5MT ' catalog '2.0 5MT'",
        "vehicle 40: version file '2.0 5MT ' catalog '2.0 5MT'",
```

Run the test command. Expected: PASS. Commit with message `fix(config): correct a five-digit year and trailing spaces in vehicle text`.

- [ ] **Step 4: Fix 4 — Unisia Jecs M3779x/M3775x declare no kernel load address**

Test:

```cpp
TEST(BuiltinCatalogFixes, UnisiaJecsM377xDeclareNoKernelLoadAddress)
{
    for (std::string_view name : {"sub_ecu_unisia_jecs_m3779x", "sub_ecu_unisia_jecs_m3775x"})
    {
        EXPECT_TRUE(protocol_named(name).kernel.empty()) << name;
        EXPECT_FALSE(protocol_named(name).kernel_load_address.has_value()) << name;
    }
}
```

Run the test command. Expected: FAIL.

Edit: delete the `.kernel_load_address = 0x0U,` line from the `sub_ecu_unisia_jecs_m3779x` and `sub_ecu_unisia_jecs_m3775x` entries.

Parity: add to `expected_protocol_differences`:

```cpp
        // Fix 4: no kernel, so no load address; the family's plan requires none.
        "protocol sub_ecu_unisia_jecs_m3779x: kernel_addr file '0x0' catalog ''",
        "protocol sub_ecu_unisia_jecs_m3775x: kernel_addr file '0x0' catalog ''",
```

Known defects: remove the two `"... has a kernel load address but no kernel"` lines.

Run the test command. Expected: PASS. Commit with message `fix(config): drop the kernel load address of the kernel-free Unisia Jecs protocols`.

- [ ] **Step 5: Fix 5 — the SH72543 diesel uses the ECU's on-board kernel**

Test:

```cpp
TEST(BuiltinCatalogFixes, Sh72543DieselUploadsNoKernel)
{
    const ProtocolSpec& protocol = protocol_named("sub_ecu_denso_sh72543_can_diesel");
    EXPECT_TRUE(protocol.kernel.empty());
    EXPECT_FALSE(protocol.kernel_load_address.has_value());
}
```

Run the test command. Expected: FAIL.

Edit: delete the `.kernel = "ssmk_can_tp_sh72543d_euro6.bin",` and `.kernel_load_address = 0xFFF80000U,` lines from the `sub_ecu_denso_sh72543_can_diesel` entry.

Parity: add to `expected_protocol_differences`:

```cpp
        // Fix 5: the family talks to the on-board kernel and rejects a plan with one;
        // the named kernel was never bundled.
        "protocol sub_ecu_denso_sh72543_can_diesel: kernel file 'ssmk_can_tp_sh72543d_euro6.bin' catalog ''",
        "protocol sub_ecu_denso_sh72543_can_diesel: kernel_addr file '0xFFF80000' catalog ''",
```

Run the test command. Expected: PASS. Commit with message `fix(config): drop the unused kernel of the SH72543 diesel protocol`.

---

### Task 4: The EEPROM SH7058 DensoCAN kernel load address

**Files:**
- Modify: `src/backend/config/builtin_catalog.cpp`
- Modify: `src/backend/config/builtin_catalog_test.cpp`
- Modify: `src/backend/config/builtin_catalog_parity_test.cpp`
- Modify: `docs/flash-qualification-matrix.md` (row `DensoSh705xEepromCan`)

**Interfaces:**
- Consumes: `protocol_named()` from Task 3.
- Produces: corrected data and a VERIFY item.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(BuiltinCatalogFixes, Sh7058DensoCanEepromLoadsItsKernelWhereTheFlashFamilyDoes)
{
    const ProtocolSpec& eeprom = protocol_named("sub_ecu_eeprom_denso_sh7058_densocan");
    const ProtocolSpec& flash = protocol_named("sub_ecu_denso_sh7058_densocan");
    EXPECT_EQ(eeprom.kernel, flash.kernel);
    EXPECT_EQ(eeprom.kernel_load_address, std::optional<std::uint32_t>(0xFFFF3000U));
    EXPECT_EQ(eeprom.kernel_load_address, flash.kernel_load_address);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test --config=release //src/backend/config:builtin_catalog_test`
Expected: FAIL (`0xFFFF6004`).

- [ ] **Step 3: Correct the data and the parity expectation**

Edit `kProtocols`, entry `sub_ecu_eeprom_denso_sh7058_densocan`: `.kernel_load_address = 0xFFFF6004U` → `.kernel_load_address = 0xFFFF3000U`.

Parity: add to `expected_protocol_differences`:

```cpp
        // Fix 6: 0xFFFF6004 is the SH7055 address; the same kernel loads at
        // 0xFFFF3000 for every main-flash SH7058 DensoCAN entry. VERIFY on bench.
        "protocol sub_ecu_eeprom_denso_sh7058_densocan: kernel_addr file '0xFFFF6004' catalog '0xFFFF3000'",
```

- [ ] **Step 4: Record the VERIFY item**

In `docs/flash-qualification-matrix.md`, append to the notes cell of the `DensoSh705xEepromCan` row (keep it one table row):

```markdown
 **VERIFY on first bench use:** `sub_ecu_eeprom_denso_sh7058_densocan` uploads `ssmk_can_sh7058.bin` to `0xFFFF3000`, corrected from the retired `protocols.cfg` value `0xFFFF6004` (the SH7055 address) to match the three main-flash DensoCAN SH7058 entries that load the same kernel. The preflight's SH7058 kernel RAM window `0xFFFF3000`–`0xFFFFC000` accepts both, so only a bench read confirms it. The protocol was unreachable before the built-in catalog offered it.
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/config:builtin_catalog_test //src/backend/config:builtin_catalog_parity_test`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
prek run --files src/backend/config/builtin_catalog.cpp src/backend/config/builtin_catalog_test.cpp src/backend/config/builtin_catalog_parity_test.cpp docs/flash-qualification-matrix.md
git add src/backend/config/builtin_catalog.cpp src/backend/config/builtin_catalog_test.cpp src/backend/config/builtin_catalog_parity_test.cpp docs/flash-qualification-matrix.md
git commit -m "fix(config): load the SH7058 DensoCAN EEPROM kernel at the SH7058 address"
```

---

### Task 5: Every protocol reachable

**Files:**
- Modify: `src/backend/config/builtin_catalog.cpp`
- Modify: `src/backend/config/builtin_catalog_test.cpp`
- Modify: `src/backend/config/builtin_catalog_parity_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.cpp:15-33`
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan_test.cpp:64-73`
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp` (`kRoutes` row and the comment near line 716)
- Modify: `src/platform/desktop/common/flash/flash_workflow_test.cpp:810-819`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: a consistent built-in catalog (61 protocols, 75 vehicles) guarded by `static_assert(catalog_references_resolve(...))`. Vehicle ids added here are final:
  - `mitsubishi-unknown-unk-ecu-unk--mitsu-ecu-m32r-kline-mut-dma`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7055-02-ecutek`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7055-04-cobb`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7058-cobb`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7058-can-cobb`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7055-kline`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-kline`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7055-densocan`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-densocan`
  - `subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-can-diesel`

- [ ] **Step 1: Write the failing tests**

In `builtin_catalog_test.cpp`, delete `kKnownDefects` and `HasExactlyTheKnownDefects`, change `ListsEveryProtocolsCfgEntry`, and add the reachability tests (add `using ::testing::IsEmpty;`):

```cpp
TEST(BuiltinCatalog, ListsSixtyOneProtocolsAndSeventyFiveVehicles)
{
    EXPECT_EQ(builtin_catalog().protocols().size(), 61U);
    EXPECT_EQ(builtin_catalog().vehicles().size(), 75U);
}

TEST(BuiltinCatalog, IsConsistent)
{
    EXPECT_THAT(catalog_problems(builtin_catalog()), IsEmpty());
}

TEST(BuiltinCatalogFixes, Mc68Revision04IsGone)
{
    EXPECT_EQ(builtin_catalog().find_protocol("sub_ecu_denso_mc68hc16y5_04"), nullptr);
    EXPECT_EQ(builtin_catalog().find_protocol("sub_ecu_denso_mc68hc16y5_04_ecutek"), nullptr);
}

// The new vehicles come after every older row, so an alias two protocols
// share still resolves to the protocol it resolved to before.
TEST(BuiltinCatalogFixes, SharedAliasesResolveAsBefore)
{
    const auto resolves_to = [](std::string_view alias)
    {
        const auto *vehicle = builtin_catalog().first_vehicle_for_alias(alias);
        return vehicle == nullptr ? std::string_view{} : vehicle->protocol->name;
    };
    EXPECT_EQ(resolves_to("fxt02"), "sub_ecu_denso_sh7055_02");
    EXPECT_EQ(resolves_to("wrx02"), "sub_ecu_denso_mc68hc16y5_02");
    EXPECT_EQ(resolves_to("subarucand"), "sub_ecu_denso_sh7058_can_diesel");
}

TEST(BuiltinCatalogFixes, CobbAliasesNowSelectTheirVehicles)
{
    for (const auto& [alias, protocol] : {std::pair{"sti04_cobb", "sub_ecu_denso_sh7055_04_cobb"},
                                         std::pair{"sti05_cobb", "sub_ecu_denso_sh7058_cobb"},
                                         std::pair{"subarucan_cobb", "sub_ecu_denso_sh7058_can_cobb"}})
    {
        const auto *vehicle = builtin_catalog().first_vehicle_for_alias(alias);
        ASSERT_NE(vehicle, nullptr) << alias;
        EXPECT_EQ(vehicle->protocol->name, protocol);
    }
}

TEST(BuiltinCatalogFixes, MutDmaLoggingHasAMitsubishiVehicle)
{
    const auto row = builtin_catalog().find_vehicle("mitsubishi-unknown-unk-ecu-unk--mitsu-ecu-m32r-kline-mut-dma");
    ASSERT_TRUE(row.has_value());
    const auto& vehicle = builtin_catalog().vehicles()[*row];
    EXPECT_EQ(vehicle.make, "Mitsubishi");
    EXPECT_EQ(vehicle.protocol->log_protocol, "MUT_DMA");
}
```

In `subaru_denso_mc68hc16y5_02_plan_test.cpp`, replace `RejectsRevision04Entirely` with:

```cpp
TEST(SubaruDensoMc68hc16y5_02Plan, Revision04IsNotAProtocolOfThisFamily)
{
    for (auto *name : {"sub_ecu_denso_mc68hc16y5_04", "sub_ecu_denso_mc68hc16y5_04_ecutek"})
    {
        ASSERT_THAT(
            build_subaru_denso_mc68hc16y5_02_plan(FlashOperation::Read, name, "MC68HC16Y5", std::nullopt,
                                                  KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0xaa}}),
            fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    }
}
```

In `flash_workflow_test.cpp`, replace `mc68Revision04IsClaimedButPlanBuildFails` with:

```cpp
TEST(FlashWorkflowTest, mc68Revision04HasNoRoute)
{
    for (const char *protocol : {"sub_ecu_denso_mc68hc16y5_04", "sub_ecu_denso_mc68hc16y5_04_ecutek"})
    {
        auto input = request(protocol);
        input.mcu = "MC68HC16Y5";
        ASSERT_TRUE(FlashWorkflowFactory::tryCreate(std::move(input)) == nullptr) << protocol;
    }
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `bazel test --config=release //src/backend/config:builtin_catalog_test //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_plan_test //src/platform/desktop/common/flash:test_flash_workflow`
Expected: FAIL (sizes, problems, `_04` still present and routed, plan returns `Unsupported`).

- [ ] **Step 3: Delete MC68 revision 04 everywhere**

`builtin_catalog.cpp`: delete the `kProtocols` entries named `sub_ecu_denso_mc68hc16y5_04` and `sub_ecu_denso_mc68hc16y5_04_ecutek`.

`subaru_denso_mc68hc16y5_02_plan.cpp`: replace the comment above `validate_identity` and the function's name checks with:

```cpp
// mainwindow.cpp:1250-1258: sub_ecu_denso_mc68hc16y5_02(_ecutek)? and the
// reachable-but-quirky _02_tpu (see spec) all construct this class.
// Revision 04, which declared no supported operation, was deleted with the
// built-in catalog and is now an unknown name like any other.
Status validate_identity(std::string_view protocol, std::string_view mcu)
{
    using enum ErrorKind;
    if (protocol != "sub_ecu_denso_mc68hc16y5_02" && protocol != "sub_ecu_denso_mc68hc16y5_02_ecutek" &&
        protocol != "sub_ecu_denso_mc68hc16y5_02_tpu")
    {
        return fail(InvalidConfig, std::format("Unsupported MC68HC16Y5_02 protocol: {}", protocol));
    }
    if (const std::string_view expected_mcu =
            protocol == "sub_ecu_denso_mc68hc16y5_02_tpu" ? "MC68HC16Y5_TPU" : "MC68HC16Y5";
        mcu != expected_mcu)
    {
        return fail(InvalidConfig, std::format("protocol {} requires MCU {}, not {}", protocol, expected_mcu, mcu));
    }
    return {};
}
```

`flash_workflow.cpp`: delete the `{"sub_ecu_denso_mc68hc16y5_04", SubaruDensoMc68hc16y5_02},` row from `kRoutes`, and in `prepareMc68` replace the comment `// Run the family builder first so recognized-but-unsupported // revision 04 is rejected by the plan even without a catalog.` with `// Run the family builder first so a protocol or MCU the family rejects // fails before the kernel file is read.`

- [ ] **Step 4: Append the ten vehicles and enable the static check**

In `builtin_catalog.cpp`, append these entries at the end of `kVehicles`:

```cpp
    // Protocols the retired protocols.cfg declared but no vehicle reached, in
    // the unknown-vehicle convention of the rows above. Appended last so a
    // shared alias still resolves to the older row.
    {.id = "mitsubishi-unknown-unk-ecu-unk--mitsu-ecu-m32r-kline-mut-dma",
     .make = "Mitsubishi",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "mitsu_ecu_m32r_kline_mut_dma")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7055-02-ecutek",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_denso_sh7055_02_ecutek")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7055-04-cobb",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_denso_sh7055_04_cobb")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7058-cobb",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_denso_sh7058_cobb")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7058-can-cobb",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_denso_sh7058_can_cobb")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7055-kline",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_eeprom_denso_sh7055_kline")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-kline",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_eeprom_denso_sh7058_kline")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7055-densocan",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_eeprom_denso_sh7055_densocan")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-densocan",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Petrol",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_eeprom_denso_sh7058_densocan")},
    {.id = "subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-can-diesel",
     .make = "Subaru",
     .model = "Unknown",
     .version = "unk ECU",
     .type = "Unk",
     .kw = "Unk",
     .hp = "Unk",
     .fuel = "Diesel",
     .year = "Unk",
     .protocol = protocol_in(kProtocols, "sub_ecu_eeprom_denso_sh7058_can_diesel")},
```

After `kVehicles`, before `kBuiltinCatalog`, add:

```cpp
static_assert(catalog_references_resolve(kProtocols, kVehicles),
              "every built-in vehicle needs a protocol and every built-in protocol a vehicle");
```

If MSVC rejects this `static_assert` for exceeding its constant-evaluation step limit, move only the "every protocol has a vehicle" part to the `IsConsistent` test (it already checks it) — do not raise `/constexpr:steps`.

- [ ] **Step 5: Update the parity expectations**

Add to `expected_protocol_differences`:

```cpp
        // Fix 7: revision 04 declared no supported operation.
        "protocol sub_ecu_denso_mc68hc16y5_04: missing from catalog",
        "protocol sub_ecu_denso_mc68hc16y5_04_ecutek: missing from catalog",
```

Add to `expected_vehicle_differences`:

```cpp
        // Fix 8: one vehicle for each protocol no vehicle reached.
        "vehicle 65: not in file (mitsubishi-unknown-unk-ecu-unk--mitsu-ecu-m32r-kline-mut-dma)",
        "vehicle 66: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7055-02-ecutek)",
        "vehicle 67: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7055-04-cobb)",
        "vehicle 68: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7058-cobb)",
        "vehicle 69: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-denso-sh7058-can-cobb)",
        "vehicle 70: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7055-kline)",
        "vehicle 71: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-kline)",
        "vehicle 72: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7055-densocan)",
        "vehicle 73: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-densocan)",
        "vehicle 74: not in file (subaru-unknown-unk-ecu-unk--sub-ecu-eeprom-denso-sh7058-can-diesel)",
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `bazel test --config=release //src/backend/config/... //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_plan_test //src/platform/desktop/common/flash:test_flash_workflow`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
prek run --all-files
git add src/backend/config src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.cpp src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan_test.cpp src/platform/desktop/common/flash/flash_workflow.cpp src/platform/desktop/common/flash/flash_workflow_test.cpp
git commit -m "feat(config): give every built-in protocol a vehicle and delete MC68 revision 04"
```

---

### Task 6: The catalog agrees with checksum routing, the kernel bundle and the flash models

**Files:**
- Modify: `src/backend/checksum/dispatch.h`, `src/backend/checksum/dispatch.cpp`, `src/backend/checksum/dispatch_test.cpp`
- Modify: `resources/shared/BUILD.bazel` (new `filegroup`)
- Create: `tests/catalog_consistency_test.cpp`
- Modify: `tests/BUILD.bazel`

**Interfaces:**
- Consumes: `builtin_catalog()`.
- Produces: `bool fastecu::checksum::has_route(std::string_view make, std::string_view flash_method);` and `//resources/shared:kernel_files`.

- [ ] **Step 1: Write the failing `has_route` test**

Append to `src/backend/checksum/dispatch_test.cpp`:

```cpp
TEST(HasRoute, MatchesExactlyTheRoutesCorrectionDispatches)
{
    EXPECT_TRUE(fastecu::checksum::has_route("Subaru", "sub_ecu_denso_sh7058_can"));
    EXPECT_TRUE(fastecu::checksum::has_route("Subaru", "sub_ecu_denso_sh7058_can_cobb")); // prefix routes take suffixes
    EXPECT_TRUE(fastecu::checksum::has_route("Mitsubishi", "mitsu_ecu_m32r_can_512kb"));
    EXPECT_FALSE(fastecu::checksum::has_route("Mitsubishi", "sub_ecu_denso_sh7058_can")); // a route belongs to one make
    EXPECT_FALSE(fastecu::checksum::has_route("Subaru", "sub_ecu_mitsu_m32r_kline"));
    EXPECT_FALSE(fastecu::checksum::has_route("Mitsubishi", "mitsu_ecu_m32r_kline_mut_dma"));
}
```

Run: `bazel test --config=release //src/backend/checksum:dispatch_test`
Expected: FAIL — `has_route` is not declared.

- [ ] **Step 2: Implement `has_route`**

In `dispatch.h`, after `apply_checksum_correction`, add (and `#include <string_view>`):

```cpp
// Whether correction has a family for this make and flash method: the
// routing decision alone, without ROM bytes. The built-in catalog's checksum
// flags are tested against it.
bool has_route(std::string_view make, std::string_view flash_method);
```

In `dispatch.cpp`, after `dispatch_family` (outside the anonymous namespace, next to `apply_checksum_correction`; add `#include <algorithm>`):

```cpp
bool has_route(std::string_view make, std::string_view flash_method)
{
    return std::ranges::any_of(kRoutes, [&](const RouteSpec& spec)
                               { return spec.make == make && starts_with(flash_method, spec.prefix); });
}
```

Run: `bazel test --config=release //src/backend/checksum:dispatch_test`
Expected: PASS.

- [ ] **Step 3: Expose the kernel files to tests**

In `resources/shared/BUILD.bazel`, after the `exports_files` block, add:

```starlark
# The bundled kernels as plain files, so tests can compare the catalog's
# kernel names with the bundle and load real kernels.
filegroup(
    name = "kernel_files",
    srcs = glob(["kernels/*"]),
    visibility = ["//bazel/layers:tests"],
)
```

- [ ] **Step 4: Write the consistency test**

Create `tests/catalog_consistency_test.cpp`:

```cpp
// The built-in catalog against the three things it must agree with and no
// single production package owns: checksum routing, the kernel bundle, and
// the flash memory models.
#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "src/backend/checksum/dispatch.h"
#include "src/backend/config/builtin_catalog.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace
{

using fastecu::config::builtin_catalog;
using fastecu::config::ChecksumSupport;

TEST(CatalogConsistency, ChecksumFlagAgreesWithChecksumRouting)
{
    for (const auto& vehicle : builtin_catalog().vehicles())
    {
        const bool routed = fastecu::checksum::has_route(vehicle.make, vehicle.protocol->name);
        EXPECT_EQ(vehicle.protocol->checksum == ChecksumSupport::Corrected, routed)
            << vehicle.id << " (" << vehicle.protocol->name << ")";
    }
}

// File names exactly as Bazel bundles them, from
// $(locations //resources/shared:kernel_files): compared as strings, so a
// case-insensitive filesystem cannot hide a mismatch.
std::set<std::string> bundled_kernel_names()
{
    std::set<std::string> names;
    const char *paths = std::getenv("KERNEL_FILES");
    if (paths == nullptr)
    {
        return names;
    }
    std::istringstream stream{paths};
    for (std::string path; stream >> path;)
    {
        names.insert(path.substr(path.find_last_of('/') + 1));
    }
    return names;
}

TEST(CatalogConsistency, EveryKernelNameIsABundledFileSpelledExactly)
{
    const std::set<std::string> bundled = bundled_kernel_names();
    ASSERT_FALSE(bundled.empty());
    for (const auto& protocol : builtin_catalog().protocols())
    {
        if (!protocol.kernel.empty())
        {
            EXPECT_TRUE(bundled.contains(std::string(protocol.kernel))) << protocol.name << ": " << protocol.kernel;
        }
    }
}

TEST(CatalogConsistency, EveryBundledKernelIsUsed)
{
    for (const std::string& name : bundled_kernel_names())
    {
        EXPECT_TRUE(std::ranges::any_of(builtin_catalog().protocols(),
                                        [&name](const auto& protocol) { return protocol.kernel == name; }))
            << name;
    }
}

// MUT/DMA logging's M32170 is the one MCU without a flash memory model, and
// that protocol offers no flash operation.
TEST(CatalogConsistency, EveryFlashCapableProtocolNamesAKnownMcu)
{
    for (const auto& protocol : builtin_catalog().protocols())
    {
        if (protocol.read || protocol.test_write || protocol.write)
        {
            EXPECT_NE(fastecu::flash::find_flash_device(protocol.mcu), nullptr)
                << protocol.name << ": " << protocol.mcu;
        }
    }
}

} // namespace
```

- [ ] **Step 5: Register the test**

Run `python3 scripts/gazelle_check.py --fix`, then make the generated target in `tests/BUILD.bazel` read:

```starlark
# The built-in catalog against checksum routing, the kernel bundle and the
# flash memory models; no single production package owns all four.
fastecu_gtest(
    name = "catalog_consistency_test",
    srcs = ["catalog_consistency_test.cpp"],
    data = ["//resources/shared:kernel_files"],
    env = {"KERNEL_FILES": "$(locations //resources/shared:kernel_files)"},
    qt = False,
    deps = [
        "//src/backend/checksum:dispatch",
        "//src/backend/config:builtin_catalog",
        "//src/backend/flash:flash_device_lookup",
        "@googletest//:gtest",
    ],
)
```

- [ ] **Step 6: Run it to verify it passes**

Run: `bazel test --config=release //tests:catalog_consistency_test`
Expected: PASS. A failure here is a real disagreement between the data and the code: fix the catalog data if the data is wrong; if the code looks wrong, stop and report — never weaken a route or a plan.

- [ ] **Step 7: Commit**

```bash
prek run --all-files
git add src/backend/checksum resources/shared/BUILD.bazel tests/catalog_consistency_test.cpp tests/BUILD.bazel
git commit -m "test: check the built-in catalog against checksum routes, kernels and flash models"
```

---

### Task 7: The configuration session and its consumers read the catalog

The largest task: the session's vehicle type changes, so every reader of `ConfigSession::vehicles()` / `selected_vehicle()` changes with it in one commit. Selection still uses the saved row (`protocol_id`); Task 10 changes that. Flash and EEPROM still read `protocols.cfg` by name until Task 8.

**Files:**
- Modify: `src/backend/config/config_session.h`, `src/backend/config/config_session.cpp`, `src/backend/config/config_session_test.cpp`, `src/backend/config/BUILD.bazel`
- Modify: `src/backend/config/testing/config_session_fixture.h`, `src/backend/config/testing/BUILD.bazel`
- Modify: `src/backend/calibration/session/rom_open.cpp`
- Modify: `src/ui/desktop/config_fields.h`, `src/ui/desktop/config_fields.cpp`, `src/ui/desktop/config_fields_test.cpp`
- Modify: `src/ui/desktop/widgets/vehicle_select.cpp`, `src/ui/desktop/widgets/protocol_select.cpp`, `src/ui/desktop/widgets/protocol_select_test.cpp`
- Modify: `src/ui/desktop/widgets/mainwindow.h`, `src/ui/desktop/widgets/mainwindow.cpp`, `src/ui/desktop/widgets/mainwindow_test.cpp`
- Modify: `src/ui/desktop/calibration/calibration_operation_coordinator.cpp`, `src/ui/desktop/calibration/calibration_operation_coordinator_test.cpp`
- Modify: `apps/desktop/desktop_composition.cpp`, `apps/desktop/desktop_composition_test.cpp`, `apps/desktop/BUILD.bazel`
- Modify: whatever else `bazel build //...` reports (the compiler is the checklist)

**Interfaces:**
- Consumes: `Catalog`, `VehicleSpec`, `ProtocolSpec`, `checksum_flag`, `kernel_load_address_text`, `builtin_catalog()`.
- Produces:
  - `ConfigSession(const Catalog& catalog, IFileSystem&, IResourceBundle&, IFileRepository&, IEventSink&)` — stores the reference; `catalog` must outlive the session and is read by `initialize()`.
  - `std::span<const VehicleSpec> ConfigSession::vehicles() const` (empty until initialized), `const VehicleSpec *selected_vehicle() const`, `const VehicleSpec *vehicle_for_alias(std::string_view flash_method) const`.
  - Fixture (`fastecu::config::testing`): `kStandardProtocols`, `kStandardVehicles` (ids `subaru-impreza-v1`, `mitsubishi-colt-v2`, `subaru-forester-v3`), `kStandardCatalog`; `ConfigSessionFixture` gains a public `Catalog catalog` member (assign before `initialize()`) and loses `put_protocols`.
  - UI helpers in `fastecu::ui`: `protocol_field(const config::VehicleSpec&, std::string_view config::ProtocolSpec::*)`, `protocol_flag(const config::VehicleSpec&, bool config::ProtocolSpec::*)`, `protocol_capability(const config::VehicleSpec&, bool config::ProtocolSpec::*)`, `checksum_field(const config::VehicleSpec&)`, `kernel_address_field(const config::VehicleSpec&)`.

- [ ] **Step 1: Rewrite the session fixture**

Replace the body of `src/backend/config/testing/config_session_fixture.h` (keep the include guards and namespace) with:

```cpp
#pragma once
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/config/catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/config/config_session.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_file_system.h"
#include "src/backend/ports/testing/in_memory_resource_bundle.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::config::testing
{

inline constexpr std::string_view kRoot = "/root";
inline constexpr std::string_view kVersion = "0.1.0-beta.5";

inline constexpr auto kStandardProtocols = std::to_array<ProtocolSpec>({
    {.name = "proto_a",
     .alias = "alias_a",
     .ecu = "ECU A",
     .mcu = "SH7058",
     .mode = "OBD2",
     .checksum = ChecksumSupport::Corrected,
     .read = true,
     .test_write = false,
     .write = true,
     .flash_transport = "iso15765,CAN",
     .log_transport = "K-Line",
     .log_protocol = "SSM",
     .kernel = "a.bin",
     .kernel_load_address = 0xFFFF3000U,
     .description = "Protocol A"},
    {.name = "proto_b",
     .alias = "alias_b",
     .ecu = "ECU B",
     .mcu = "M32R",
     .mode = "OBD2",
     .checksum = ChecksumSupport::Missing,
     .read = true,
     .test_write = true,
     .write = true,
     .flash_transport = "K-Line",
     .log_transport = "K-Line",
     .log_protocol = "MUT_DMA",
     .kernel = "b.bin",
     .kernel_load_address = 0x0U,
     .description = "Protocol B"},
});

// Rows: 0 Subaru Impreza -> proto_a (shared with row 2), 1 Mitsubishi Colt ->
// proto_b, 2 Subaru Forester -> proto_a.
inline constexpr auto kStandardVehicles = std::to_array<VehicleSpec>({
    {.id = "subaru-impreza-v1",
     .make = "Subaru",
     .model = "Impreza",
     .version = "v1",
     .protocol = protocol_in(kStandardProtocols, "proto_a")},
    {.id = "mitsubishi-colt-v2",
     .make = "Mitsubishi",
     .model = "Colt",
     .version = "v2",
     .protocol = protocol_in(kStandardProtocols, "proto_b")},
    {.id = "subaru-forester-v3",
     .make = "Subaru",
     .model = "Forester",
     .version = "v3",
     .protocol = protocol_in(kStandardProtocols, "proto_a")},
});

inline constexpr Catalog kStandardCatalog{kStandardProtocols, kStandardVehicles};
static_assert(catalog_references_resolve(kStandardProtocols, kStandardVehicles));

inline std::string setting(std::string_view name, std::string_view data)
{
    return std::format(R"(<setting name="{}"><value data="{}"/></setting>)", name, data);
}

struct ConfigSessionFixture
{
    explicit ConfigSessionFixture(std::string_view root_path = kRoot)
        : root(root_path), paths(resolve_config_paths(root, kVersion))
    {
        put_settings("");
    }

    void put(const std::string& handle, std::string_view text)
    {
        file_repository.files[handle] = std::vector<std::uint8_t>(text.begin(), text.end());
    }
    std::string text(const std::string& handle) const
    {
        const std::vector<std::uint8_t>& bytes = file_repository.files.at(handle);
        return {bytes.begin(), bytes.end()};
    }
    void put_settings(std::string_view settings)
    {
        put(paths.config_file, std::format(R"(<?xml version="1.0"?><config name="FastECU" version="test">)"
                                           R"(<software_settings>{}</software_settings></config>)",
                                           settings));
    }
    Status initialize()
    {
        return session.initialize(root, kVersion);
    }

    std::string root;
    ConfigPaths paths;
    // The session reads this at initialize(); a test may assign another
    // catalog first.
    Catalog catalog = kStandardCatalog;
    InMemoryFileSystem file_system;
    InMemoryResourceBundle resource_bundle;
    InMemoryFileRepository file_repository;
    RecordingEventSink events;
    ConfigSession session{catalog, file_system, resource_bundle, file_repository, events};
};

} // namespace fastecu::config::testing
```

Update the fixture BUILD comment to `# Config-package-owned test fixture: a ConfigSession over in-memory ports and a small, fully known catalog.` and let Gazelle add `//src/backend/config:catalog` to its deps.

- [ ] **Step 2: Update the session tests**

In `src/backend/config/config_session_test.cpp`:

- Remove the `using` declarations of `kMissingProtocolField`, `protocol_field_or_placeholder` and `ProtocolEntry`; add `using fastecu::config::builtin_catalog;`.
- Delete these tests: `MissingProtocolsFileFailsNamingIt`, `EmptyVehicleCatalogIsRejected`, `UnresolvedReferenceStaysNullopt`, `UnresolvedRowSelectsWithPlaceholders`, `PlaceholderHelperReturnsResolvedFields`.
- In `FailedInitializationExposesNothing` and `AFailedReinitializationDropsTheEarlierState`, replace `f.put_protocols(R"(<config name="FastECU"><protocols/><car_models/></config>)");` with `f.put(f.paths.config_file, "<config");`.
- In the `InvalidSavedId` instantiation, change the comment to `// The fixture has three rows, so "3" is the first out-of-range id.` and the value `"4"` to `"3"`.
- `KeepFileOrder` → rename `KeepCatalogOrder`; expect 3 rows (Impreza, Colt, Forester) and drop the Skyline line.
- Replace `SharedProtocolRowsResolveToTheSameEntry` with:

```cpp
TEST(ConfigSessionVehicles, SharedProtocolRowsPointAtTheSameEntry)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.vehicles()[0].protocol, f.session.vehicles()[2].protocol);
    EXPECT_EQ(f.session.vehicles()[0].protocol->description, "Protocol A");
}
```

- In `InvalidRowFailsWithoutChangingSettings`, change `select_row(4)` to `select_row(3)`.
- Add these tests:

```cpp
TEST(ConfigSessionInitialize, AStaleProtocolsFileIsIgnored)
{
    ConfigSessionFixture f;
    f.put(f.paths.config_files_directory + "protocols.cfg", "<config><protocols/><car_models/></config>");
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.vehicles().size(), 3U);
    EXPECT_EQ(f.file_repository.read_count(f.paths.config_files_directory + "protocols.cfg"), 0);
}

TEST(ConfigSessionSelect, AliasFindsTheFirstVehicleOfItsProtocol)
{
    ConfigSessionFixture f;
    EXPECT_EQ(f.session.vehicle_for_alias("alias_a"), nullptr); // not initialized
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.vehicle_for_alias("alias_a"), &f.session.vehicles()[0]);
    EXPECT_EQ(f.session.vehicle_for_alias("alias_b"), &f.session.vehicles()[1]);
    EXPECT_EQ(f.session.vehicle_for_alias("absent"), nullptr);
}

TEST(ConfigSessionBuiltin, TheMutDmaVehicleLogsMutDmaAndOffersNoFlashOperation)
{
    ConfigSessionFixture f;
    f.catalog = builtin_catalog();
    ASSERT_THAT(f.initialize(), IsOk());
    const auto row = builtin_catalog().find_vehicle("mitsubishi-unknown-unk-ecu-unk--mitsu-ecu-m32r-kline-mut-dma");
    ASSERT_TRUE(row.has_value());

    ASSERT_THAT(f.session.select_row(*row), IsOk());

    EXPECT_EQ(f.session.settings().selected_log_protocol, "MUT_DMA");
    const auto& protocol = *f.session.selected_vehicle()->protocol;
    EXPECT_FALSE(protocol.read);
    EXPECT_FALSE(protocol.test_write);
    EXPECT_FALSE(protocol.write);
}
```

Run: `python3 scripts/gazelle_check.py --fix && bazel test --config=release //src/backend/config:config_session_test`
Expected: FAIL to compile (constructor arity, `vehicle_for_alias`, `VehicleSpec`).

- [ ] **Step 3: Move the session onto the catalog**

`config_session.h`:
- Replace `#include "src/backend/config/car_model_catalog.h"` with `#include "src/backend/config/catalog.h"`; drop `<string>` and `<vector>` if nothing else needs them.
- Delete `kMissingProtocolField` and `protocol_field_or_placeholder`.
- Change the class comment's last sentence to: `everything about the selected vehicle is read from its VehicleSpec rather than cached.`
- Constructor: `ConfigSession(const Catalog& catalog, IFileSystem& file_system, IResourceBundle& resource_bundle, IFileRepository& file_repository, IEventSink& events);` with the comment `// \`catalog\` must outlive the session; initialize() reads it.`
- `initialize` comment: `// Provisions <app_root>/<version>/, loads settings, and validates the saved row against the catalog (an invalid one becomes "0"). ...` (keep the rest).
- `vehicles()`: `// Catalog order; a row's id is its position. Empty until initialized.` returning `std::span<const VehicleSpec>`.
- `selected_vehicle()` returns `const VehicleSpec *`.
- `select_row` comment: `// Makes \`row\` the saved row and sets the logging protocol from its protocol. ...`
- Add after `select_by_protocol_name`:

```cpp
    // The first vehicle whose protocol's alias is `flash_method`, which is how
    // a definition's flash method resolves; nullptr when none matches or the
    // session is not initialized.
    const VehicleSpec *vehicle_for_alias(std::string_view flash_method) const;
```

- Private members: add `const Catalog& catalog_;` first; delete `std::vector<ResolvedCarModel> vehicles_;`.

`config_session.cpp`:
- Remove `#include "src/backend/config/protocol_catalog.h"` and the definition of `protocol_field_or_placeholder`.
- Constructor initializer list starts with `catalog_(catalog)`.
- In `initialize`, delete `vehicles_.clear();`, the whole block from `Result<ProtocolCatalog> protocols = ...` through the `"No vehicles defined"` check, and `vehicles_ = std::move(vehicles);`. The row validation becomes:

```cpp
    if (!parse_row(settings.selected_protocol_id, catalog_.vehicles().size()).has_value())
    {
        settings.selected_protocol_id = "0";
    }
```

- Replace `vehicles()`, `selected_row()`, `selected_vehicle()`, `select_row()` and `select_by_protocol_name()` and add `vehicle_for_alias()`:

```cpp
std::span<const VehicleSpec> ConfigSession::vehicles() const
{
    return initialized_ ? catalog_.vehicles() : std::span<const VehicleSpec>{};
}

Result<std::size_t> ConfigSession::selected_row() const
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    const std::optional<std::size_t> row = parse_row(settings_.selected_protocol_id, catalog_.vehicles().size());
    if (!row.has_value())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("selected protocol id '{}' names no vehicle", settings_.selected_protocol_id));
    }
    return *row;
}

const VehicleSpec *ConfigSession::selected_vehicle() const
{
    const Result<std::size_t> row = selected_row();
    return row.has_value() ? &catalog_.vehicles()[*row] : nullptr;
}

Status ConfigSession::select_row(std::size_t row)
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    const std::span<const VehicleSpec> vehicles = catalog_.vehicles();
    if (row >= vehicles.size())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("vehicle row {} is out of range ({} rows)", row, vehicles.size()));
    }
    settings_.selected_protocol_id = std::to_string(row);
    settings_.selected_log_protocol = std::string(vehicles[row].protocol->log_protocol);
    return {};
}

bool ConfigSession::select_by_protocol_name(std::string_view protocol_name)
{
    if (!initialized_)
    {
        return false;
    }
    const std::optional<std::size_t> row = catalog_.last_vehicle_for_protocol(protocol_name);
    return row.has_value() && select_row(*row).has_value();
}

const VehicleSpec *ConfigSession::vehicle_for_alias(std::string_view flash_method) const
{
    return initialized_ ? catalog_.first_vehicle_for_alias(flash_method) : nullptr;
}
```

BUILD: run Gazelle; `config_session` must depend on `:catalog` (public) and no longer on `:car_model_catalog` or `:protocol_catalog`.

Run: `python3 scripts/gazelle_check.py --fix && bazel test --config=release //src/backend/config:all`
Expected: PASS.

- [ ] **Step 4: ROM open reads the catalog through the session**

In `src/backend/calibration/session/rom_open.cpp`:

Replace the checksum and MCU lookups in `finish` (the block starting `outcome.vehicle_selected = ...`) with:

```cpp
    outcome.vehicle_selected = config_.select_by_protocol_name(flash_method);
    const config::VehicleSpec *vehicle = config_.selected_vehicle();
    if (vehicle != nullptr)
    {
        switch (vehicle->protocol->checksum)
        {
        case config::ChecksumSupport::Corrected:
            checksum_module = checksum_module_for(flash_method);
            break;
        case config::ChecksumSupport::Missing:
            checksum_module = "Not implemented yet";
            break;
        case config::ChecksumSupport::None:
            checksum_module = "No checksums";
            break;
        }
    }
```

and the `.mcu_type` initializer with `.mcu_type = vehicle != nullptr ? std::string(vehicle->protocol->mcu) : std::string{},`.

Replace `resolve_alias` and delete the now-unused `alias_list_contains` helper:

```cpp
std::string RomOpenUseCase::resolve_alias(const std::string& flash_method)
{
    const config::VehicleSpec *vehicle = config_.vehicle_for_alias(flash_method);
    if (vehicle == nullptr)
    {
        return flash_method;
    }
    events_.log(LogLevel::Debug, std::format("Alias: {}", flash_method));
    events_.log(LogLevel::Debug, std::format("Protocol: {}", vehicle->protocol->name));
    return std::string(vehicle->protocol->name);
}
```

Run: `bazel test --config=release //src/backend/calibration/...`
Expected: PASS.

- [ ] **Step 5: UI display helpers**

Replace the declarations in `src/ui/desktop/config_fields.h` (include `src/backend/config/catalog.h` instead of the two catalog headers):

```cpp
// A text field of the vehicle's protocol, for display.
QString protocol_field(const config::VehicleSpec& vehicle, std::string_view config::ProtocolSpec::*field);

// "yes" or "no" for a capability, as the vehicle chooser has always shown it.
QString protocol_flag(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability);

// Whether the vehicle's protocol offers a capability.
bool protocol_capability(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability);

// "yes", "n/a" or "no".
QString checksum_field(const config::VehicleSpec& vehicle);

// "0xFFFF3000", or empty when the protocol uploads no kernel.
QString kernel_address_field(const config::VehicleSpec& vehicle);
```

Replace the definitions in `config_fields.cpp` (drop the `config_session.h` include):

```cpp
QString protocol_field(const config::VehicleSpec& vehicle, std::string_view config::ProtocolSpec::*field)
{
    return qs(vehicle.protocol->*field);
}

QString protocol_flag(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability)
{
    return protocol_capability(vehicle, capability) ? QStringLiteral("yes") : QStringLiteral("no");
}

bool protocol_capability(const config::VehicleSpec& vehicle, bool config::ProtocolSpec::*capability)
{
    return vehicle.protocol->*capability;
}

QString checksum_field(const config::VehicleSpec& vehicle)
{
    return qs(config::checksum_flag(vehicle.protocol->checksum));
}

QString kernel_address_field(const config::VehicleSpec& vehicle)
{
    return QString::fromStdString(config::kernel_load_address_text(*vehicle.protocol));
}
```

Replace `src/ui/desktop/config_fields_test.cpp`'s first two tests with:

```cpp
using fastecu::config::ProtocolSpec;
using fastecu::config::testing::ConfigSessionFixture;

TEST(ConfigFields, FieldsAndCapabilities)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    const auto& impreza = f.session.vehicles()[0];
    EXPECT_EQ(fastecu::ui::protocol_field(impreza, &ProtocolSpec::mcu), QString("SH7058"));
    EXPECT_TRUE(fastecu::ui::protocol_capability(impreza, &ProtocolSpec::read));
    EXPECT_FALSE(fastecu::ui::protocol_capability(impreza, &ProtocolSpec::test_write));
    EXPECT_EQ(fastecu::ui::protocol_flag(impreza, &ProtocolSpec::write), QString("yes"));
    EXPECT_EQ(fastecu::ui::protocol_flag(impreza, &ProtocolSpec::test_write), QString("no"));
    EXPECT_EQ(fastecu::ui::checksum_field(impreza), QString("yes"));
    EXPECT_EQ(fastecu::ui::checksum_field(f.session.vehicles()[1]), QString("n/a"));
    EXPECT_EQ(fastecu::ui::kernel_address_field(impreza), QString("0xFFFF3000"));
}
```

(keep `ListConversionsRoundTrip`).

- [ ] **Step 6: Migrate the remaining readers**

Apply this table in `vehicle_select.cpp`, `protocol_select.cpp`, `mainwindow.h`, `mainwindow.cpp`, `calibration_operation_coordinator.cpp`, their tests, and any file `bazel build --config=release //...` reports. Find sites with `git grep -n -E 'ResolvedCarModel|ProtocolEntry|protocol_name|protocol_field_or_placeholder|kNoChecksumModule|protocols_file' -- src apps tests` (leave `flash_workflow.cpp`, `eeprom_read_plan.*` and their tests for Task 8).

| Old | New |
| --- | --- |
| `ResolvedCarModel` | `VehicleSpec` |
| `ProtocolEntry` (as `&ProtocolEntry::x`, or `using ...::ProtocolEntry`) | `ProtocolSpec` |
| `vehicle.protocol_name` | `vehicle.protocol->name` — `qs(...)` in UI code, `std::string(...)` where a `std::string` is required |
| `protocol_field_or_placeholder(v, &ProtocolEntry::x)` for a text field | `std::string(v.protocol->x)` |
| `protocol_field_or_placeholder(v, &ProtocolEntry::checksum)` (backend or test code) | `std::string(fastecu::config::checksum_flag(v.protocol->checksum))` |
| `protocol_field(v, &ProtocolEntry::checksum)` | `checksum_field(v)` |
| `protocol_field(v, &ProtocolEntry::read)` / `write` / `test_write` | `protocol_flag(v, &ProtocolSpec::read)` / … |
| `protocol_field(v, &ProtocolEntry::kernel_addr)` | `kernel_address_field(v)` |
| `selected_field(config_, &ProtocolEntry::checksum) == kNoChecksumModule` | `selected_protocol(config_).checksum == config::ChecksumSupport::Missing` |
| `const fastecu::config::ResolvedCarModel& MainWindow::selected_vehicle() const` | `const fastecu::config::VehicleSpec& MainWindow::selected_vehicle() const` (same in `mainwindow.h`) |

`calibration_operation_coordinator.cpp`: delete `kNoChecksumModule` and `selected_field`; add next to `selected_vehicle`:

```cpp
const config::ProtocolSpec& selected_protocol(const config::ConfigSession& config)
{
    return *selected_vehicle(config).protocol;
}
```

and use it: `callbacks_.protocol_description_changed(selected_protocol(config_).description);`, `protocol.kernel_path = flash::kernel_path(kernel_directory, selected_protocol(config_).kernel);`, `protocol.kernel_start_address = config::kernel_load_address_text(selected_protocol(config_));`, `protocol.mcu_type = std::string(selected_protocol(config_).mcu);`, `.protocol = std::string(selected_protocol(config_).name),`, `protocol.flash_method = std::string(selected_protocol(config_).name);`. In `correct_operation_image` build the selection as:

```cpp
    const checksum::ChecksumSelection selection{
        .make = std::string(vehicle.make),
        .checksum_flag = std::string(config::checksum_flag(vehicle.protocol->checksum)),
        .flash_method = std::string(vehicle.protocol->name),
        .mcu_type = session.protocol().mcu_type,
        .rom_id = session.protocol().rom_id,
    };
```

and log `std::format("ecuCalDef->McuType: {} {}", session.protocol().mcu_type, vehicle.protocol->mcu)`.

`mainwindow.cpp` read path: `read_kernel_path = QString::fromStdString(fastecu::flash::kernel_path(kernel_dir.toStdString(), selected_vehicle().protocol->kernel));` and `read_kernel_address = kernel_address_field(selected_vehicle());`. The flash request still passes strings (`.protocol = ... std::string(selected_vehicle().protocol->name)`, `.mcu = ...`) until Task 8.

`protocol_select_test.cpp`: in `listsEachVehicleBackedProtocolOnce` expect `2` with comment `// proto_a (two rows) and proto_b.`

`calibration_operation_coordinator_test.cpp`: `start` takes `const config::Catalog& catalog`, sets `cfg.catalog = catalog;` before `cfg.initialize()`; both callers pass `config::testing::kStandardCatalog`. Replace the string surgery in `EmptyDefinedMethodReselectsBeforeChecksum` with a namespace-scope catalog:

```cpp
// The standard catalog with row 2 made a Nissan.
constexpr auto kNissanForesterVehicles = std::to_array<config::VehicleSpec>({
    config::testing::kStandardVehicles[0],
    config::testing::kStandardVehicles[1],
    {.id = "nissan-forester-v3",
     .make = "Nissan",
     .model = "Forester",
     .version = "v3",
     .protocol = config::protocol_in(config::testing::kStandardProtocols, "proto_a")},
});
constexpr config::Catalog kNissanForesterCatalog{config::testing::kStandardProtocols, kNissanForesterVehicles};
```

and call `start(kNissanForesterCatalog)`.

`mainwindow_test.cpp`: the suite writes a synthetic `protocols.cfg` in `SetUpTestSuite`. Convert it once with the generator:

```bash
awk '/writeTextFile\(config_dir \+ "protocols.cfg"/{f=1;next} f&&/^\)"\)\);/{f=0} f' src/ui/desktop/widgets/mainwindow_test.cpp | sed '1s/^ *R"(//' > "$TMPDIR/window_protocols.xml"
python3 "$TMPDIR/generate_catalog.py" "$TMPDIR/window_protocols.xml" Window > "$TMPDIR/window_arrays.inc"
```

Check `window_arrays.inc` contains every `<protocol>` and `<car_model>` of that block, paste it into the test's anonymous namespace before `struct TestServices` after these declarations, and append the catalog:

```cpp
using fastecu::config::ChecksumSupport;
using fastecu::config::protocol_in;
using fastecu::config::ProtocolSpec;
using fastecu::config::VehicleSpec;

// <paste window_arrays.inc here>

constexpr fastecu::config::Catalog kWindowCatalog{kWindowProtocols, kWindowVehicles};
```

Delete the `writeTextFile(config_dir + "protocols.cfg", ...)` call, add `ASSERT_TRUE(fastecu::config::catalog_problems(kWindowCatalog).empty());` at the start of `SetUpTestSuite` (if a synthetic protocol has no vehicle, delete that protocol: the session could never have selected it), and construct the session as `fastecu::config::ConfigSession config{kWindowCatalog, file_system, resource_bundle, file_repository, config_events};`. Apply the rename table to the rest of the file.

- [ ] **Step 7: The composition root passes the built-in catalog**

`apps/desktop/desktop_composition.cpp`: `#include "src/backend/config/builtin_catalog.h"` and construct `config_(fastecu::config::builtin_catalog(), file_system_, resource_bundle_, file_repository_, startup_events_)`.

`apps/desktop/desktop_composition_test.cpp`, `failedStartupBuildsNoServicesAndPerformsNoEcuIo`: replace the `protocols.cfg` write with `ASSERT_TRUE(writeFile(config_dir + "fastecu.cfg", "<config"));` and the path assertion with `ASSERT_TRUE(QString::fromStdString(composition.startup_error()->detail).contains(config_dir + "fastecu.cfg"));`. Keep every "nothing was created" assertion.

- [ ] **Step 8: Build and test everything**

Run: `python3 scripts/gazelle_check.py --fix && bazel build --config=release //... && bazel test --config=release //...`
Expected: PASS. Then the portable-core check from Global Constraints.

- [ ] **Step 9: Commit**

```bash
prek run --all-files
git add -A src apps tests
git commit -m "refactor(config): read vehicles and protocols from the catalog instead of protocols.cfg"
```

---

### Task 8: Flash and EEPROM take the protocol from the caller

**Files:**
- Modify: `src/platform/desktop/common/flash/flash_workflow.h`, `flash_workflow.cpp`, `flash_workflow_test.cpp`, `BUILD.bazel`
- Modify: `src/ui/desktop/flash/operation/flash_operation_controller.h`, `.cpp`, `flash_operation_controller_test.cpp`
- Modify: `src/ui/desktop/calibration/calibration_operation_coordinator.h`, `.cpp`, `calibration_operation_coordinator_test.cpp`
- Modify: `src/ui/desktop/widgets/mainwindow.cpp` (flash request)
- Modify: `src/backend/flash/eeprom/eeprom_read_plan.h`, `.cpp`, `eeprom_read_plan_test.cpp`, `eeprom_read_plan_goldens_test.cpp`, `BUILD.bazel`
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor_test.cpp:369-391`
- Delete: `src/backend/config/protocol_catalog.{h,cpp}`, `protocol_catalog_test.cpp`, `car_model_catalog.{h,cpp}`, `car_model_catalog_test.cpp`, `protocols_document.{h,cpp}`, `protocols_document_test.cpp`

**Interfaces:**
- Consumes: `ProtocolSpec`, `kernel_load_address_text`.
- Produces:
  - `struct FlashWorkflowRequest { FlashOperation operation; config::ProtocolSpec protocol; std::optional<bytes::Bytes> image; config::ConfigPaths paths; std::string display_filename; SerialPortActions *serial = nullptr; };`
  - `struct FlashOperationInput { FlashOperation operation; config::ProtocolSpec protocol; std::string kernel_path; std::optional<bytes::Bytes> image; config::ConfigPaths paths; std::string display_filename; };`
  - `struct PreparedWrite { bytes::Bytes image; config::ProtocolSpec protocol; std::string kernel_path; std::string display_filename; };`
  - `Result<FlashPlan> build_eeprom_read_plan(const config::ConfigPaths& paths, const config::ProtocolSpec& protocol, EepromReadMode mode, IFileRepository& file_repository);`

- [ ] **Step 1: Update the EEPROM plan tests first**

In `eeprom_read_plan_test.cpp`, replace `kFixture`, `make_repository` and `make_repository_with_protocol` with specs and a kernel-only repository:

```cpp
constexpr config::ProtocolSpec kKline{.name = "sub_ecu_eeprom_denso_sh7055_kline",
                                      .mcu = "SH7055",
                                      .kernel = "ssmk_kline_sh7055.bin",
                                      .kernel_load_address = 0xFFFF6004U};
constexpr config::ProtocolSpec kCan{.name = "sub_ecu_eeprom_denso_sh7058_can",
                                    .mcu = "SH7058",
                                    .kernel = "ssmk_can_tp_sh7058.bin",
                                    .kernel_load_address = 0xFFFF3000U};
constexpr config::ProtocolSpec kKlineCobb{.name = "sub_ecu_eeprom_denso_sh7055_kline_cobb",
                                          .mcu = "SH7055",
                                          .kernel = "ssmk_kline_sh7055.bin",
                                          .kernel_load_address = 0xFFFF6004U};
constexpr config::ProtocolSpec kBadKernelAddress{.name = "sub_ecu_eeprom_denso_sh7055_bad_kernel_addr",
                                                 .mcu = "SH7055",
                                                 .kernel = "out_of_range.bin",
                                                 .kernel_load_address = 0xFFFF0000U};

config::ConfigPaths test_paths()
{
    config::ConfigPaths paths;
    paths.kernel_files_directory = "kernels/";
    return paths;
}

InMemoryFileRepository make_repository()
{
    InMemoryFileRepository repository;
    repository.files["kernels/ssmk_kline_sh7055.bin"] = {0xaa, 0xbb};
    repository.files["kernels/ssmk_can_tp_sh7058.bin"] = {0x01, 0x02, 0x03};
    return repository;
}

// A protocol like kCan with its own name, MCU and kernel address, and a
// three-byte kernel.
config::ProtocolSpec synthetic_protocol(std::string_view name, std::string_view mcu,
                                        std::optional<std::uint32_t> kernel_load_address)
{
    return {.name = name, .mcu = mcu, .kernel = "synthetic.bin", .kernel_load_address = kernel_load_address};
}
```

Then:
- Every `build_eeprom_read_plan(test_paths(), "<name>", mode, repository)` passes the matching constant (`kKline`, `kCan`, `kKlineCobb`, `kBadKernelAddress`) instead of the name; tests that called `make_repository_with_protocol(name, mcu, kernel, addr)` use `synthetic_protocol(name, mcu, parsed address)` and put `{0x01, 0x02, 0x03}` at `kernels/synthetic.bin`.
- Delete `ProtocolWithNoCarModelIsRejected`, `UnknownProtocolNameIsRejected`, `CarModelReferencingAbsentProtocolIsRejectedBeforeReadingTheKernel` and `MissingProtocolsFileIsPropagated`: protocol resolution is the caller's now.
- Replace `UnparseableKernelAddrIsRejectedBeforeReadingTheKernel` with:

```cpp
TEST(BuildEepromReadPlanTest, MissingKernelLoadAddressIsRejectedBeforeReadingTheKernel)
{
    InMemoryFileRepository repository = make_repository();
    repository.files["kernels/synthetic.bin"] = {0x01, 0x02, 0x03};
    ASSERT_THAT(build_eeprom_read_plan(test_paths(),
                                       synthetic_protocol("sub_ecu_eeprom_denso_sh7058_can", "SH7058", std::nullopt),
                                       EepromReadMode::Mode2, repository),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(repository.read_count("kernels/synthetic.bin"), 0);
}
```

Do the same conversion in `eeprom_read_plan_goldens_test.cpp`, and in `subaru_denso_mc68hc16y5_02_executor_test.cpp` replace `different_family_plan()` with:

```cpp
Result<FlashPlan> different_family_plan()
{
    InMemoryFileRepository files;
    files.files["kernels/kernel.bin"] = {0x01, 0x02, 0x03, 0x04};
    return build_eeprom_read_plan({.kernel_files_directory = "kernels/"},
                                  config::ProtocolSpec{.name = "sub_ecu_eeprom_denso_sh7055_kline",
                                                       .mcu = "SH7055",
                                                       .kernel = "kernel.bin",
                                                       .kernel_load_address = 0xFFFF6004U},
                                  EepromReadMode::Mode2, files);
}
```

and delete `eeprom_paths()`.

Run: `bazel test --config=release //src/backend/flash/eeprom:all //src/backend/flash/ecu:subaru_denso_mc68hc16y5_02_executor_test`
Expected: FAIL to compile (`build_eeprom_read_plan` takes a name).

- [ ] **Step 2: Build the EEPROM plan from the spec**

`eeprom_read_plan.h`: `#include "src/backend/config/catalog.h"` and replace the long comment and declaration with:

```cpp
// Builds a Denso SH705x EEPROM read plan for `protocol`, the selected
// vehicle's protocol. Owns no state; the returned FlashPlan owns its kernel
// bytes by value.
//
// The caller resolves the protocol from the selected vehicle, so only a
// vehicle-backed protocol ever reaches here -- the guarantee the former
// car-model lookup gave, now carried by the catalog's invariant that every
// protocol has a vehicle.
//
// Every fallible validation decidable from the protocol runs before the
// kernel read, so an invalid mode, security variant, MCU/region, or missing
// or definitely out-of-range kernel address is rejected without reading the
// kernel file. Validation that needs the kernel byte count runs after the
// read. This ordering is a guarantee, not an accident -- the tests assert it.
Result<FlashPlan> build_eeprom_read_plan(const config::ConfigPaths& paths, const config::ProtocolSpec& protocol,
                                         EepromReadMode mode, IFileRepository& file_repository);
```

`eeprom_read_plan.cpp`: remove the `car_model_catalog.h`, `protocol_catalog.h` and `text_format.h` includes, `parse_kernel_start_addr` and `resolve_protocol`. The start of `build_eeprom_read_plan` becomes:

```cpp
Result<FlashPlan> build_eeprom_read_plan(const config::ConfigPaths& paths, const config::ProtocolSpec& protocol,
                                         EepromReadMode mode, IFileRepository& file_repository)
{
    const std::string target_id(protocol.name);

    // Every fallible metadata-only validation runs before the kernel read below.
    if (!protocol.kernel_load_address.has_value())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("protocol '{}' declares no kernel load address", protocol.name));
    }
    Result<MemoryRegion> eeprom_region = resolve_sh705x_eeprom_region(protocol.mcu);
```

Use `protocol.name` where `protocol_name` was used (`family_for_protocol`, `security_for_protocol`), `std::string(protocol.mcu)` for `.mcu_name`, `*protocol.kernel_load_address` for `.load_address`, and `paths.kernel_files_directory + std::string(protocol.kernel)` for the kernel handle. Gazelle drops the catalog deps.

Run: `python3 scripts/gazelle_check.py --fix && bazel test --config=release //src/backend/flash/...`
Expected: PASS.

- [ ] **Step 3: The workflow request carries the spec**

`flash_workflow.h`: `#include "src/backend/config/catalog.h"`; the request becomes the struct in **Interfaces** above.

`flash_workflow.cpp`:
- Remove `#include "src/backend/config/protocol_catalog.h"`, `parseKernelStartAddress` and `resolveProtocol`; remove `#include "src/backend/definition/text_format.h"` if nothing else in the file uses it.
- Replace `resolveKernel` and `resolveKernelBytes`:

```cpp
Result<KernelImage> resolveKernel(const FlashWorkflowRequest& request, IFileRepository& repository)
{
    if (!request.protocol.kernel_load_address.has_value())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("protocol '{}' declares no kernel load address", request.protocol.name));
    }
    Result<std::vector<std::uint8_t>> kernel_bytes =
        repository.read(request.paths.kernel_files_directory + std::string(request.protocol.kernel));
    if (!kernel_bytes.has_value())
    {
        return std::unexpected(kernel_bytes.error());
    }
    return KernelImage{.id = std::format("{}-kernel", request.protocol.name),
                       .load_address = *request.protocol.kernel_load_address,
                       .bytes = std::move(*kernel_bytes)};
}

// The kernel file alone. The Unisia Jecs M32R _bootmode entries declare no
// kernel load address: the M32R boot ROM places the kernel itself.
Result<bytes::Bytes> resolveKernelBytes(const FlashWorkflowRequest& request, IFileRepository& repository)
{
    Result<std::vector<std::uint8_t>> kernel_bytes =
        repository.read(request.paths.kernel_files_directory + std::string(request.protocol.kernel));
    if (!kernel_bytes.has_value())
    {
        const Error& error = kernel_bytes.error();
        return fail(error.kind, std::format("kernel file '{}': {}", request.protocol.kernel, error.detail));
    }
    return bytes::Bytes(kernel_bytes->begin(), kernel_bytes->end());
}
```

- Everywhere else: `request.protocol` / `request_.protocol` used as a name → `.protocol.name`; `request.mcu` / `request_.mcu` → `.protocol.mcu`; `request.protocol + "-kernel"` → `std::format("{}-kernel", request.protocol.name)`. `build_eeprom_read_plan(request_.paths, request_.protocol, mode_, repository)` is unchanged text and now passes the spec.

`flash_workflow_test.cpp`:
- Replace `request()` and add the kernel table (the XML in `catalogPaths` declared these):

```cpp
// The kernel each protocol declared in the synthetic catalog these tests
// used to write as protocols.cfg; catalogPaths() writes the files.
struct CatalogKernel
{
    std::string_view protocol;
    std::string_view file;
    std::optional<std::uint32_t> load_address;
};

constexpr auto kCatalogKernels = std::to_array<CatalogKernel>({
    {"sub_ecu_denso_mc68hc16y5_02", "catalog_mc68.bin", 0x20000U},
    {"sub_ecu_denso_mc68hc16y5_02_tpu", "catalog_tpu.bin", 0x20000U},
    {"sub_ecu_denso_mc68hc16y5_02_bdm", "catalog_mc68.bin", 0x20000U},
    {"sub_ecu_denso_sh7055_02", "catalog_sh7055.bin", 0xFFFF6004U},
    {"sub_ecu_denso_sh7055_02_ecutek", "catalog_sh7055.bin", 0xFFFF6004U},
    {"sub_ecu_denso_sh7055_densocan", "catalog_densocan.bin", 0xFFFF6004U},
    {"sub_tcu_denso_sh7055_can", "catalog_tcu_sh7055.bin", 0xFFFF9000U},
    {"sub_tcu_denso_sh7058_can", "catalog_tcu_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_ecutek", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_cobb", "catalog_petrol_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_can_diesel", "catalog_diesel_sh7058.bin", 0xFFFF4000U},
    {"sub_ecu_denso_sh7059_can_diesel", "catalog_diesel_sh7059.bin", 0xFFFEE000U},
    {"sub_ecu_denso_sh7055_04", "catalog_kline_sh7055.bin", 0xFFFF6004U},
    {"sub_ecu_denso_sh7058_ecutek", "catalog_kline_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_denso_sh7058_cobb", "catalog_kline_sh7058.bin", 0xFFFF3000U},
    {"sub_ecu_unisia_jecs_20_bootmode", "catalog_uj20_bootmode.bin", std::nullopt},
    {"sub_ecu_unisia_jecs_30_bootmode", "catalog_uj30_bootmode.bin", std::nullopt},
});

// `protocol` must outlive the request: pass a literal.
FlashWorkflowRequest request(std::string_view protocol, FlashOperation operation = FlashOperation::Read)
{
    config::ProtocolSpec spec{.name = protocol, .mcu = "M32R_384KB_1block"};
    if (const auto kernel = std::ranges::find(kCatalogKernels, protocol, &CatalogKernel::protocol);
        kernel != kCatalogKernels.end())
    {
        spec.kernel = kernel->file;
        spec.kernel_load_address = kernel->load_address;
    }
    return {.operation = operation,
            .protocol = spec,
            .image = std::nullopt,
            .paths = {},
            .display_filename = "test.bin",
            .serial = nullptr};
}
```

- `catalogPaths`: delete the `kCatalog` XML, the `protocols.cfg` write and `paths.protocols_file = ...`; keep the kernel directory and kernel files.
- Delete every `ASSERT_TRUE(QFile::remove(QString::fromStdString(paths->protocols_file)));` line (deleting the kernel file still proves the snapshot).
- `input.mcu = ...` → `input.protocol.mcu = ...`; `request.mcu` → `request.protocol.mcu`; any literal request `.protocol = "x", .mcu = "y"` → `.protocol = config::ProtocolSpec{.name = "x", .mcu = "y"}`. Check no `request(...)` call passes a temporary `std::string` (`git grep -n 'request(std::string' src/platform/desktop/common/flash/flash_workflow_test.cpp` must print nothing).

- [ ] **Step 4: The UI hands the selected protocol to flash**

`flash_operation_controller.h`: `#include "src/backend/config/catalog.h"`; `FlashOperationInput` per **Interfaces**. In `.cpp`: `is_denso_tcu_protocol(input.protocol.name)`, the log line and the warning use `QString::fromUtf8(input.protocol.name.data(), static_cast<qsizetype>(input.protocol.name.size()))`, `run_denso_tcu_service_action(action, &serial_, std::string(input.protocol.name), dialog_parent_)`, and `tryCreate({.operation = input.operation, .protocol = input.protocol, .image = input.image, .paths = input.paths, .display_filename = input.display_filename, .serial = &serial_})`. In its test, `.protocol = config::ProtocolSpec{.name = "...", .mcu = "SH7058"}` replaces the `.protocol`/`.mcu` pair.

`calibration_operation_coordinator.h`: `PreparedWrite` per **Interfaces** (include `catalog.h`). In `prepare_write`: `.protocol = selected_protocol(config_),` replaces `.protocol` and `.mcu`. Tests: `prepared->protocol` → `prepared->protocol.name`, `prepared->mcu` → `prepared->protocol.mcu`.

`mainwindow.cpp` (`run` of the flash command): delete `read_mcu`; build the input with `.protocol = prepared_write.has_value() ? prepared_write->protocol : *selected_vehicle().protocol,` and no `.mcu`.

- [ ] **Step 5: Delete the loaders**

```bash
git rm src/backend/config/protocol_catalog.h src/backend/config/protocol_catalog.cpp src/backend/config/protocol_catalog_test.cpp \
       src/backend/config/car_model_catalog.h src/backend/config/car_model_catalog.cpp src/backend/config/car_model_catalog_test.cpp \
       src/backend/config/protocols_document.h src/backend/config/protocols_document.cpp src/backend/config/protocols_document_test.cpp
```

Remove their targets and the `# Shared by the two catalogs ...` comment from `src/backend/config/BUILD.bazel` (Gazelle removes generated targets for deleted sources; delete any leftovers by hand), and remove `//src/backend/config:protocol_catalog` from `src/platform/desktop/common/flash/BUILD.bazel`.

`git grep -n -E 'protocol_catalog|car_model_catalog|protocols_document|ProtocolEntry|ResolvedCarModel' -- src apps tests` must print nothing.

- [ ] **Step 6: Build and test everything**

Run: `python3 scripts/gazelle_check.py --fix && bazel build --config=release //... && bazel test --config=release //...`
Expected: PASS. Then the portable-core check.

- [ ] **Step 7: Commit**

```bash
prek run --all-files
git add -A src apps tests
git commit -m "refactor(flash): carry the selected protocol in flash and EEPROM requests"
```

---

### Task 9: Capabilities agree with the workflows

**Files:**
- Create: `src/platform/desktop/common/flash/builtin_catalog_capability_test.cpp`
- Modify: `src/platform/desktop/common/flash/BUILD.bazel`
- Modify: `src/backend/flash/eeprom/eeprom_read_plan_test.cpp`, `src/backend/flash/eeprom/BUILD.bazel`
- Modify: `src/backend/config/BUILD.bazel` (`builtin_catalog` visibility), `resources/shared/BUILD.bazel` (`kernel_files` visibility)

**Interfaces:**
- Consumes: `builtin_catalog()`, `FlashWorkflowFactory::tryCreate`, `build_eeprom_read_plan`, `find_flash_device`, `//resources/shared:kernel_files`.
- Produces: tests only.

- [ ] **Step 1: Widen visibility for the two test packages**

In `src/backend/config/BUILD.bazel`, `builtin_catalog`'s `visibility` gains:

```starlark
        # One-off test consumers: each checks every built-in entry against its family.
        "//src/backend/flash/eeprom:__pkg__",
        "//src/platform/desktop/common/flash:__pkg__",
```

In `resources/shared/BUILD.bazel`, `kernel_files`' `visibility` gains the same two literals with the comment `# Tests that load the real kernels.`

- [ ] **Step 2: Write the workflow capability test**

Create `src/platform/desktop/common/flash/builtin_catalog_capability_test.cpp`:

```cpp
#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cstdlib>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "src/backend/config/builtin_catalog.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::flash
{
namespace
{

// A kernel directory holding every bundled kernel, from
// $(locations //resources/shared:kernel_files).
std::optional<config::ConfigPaths> bundledKernelPaths(const QTemporaryDir& directory)
{
    const QString kernel_directory = directory.filePath("kernels");
    const char *paths = std::getenv("KERNEL_FILES");
    if (paths == nullptr || !QDir().mkpath(kernel_directory))
    {
        return std::nullopt;
    }
    std::istringstream stream{paths};
    for (std::string path; stream >> path;)
    {
        const QString source = QString::fromStdString(path);
        if (!QFile::copy(source, kernel_directory + "/" + QFileInfo(source).fileName()))
        {
            return std::nullopt;
        }
    }
    config::ConfigPaths result;
    result.kernel_files_directory = (kernel_directory + "/").toStdString();
    return result;
}

// Erased flash of the MCU's ROM size: right for most families, and never a
// reason for an Unsupported rejection.
std::optional<bytes::Bytes> imageFor(const config::ProtocolSpec& protocol, FlashOperation operation)
{
    if (operation == FlashOperation::Read)
    {
        return std::nullopt;
    }
    const flashdev_t *device = find_flash_device(protocol.mcu);
    return bytes::Bytes(device != nullptr ? device->romsize : 0U, 0xFF);
}

// The UI offers exactly the operations the catalog marks, so every one of
// them must reach the family, and every other one must be refused as
// Unsupported before any ECU I/O. A supported read must pass preflight,
// which proves the family accepts the entry's MCU, kernel and load address.
TEST(BuiltinCatalogCapability, EveryOfferedOperationIsAcceptedAndEveryOtherIsUnsupported)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const std::optional<config::ConfigPaths> paths = bundledKernelPaths(directory);
    ASSERT_TRUE(paths.has_value());

    for (const config::ProtocolSpec& protocol : config::builtin_catalog().protocols())
    {
        if (!protocol.read && !protocol.test_write && !protocol.write)
        {
            EXPECT_TRUE(FlashWorkflowFactory::tryCreate({.operation = FlashOperation::Read,
                                                          .protocol = protocol,
                                                          .paths = *paths}) == nullptr)
                << protocol.name << " offers no operation but has a route";
            continue;
        }
        for (const auto& [operation, offered] :
             {std::pair{FlashOperation::Read, protocol.read}, std::pair{FlashOperation::TestWrite, protocol.test_write},
              std::pair{FlashOperation::Write, protocol.write}})
        {
            SCOPED_TRACE(std::format("{} operation {}", protocol.name, static_cast<int>(operation)));
            auto workflow = FlashWorkflowFactory::tryCreate({.operation = operation,
                                                              .protocol = protocol,
                                                              .image = imageFor(protocol, operation),
                                                              .paths = *paths,
                                                              .display_filename = "capability.bin",
                                                              .serial = nullptr});
            ASSERT_TRUE(workflow != nullptr) << "no route";
            const FlashWorkflowStep step = workflow->next();
            const auto *failure = std::get_if<FlashFailureStep>(&step);
            if (!offered)
            {
                ASSERT_TRUE(failure != nullptr) << "an unoffered operation passed preflight";
                EXPECT_EQ(failure->error.kind, ErrorKind::Unsupported) << failure->error.detail;
            }
            else if (operation == FlashOperation::Read)
            {
                EXPECT_TRUE(failure == nullptr) << (failure != nullptr ? failure->error.detail : std::string{});
            }
            else if (failure != nullptr)
            {
                EXPECT_NE(failure->error.kind, ErrorKind::Unsupported) << failure->error.detail;
            }
        }
    }
}

} // namespace
} // namespace fastecu::flash

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
```

The registration is the one `flash_workflow_test.cpp` uses: the workflows read kernels through `QtFileRepository`.

Run `python3 scripts/gazelle_check.py --fix` and add to the generated `fastecu_gtest` target:

```starlark
    data = ["//resources/shared:kernel_files"],
    env = {"KERNEL_FILES": "$(locations //resources/shared:kernel_files)"},
```

- [ ] **Step 3: Run it**

Run: `bazel test --config=release //src/platform/desktop/common/flash:builtin_catalog_capability_test`
Expected: PASS. A failure is a real disagreement: if a workflow rejects an unoffered operation with a kind other than `Unsupported`, change that family's rejection to `Unsupported` (no other change to the check, and no check removed), with a test in the family's own package; if a supported read fails preflight, the catalog data or the family is wrong — fix data that is wrong, otherwise stop and report. Never weaken a validation.

- [ ] **Step 4: Every built-in EEPROM entry builds with its bundled kernel**

Append to `eeprom_read_plan_test.cpp` (add `#include <cstdlib>`, `<fstream>`, `<iterator>`, `<sstream>`, `"src/backend/config/builtin_catalog.h"`):

```cpp
// The bundled kernel file named `name`, from $(locations //resources/shared:kernel_files).
std::vector<std::uint8_t> bundled_kernel(std::string_view name)
{
    const char *paths = std::getenv("KERNEL_FILES");
    std::istringstream stream{paths == nullptr ? "" : paths};
    for (std::string path; stream >> path;)
    {
        if (path.ends_with("/" + std::string(name)))
        {
            std::ifstream file{path, std::ios::binary};
            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }
    }
    return {};
}

TEST(BuildEepromReadPlanTest, EveryBuiltinEepromProtocolBuildsWithItsBundledKernel)
{
    int checked = 0;
    for (const config::ProtocolSpec& protocol : config::builtin_catalog().protocols())
    {
        if (!protocol.name.starts_with("sub_ecu_eeprom_"))
        {
            continue;
        }
        InMemoryFileRepository repository;
        repository.files["kernels/" + std::string(protocol.kernel)] = bundled_kernel(protocol.kernel);
        ASSERT_FALSE(repository.files.begin()->second.empty()) << protocol.kernel;
        for (EepromReadMode mode : {EepromReadMode::Mode2, EepromReadMode::Mode3, EepromReadMode::Mode4})
        {
            EXPECT_THAT(build_eeprom_read_plan(test_paths(), protocol, mode, repository), fastecu::testing::IsOk())
                << protocol.name << " mode " << static_cast<int>(mode);
        }
        ++checked;
    }
    EXPECT_EQ(checked, 6);
}
```

If a family rejects one mode for a reason documented in its plan, restrict the loop to the modes that family supports and say so in a comment; do not change the plan.

Add `data` and `env` (same as Step 2) to `eeprom_read_plan_test` in `src/backend/flash/eeprom/BUILD.bazel`.

Run: `python3 scripts/gazelle_check.py --fix && bazel test --config=release //src/backend/flash/eeprom:eeprom_read_plan_test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
prek run --all-files
git add -A src resources/shared/BUILD.bazel
git commit -m "test(flash): check every built-in protocol's capabilities against its workflow"
```

---

### Task 10: A stable vehicle id and the startup vehicle gate

**Files:**
- Modify: `src/backend/config/app_config.h`, `app_config.cpp`, `app_config_test.cpp`
- Modify: `src/backend/config/config_session.h`, `config_session.cpp`, `config_session_test.cpp`
- Modify: `resources/shared/config/fastecu.cfg`
- Modify: `src/ui/desktop/widgets/vehicle_select.cpp`, `src/ui/desktop/widgets/mainwindow.cpp`, `src/ui/desktop/widgets/mainwindow_test.cpp`
- Create: `apps/desktop/startup_vehicle_gate.h`, `apps/desktop/startup_vehicle_gate.cpp`, `apps/desktop/startup_vehicle_gate_test.cpp`
- Modify: `apps/desktop/main.cpp`, `apps/desktop/BUILD.bazel`, `apps/desktop/desktop_composition_test.cpp`

**Interfaces:**
- Consumes: `Catalog::find_vehicle`, `ConfigSession`, `VehicleSelect`.
- Produces:
  - `AppConfig::selected_vehicle_id` (replaces `selected_protocol_id`), persisted as setting `vehicle_id`.
  - `using VehicleChooser = std::function<std::optional<std::size_t>()>;` and `std::optional<int> startup_vehicle_gate(fastecu::config::ConfigSession& session, const VehicleChooser& choose);` in `apps/desktop/startup_vehicle_gate.h`.

- [ ] **Step 1: Settings tests**

In `app_config_test.cpp`:
- Delete the `protocol_id` setting (three lines) from `kShippedDefaultConfig`.
- `EXPECT_EQ(config->selected_protocol_id, "35");` → `EXPECT_TRUE(config->selected_vehicle_id.empty());`
- In `SaveAppConfigThenLoadAppConfig.EveryOtherFieldRoundTrips`: `config.selected_vehicle_id = "subaru-impreza-v1";` and `EXPECT_EQ(reloaded->selected_vehicle_id, "subaru-impreza-v1");`
- Add:

```cpp
TEST(LoadAppConfig, ALegacyProtocolIdIsNotRead)
{
    InMemoryFileRepository repo;
    const ConfigPaths paths = test_paths();
    const std::string text = R"(<config name="FastECU"><software_settings>)"
                             R"(<setting name="protocol_id"><value data="35"/></setting>)"
                             R"(</software_settings></config>)";
    repo.files[paths.config_file] = std::vector<std::uint8_t>(text.begin(), text.end());

    auto config = load_app_config(paths, repo);

    ASSERT_THAT(config, fastecu::testing::IsOk());
    EXPECT_TRUE(config->selected_vehicle_id.empty());
}
```

Remove the `protocol_id` block from `resources/shared/config/fastecu.cfg` so it still matches `kShippedDefaultConfig`.

- [ ] **Step 2: Session tests**

In `config_session_test.cpp` (`InMemoryFileRepository::read_count` is used below; settings use the fixture's ids):
- Replace the `InvalidSavedId` suite with:

```cpp
class UnusableSavedVehicle : public ::testing::TestWithParam<std::string>
{
};

TEST_P(UnusableSavedVehicle, SelectsNothingAndForgetsIt)
{
    ConfigSessionFixture f;
    f.put_settings(setting("vehicle_id", GetParam()));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.selected_vehicle(), nullptr);
    EXPECT_THAT(f.session.selected_row(), IsErr(ErrorKind::InvalidConfig));
    EXPECT_TRUE(f.session.settings().selected_vehicle_id.empty());
}

INSTANTIATE_TEST_SUITE_P(ConfigSessionInitialize, UnusableSavedVehicle,
                         ::testing::Values("", "0", "subaru-impreza-v9", "SUBARU-IMPREZA-V1"));

TEST(ConfigSessionInitialize, ALegacyProtocolIdSelectsNothing)
{
    ConfigSessionFixture f;
    f.put_settings(setting("protocol_id", "1"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.selected_vehicle(), nullptr);
}
```

- `ValidSavedIdIsKept`: `setting("vehicle_id", "subaru-forester-v3")`.
- `RestoringTheSavedRowKeepsSavedTransports`: `setting("vehicle_id", "mitsubishi-colt-v2")`.
- `RowChangesTheSavedRowAndLogProtocolOnly`: `EXPECT_EQ(f.session.settings().selected_vehicle_id, "mitsubishi-colt-v2");`
- `UnmatchedProtocolNameChangesNothing`: `setting("vehicle_id", "mitsubishi-colt-v2")`.
- Add:

```cpp
TEST(ConfigSessionSave, ASelectionSurvivesARestart)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    ASSERT_THAT(f.session.select_row(2), IsOk());
    ASSERT_THAT(f.session.save(), IsOk());

    ASSERT_THAT(f.initialize(), IsOk());

    ASSERT_NE(f.session.selected_vehicle(), nullptr);
    EXPECT_EQ(f.session.selected_vehicle()->id, "subaru-forester-v3");
}
```

Run: `bazel test --config=release //src/backend/config:all`
Expected: FAIL to compile (`selected_vehicle_id`).

- [ ] **Step 3: Implement the id**

`app_config.h`: rename `selected_protocol_id` to `selected_vehicle_id`. `app_config.cpp`: the reader branch becomes `else if (name == "vehicle_id") { config.selected_vehicle_id = setting.child("value").attribute("data").value(); }` (there is no `protocol_id` branch: the old setting is ignored), and the writer line becomes `add_single("vehicle_id", config.selected_vehicle_id);` in the same position.

`config_session.cpp`: delete `parse_row` and `<charconv>`. In `initialize`, the validation becomes:

```cpp
    // An id this catalog does not know -- a retired vehicle, or none saved
    // yet -- selects nothing, and the startup gate asks for a vehicle.
    if (!catalog_.find_vehicle(settings.selected_vehicle_id).has_value())
    {
        settings.selected_vehicle_id.clear();
    }
```

`selected_row()`:

```cpp
Result<std::size_t> ConfigSession::selected_row() const
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    const std::optional<std::size_t> row = catalog_.find_vehicle(settings_.selected_vehicle_id);
    if (!row.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "no vehicle is selected");
    }
    return *row;
}
```

`select_row`: `settings_.selected_vehicle_id = std::string(vehicles[row].id);`. Update the header comments: the class comment names `AppConfig::selected_vehicle_id` as the only saved selection; `initialize` says an unknown saved id is cleared and selects nothing; `vehicles()` drops "a row's id is its position" in favor of `// Catalog order. Empty until initialized.`

Run: `bazel test --config=release //src/backend/config:all`
Expected: PASS.

- [ ] **Step 4: Readers of the old id**

- `vehicle_select.cpp`: before the version loop, `const fastecu::Result<std::size_t> saved = config.selected_row();`; the comparison becomes `if (saved.has_value() && id.at(i) == QString::number(*saved))`.
- `mainwindow.cpp`: `"Protocols ID: " + qs(...selected_protocol_id)` → `"Vehicle ID: " + qs(configSession->settings().selected_vehicle_id)`; both `"Selected protocol: " + qs(...selected_protocol_id)` → `"Selected vehicle: " + qs(configSession->settings().selected_vehicle_id)`.
- `mainwindow_test.cpp`: the suite's `fastecu.cfg` uses `<setting name="vehicle_id"><value data="..."/></setting>` with the id of `kWindowVehicles[0]` (copy it from the generated array) instead of `protocol_id`; the assertion `reread.settings().selected_protocol_id == "1"` becomes `reread.settings().selected_vehicle_id == std::string(kWindowVehicles[1].id)`; update any expectation of the old log texts (`git grep -n -E 'Protocols ID|Selected protocol:' -- src`).
- Any other test that relied on initialization selecting row 0 now sees no selection: give it an explicit `ASSERT_THAT(f.session.select_row(0), IsOk())` or a `vehicle_id` setting at setup — production guarantees a selection through the gate before any such consumer runs. `bazel test --config=release //...` lists them.

- [ ] **Step 5: Write the gate's failing test**

Create `apps/desktop/startup_vehicle_gate_test.cpp`:

```cpp
#include "apps/desktop/startup_vehicle_gate.h"

#include <cstdlib>
#include <optional>

#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"

namespace
{

using fastecu::config::testing::ConfigSessionFixture;
using fastecu::config::testing::setting;

TEST(StartupVehicleGate, AnExistingSelectionDoesNotAsk)
{
    ConfigSessionFixture f;
    f.put_settings(setting("vehicle_id", "subaru-forester-v3"));
    ASSERT_TRUE(f.initialize().has_value());
    int asked = 0;

    EXPECT_EQ(startup_vehicle_gate(f.session, [&asked] { ++asked; return std::optional<std::size_t>{}; }),
              std::nullopt);
    EXPECT_EQ(asked, 0);
}

TEST(StartupVehicleGate, AChosenVehicleIsSelectedSavedAndStartupContinues)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());

    EXPECT_EQ(startup_vehicle_gate(f.session, [] { return std::optional<std::size_t>{1}; }), std::nullopt);

    ASSERT_NE(f.session.selected_vehicle(), nullptr);
    EXPECT_EQ(f.session.selected_vehicle()->id, "mitsubishi-colt-v2");
    // Saved: the session an in-app restart builds asks nothing.
    ASSERT_TRUE(f.initialize().has_value());
    int asked = 0;
    EXPECT_EQ(startup_vehicle_gate(f.session, [&asked] { ++asked; return std::optional<std::size_t>{}; }),
              std::nullopt);
    EXPECT_EQ(asked, 0);
}

TEST(StartupVehicleGate, ACancelEndsStartupWithExitCodeZero)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());

    EXPECT_EQ(startup_vehicle_gate(f.session, [] { return std::optional<std::size_t>{}; }),
              std::optional<int>(EXIT_SUCCESS));
    EXPECT_EQ(f.session.selected_vehicle(), nullptr);
}

TEST(StartupVehicleGate, AnInvalidRowIsTreatedAsACancel)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());

    EXPECT_EQ(startup_vehicle_gate(f.session, [] { return std::optional<std::size_t>{99}; }),
              std::optional<int>(EXIT_SUCCESS));
}

} // namespace
```

Run: `python3 scripts/gazelle_check.py --fix && bazel test --config=release //apps/desktop:startup_vehicle_gate_test`
Expected: FAIL (header missing).

- [ ] **Step 6: Implement the gate**

Create `apps/desktop/startup_vehicle_gate.h`:

```cpp
#pragma once
#include <cstddef>
#include <functional>
#include <optional>

#include "src/backend/config/config_session.h"

// Asks the operator for a vehicle row; nullopt when they cancel.
using VehicleChooser = std::function<std::optional<std::size_t>()>;

// Runs after a successful composition and before MainWindow exists, which
// relies on a vehicle always being selected. With one selected it does
// nothing. Otherwise it asks `choose`: a chosen row is selected and saved,
// and startup continues (nullopt); a cancel returns the exit code main()
// ends with, before any window or ECU I/O exists. A failed save only means
// the next start asks again.
std::optional<int> startup_vehicle_gate(fastecu::config::ConfigSession& session, const VehicleChooser& choose);
```

Create `apps/desktop/startup_vehicle_gate.cpp`:

```cpp
#include "apps/desktop/startup_vehicle_gate.h"

#include <cstdlib>
#include <tuple>

std::optional<int> startup_vehicle_gate(fastecu::config::ConfigSession& session, const VehicleChooser& choose)
{
    if (session.selected_vehicle() != nullptr)
    {
        return std::nullopt;
    }
    const std::optional<std::size_t> row = choose();
    if (!row.has_value() || !session.select_row(*row).has_value())
    {
        return EXIT_SUCCESS;
    }
    std::ignore = session.save();
    return std::nullopt;
}
```

Run `python3 scripts/gazelle_check.py --fix`; make sure the generated library is `visibility = ["//visibility:private"]` like `startup`, and add `qt = False` to `startup_vehicle_gate_test` (it links no Qt).

Run: `bazel test --config=release //apps/desktop:startup_vehicle_gate_test`
Expected: PASS.

- [ ] **Step 7: Wire it into `main.cpp`**

Add `#include "apps/desktop/startup_vehicle_gate.h"` and `#include "src/ui/desktop/widgets/vehicle_select.h"`. After `present_startup_warnings(composition.startup_warnings());` and before `MainWindow w(...)`:

```cpp
        // MainWindow requires a selected vehicle; a first start, or a saved
        // vehicle this build no longer has, asks for one here.
        fastecu::config::ConfigSession& config = composition.services().config;
        if (const std::optional<int> exit_code = startup_vehicle_gate(config,
                                                                      [&config]
                                                                      {
                                                                          VehicleSelect chooser(config);
                                                                          chooser.exec();
                                                                          return chooser.chosen_row();
                                                                      });
            exit_code.has_value())
        {
            return_code = *exit_code;
            break;
        }
```

Add `":startup_vehicle_gate"` to the `fastecu` binary's deps if Gazelle did not.

- [ ] **Step 8: An upgrade from a protocol_id-only settings file**

Append to `desktop_composition_test.cpp`:

```cpp
TEST_F(DesktopCompositionTest, aPreviousVersionsSavedRowSelectsNoVehicle)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString previous_dir = root.path() + "/0.1.0-beta.4/config/";
    ASSERT_TRUE(QDir().mkpath(previous_dir));
    ASSERT_TRUE(writeFile(previous_dir + "fastecu.cfg", R"(<config name="FastECU"><software_settings>
<setting name="protocol_id"><value data="35"/></setting>
</software_settings></config>)"));

    DesktopComposition composition{{}, {}, root.path()};

    ASSERT_TRUE(composition.started());
    ASSERT_TRUE(config_of(composition).selected_vehicle() == nullptr);
}
```

- [ ] **Step 9: Build and test everything**

Run: `python3 scripts/gazelle_check.py --fix && bazel build --config=release //... && bazel test --config=release //...`
Expected: PASS. Then the portable-core check.

- [ ] **Step 10: Commit**

```bash
prek run --all-files
git add -A src apps resources/shared/config/fastecu.cfg
git commit -m "feat(config): save the selected vehicle by stable id and ask for one at startup"
```

---

### Task 11: Delete protocols.cfg and record the decision

**Files:**
- Delete: `resources/shared/config/protocols.cfg`, `resources/shared/protocols_cfg_eeprom_capabilities_test.cpp`, `resources/shared/protocols_cfg_subaru_mitsu_m32r_kline_test.cpp`, `resources/shared/protocols_cfg_subaru_hitachi_m32r_kline_test.cpp`, `src/backend/config/builtin_catalog_parity_test.cpp`
- Modify: `resources/shared/config.qrc`, `resources/shared/BUILD.bazel`, `src/backend/config/BUILD.bazel`
- Modify: `src/backend/config/config_paths.h`, `config_paths.cpp`, `config_paths_test.cpp` and every remaining `protocols_file` user
- Modify: `src/platform/desktop/common/ports/qt_resource_bundle_test.cpp`
- Modify (comments): `src/backend/flash/flash_device_lookup_test.cpp`, `src/ui/desktop/calibration/hex_parse_qt_compat_test.cpp`, `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor_test.cpp`
- Create: `docs/adr/0019-compile-the-protocol-catalog-into-the-backend.md`
- Modify: `docs/adr/README.md`, `docs/flash-qualification-matrix.md`, `docs/design-notes.md`, `docs/connection-bench-checklist.md`, `docs/subaru-tcu-hitachi-m32r-can-bench-checklist.md`

**Interfaces:**
- Consumes: everything above.
- Produces: no `protocols.cfg` anywhere; `ConfigPaths` without `protocols_file`.

- [ ] **Step 1: The bundle no longer lists protocols.cfg**

In `qt_resource_bundle_test.cpp`, `ListsRealShippedConfigFiles` becomes:

```cpp
TEST(QtResourceBundleTest, ListsRealShippedConfigFiles)
{
    QtResourceBundle bundle;
    auto names = bundle.list("config");

    ASSERT_THAT(names, fastecu::testing::IsOk());
    EXPECT_NE(std::find(names->begin(), names->end(), "fastecu.cfg"), names->end());
    EXPECT_NE(std::find(names->begin(), names->end(), "logger.cfg"), names->end());
    // Protocols and vehicles are compiled in; nothing provisions a copy.
    EXPECT_EQ(std::find(names->begin(), names->end(), "protocols.cfg"), names->end());
}
```

Run: `bazel test --config=release //src/platform/desktop/common/ports:all`
Expected: FAIL (the bundle still lists `protocols.cfg`).

- [ ] **Step 2: Delete the file and everything that read it**

```bash
git rm resources/shared/config/protocols.cfg \
       resources/shared/protocols_cfg_eeprom_capabilities_test.cpp \
       resources/shared/protocols_cfg_subaru_mitsu_m32r_kline_test.cpp \
       resources/shared/protocols_cfg_subaru_hitachi_m32r_kline_test.cpp \
       src/backend/config/builtin_catalog_parity_test.cpp
```

- `config.qrc`: delete the `config/protocols.cfg` line.
- `resources/shared/BUILD.bazel`: delete the comment block about the pinning tests, the `exports_files` block and the three `test_protocols_cfg_*` targets.
- `src/backend/config/BUILD.bazel`: delete `builtin_catalog_parity_test`.
- `ConfigPaths`: delete `protocols_file` from `config_paths.h` and its assignment in `config_paths.cpp`; delete the two `protocols_file` expectations in `config_paths_test.cpp`; then `git grep -n protocols_file -- src apps tests` and remove every remaining use (the session test's stale-file case already builds its path from `config_files_directory`).

Run: `python3 scripts/gazelle_check.py --fix && bazel test --config=release //...`
Expected: PASS.

- [ ] **Step 3: Comments that named the file**

- `flash_device_lookup_test.cpp`: the comment on `kKnown` becomes `// Every MCU a flash-capable built-in protocol names (the catalog consistency test checks the catalog side), except M32170: MUT/DMA logging's MCU, which offers no flash operation and has no kFlashDevices[] entry -- exercised below, because checksum correction's "Unknown MCU type" path fires for it.`; the comment in `ReturnsNullForUnknownMcuType` becomes `// "M32170" is the MUT/DMA logging protocol's MCU; it is not registered in kFlashDevices[].`
- `hex_parse_qt_compat_test.cpp`: `// Every kernel_addr value the retired protocols.cfg shipped. ...` (keep the rest and the values).
- `denso_sh705x_eeprom_kline_executor_test.cpp:43`: `resources/shared/config/protocols.cfg's` → `the built-in catalog's`.

`git grep -n 'protocols.cfg' -- src apps tests resources` must now print only these historical comments and the built-in catalog's "retired protocols.cfg" comment.

- [ ] **Step 4: ADR 0019**

Create `docs/adr/0019-compile-the-protocol-catalog-into-the-backend.md`:

```markdown
# ADR 0019: Protocol and Vehicle Data Are Compiled into the Backend

## Status

Accepted.

## Context

`protocols.cfg` held 63 flash and logging protocols and 65 vehicles. It was
bundled as a Qt resource, copied into each version's config directory when
absent, and parsed at startup and again on every flash and EEPROM read. It
looked like user configuration, but:

- Every flash family already declared each protocol's MCU, kernel load
  address and supported operations in its own plan, and rejected a file value
  that disagreed. Flash and checksum routing select behavior by protocol
  name, so a protocol added to the file reached no code.
- The checksum flag was the one flash-relevant field the code trusted:
  changing `yes` to `no` wrote an uncorrected image without a warning.
- Provisioning is per version and migrates only `fastecu.cfg`, so an edited
  file was replaced at every upgrade, and the saved selection -- a row number
  into the file -- could name a different vehicle after one.
- Nothing checked the data: a kernel name that matched its file only on
  case-insensitive filesystems, two vehicles pointing at renamed protocols,
  14 protocols no vehicle reached, and one EEPROM entry with the SH7055 kernel
  address on an SH7058.

## Decision

- Protocols and vehicles are `constexpr` data in `src/backend/config`
  (`builtin_catalog`), with no runtime override. Changes go through pull
  requests.
- Consumers receive a `Catalog` value. Only the desktop composition root names
  the built-in one; tests pass synthetic catalogs.
- Each hardware fact (MCU, kernel, load address, supported operations) stays
  declared both in the catalog and in its flash family. Tests prove the two
  agree, and the checksum flag agrees with checksum routing; plan validation
  keeps checking an independent declaration.
- The saved selection is a stable vehicle id. A saved id this build does not
  know selects nothing, and startup asks for a vehicle before the main window
  exists.

Rejected: making each flash family the single owner of its hardware facts (a
large hardware-facing refactor with no safety gain once the data is compiled
in), and making the catalog the single owner with plans validating against it
(the plans would compare a value with itself).

## Consequences

- An upstream `protocols.cfg` change is ported by hand into the catalog; it
  needed route and plan code to have any effect anyway.
- Upgrading resets the saved selection once.
- Kernels are unaffected: still bundled, provisioned to disk and read by name.
- `fastecu.cfg` and `logger.cfg` remain user files.
```

In `docs/adr/README.md`, add the index row `| [0019](0019-compile-the-protocol-catalog-into-the-backend.md) | Protocol and vehicle data are compiled into the backend |` and change `The next new ADR is 0018.` to `The next new ADR is 0020.`

- [ ] **Step 5: The other documents**

- `docs/flash-qualification-matrix.md`: rule line 22 → `- A family found unmigrated in the built-in catalog (\`src/backend/config/builtin_catalog.cpp\`) is added with \`portable=no\` and \`hardware_status=unqualified\` unless concrete historical hardware evidence is entered with a reference. Use the exact catalog protocol name or operation class name; do not invent family IDs.`; in the notes at lines 72 and 76 replace `protocols.cfg` `<protocol>` entries with "built-in catalog protocols" and drop the sentence about the file's XML sections. Add after the rules: `- Protocols the built-in catalog newly offered (MUT/DMA logging, Unisia Jecs M3779x/M3775x reads, the five Denso SH705x EEPROM reads, the three Cobb variants and \`sub_ecu_denso_sh7055_02_ecutek\`) carry their family's status; none has hardware evidence.`
- `docs/connection-bench-checklist.md:26` and `docs/subaru-tcu-hitachi-m32r-can-bench-checklist.md:117`: name the built-in catalog instead of `protocols.cfg`.
- `docs/design-notes.md`, section "Configuration session": the "One saved selection" bullet names `AppConfig::selected_vehicle_id` and `VehicleSpec`; the startup-rejection bullet drops "an unreadable `protocols.cfg`, or one with no car models" and notes the pinning test now uses a malformed `fastecu.cfg`; the "Kept quirks" bullet drops the invalid-row and unresolved-reference quirks (keep last-match selection and the datalog round-trip quirk). Add a section after it:

```markdown
## Built-in protocol and vehicle catalog

Protocols and vehicles are compile-time data in `src/backend/config/builtin_catalog.cpp`; see [ADR 0019](adr/0019-compile-the-protocol-catalog-into-the-backend.md).

- **Model.** `ProtocolSpec` holds a protocol's display text, capabilities, checksum support (`Corrected`, `Missing`, `None` -- the old `yes`, `n/a`, `no`) and kernel. `VehicleSpec` holds a vehicle's text, a permanent id and a pointer to its protocol, resolved at compile time.
- **Consistency.** A `static_assert` requires every vehicle to have a protocol and every protocol a vehicle. `catalog_problems()` adds duplicate and spelling checks in tests. `tests/catalog_consistency_test.cpp` ties checksum flags to checksum routes, kernel names to the bundle and MCUs to flash models, and `builtin_catalog_capability_test` ties capabilities to the workflows.
- **Order.** Vehicles keep the order the chooser lists them in; alias resolution takes the first match and ROM-open selection the last, so new vehicles are appended.
- **Selection.** `fastecu.cfg` saves `vehicle_id`. An unknown or missing id selects nothing; `startup_vehicle_gate` asks before `MainWindow` exists, and a cancel exits with code 0.
- **Data fixes.** Generating the catalog fixed the SH7055 TCU kernel name's case, two vehicles pointing at renamed Unisia Jecs protocols, typos, the kernel fields of three kernel-free protocols, and the SH7058 DensoCAN EEPROM kernel address (VERIFY on bench); MC68HC16Y5 revision 04 was deleted.
```

- [ ] **Step 6: Final verification**

Run:

```bash
python3 scripts/gazelle_check.py
bazel build --config=release //...
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Expected: all pass. Then the portable-core check from Global Constraints. `git grep -n -i 'protocols\.cfg' -- docs` shows only historical mentions (ADR 0019, design notes, the qualification matrix's VERIFY note, the spec and this plan).

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "chore(config): delete protocols.cfg and record ADR 0019"
```

After the pull request merges, condense the spec and this plan into the design notes and delete them, as the repository did for earlier waves.
