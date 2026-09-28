# Step 6k: Portable Configuration Session Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `ConfigValuesStructure`'s settings, paths, and thirty parallel protocol lists with one portable `fastecu::config::ConfigSession` owned by `DesktopComposition`, and delete `LegacyConfigAdapter` and `legacy_config_paths` with no synchronized copy left behind.

**Architecture:** `ConfigSession` (in `//src/backend/config`) holds provisioned `ConfigPaths`, the `AppConfig` settings, and the resolved vehicle rows. `AppConfig.selected_protocol_id` is the only saved selection; everything else about the selected vehicle is derived from its `ResolvedCarModel`. PR 6k-1 adds the session and its tests with nothing consuming it. PR 6k-2 first moves the eight definition-index lists into a `FileActions`-owned struct (independent and green), then switches every consumer in one commit, including composition-level startup rejection. After that it deletes the dead legacy package, then updates the docs.

**Tech Stack:** C++23 (`std::expected`, `std::from_chars`, `std::format`), Bazel (version in `.bazelversion`), pugixml, GoogleTest/GoogleMock (`fastecu_portable_gtest`), Qt 6 + QtTest (`fastecu_qttest`), prek, `gh stack`.

**Spec:** [Step 6k — Portable configuration session](../specs/2026-09-27-configuration-models-design.md)

## Global Constraints

- Application metadata supplied by composition: name `FastECU`, title `FastECU`, version `0.1.0-beta.5`.
- Default config roots: Unix/macOS `QDir::homePath() + "/.config/FastECU/"`, Windows `QDir::homePath() + "/AppData/Local/FastECU/"`. Keep the trailing slash exactly as the legacy struct had it. An explicit `config_root` (tests) overrides it.
- Session defaults: `window_width`/`window_height` `default`, `toolbar_iconsize` `32`, `serial_port` `ttyUSB0`, `primary_definition_base` `ecuflash`, `use_romraider_definitions` and `use_ecuflash_definitions` `disabled`, `calibration_files_directory` and `datalog_files_directory` = provisioned directories. All other settings default to `""` / empty list. `AppConfig` and `parse_app_config` keep `""` as "absent".
- Trailing-slash normalization (a trailing `/` or `\` counts) applies to `calibration_files_directory`, `ecuflash_definition_files_directory`, `datalog_files_directory` on every successful save.
- Preserve the XML mismatch: writer `logfiles_directory`, reader `datalog_files_directory`. Tests pin it; never fix it.
- Unresolved protocol reference: `ResolvedCarModel::protocol == std::nullopt`; display/legacy boundary uses the single-space placeholder `" "`; read/test-write/write capabilities are unavailable. Never synthesize a `ProtocolEntry`.
- Protocol-name selection picks the **last** matching row (`find_car_model_by_protocol_name`). No match changes nothing.
- An invalid saved `protocol_id` (empty, negative, malformed, overflowing, out of range) becomes `"0"` in memory at initialization. Saved flash transport, log transport, and log protocol are never replaced at startup.
- `//src/backend/config:config_session` is portable: no Qt, threads, direct filesystem access, or platform detection. It is registered in `bazel/portable_targets.bzl`.
- Backend results are checked with `.has_value()`, never `operator bool`. `ErrorKind` is closed; use existing values only.
- The `qt_layer` ratchet only shrinks: Task 6 removes `//src/backend/config/legacy`. No entry is ever added.
- Platform differences go in separate sources selected by `BUILD.bazel`, not `#ifdef`.
- No wire behavior, hardware support, XML schema, software version, or packaging layout changes. No bench checklist changes; hardware qualification stays a separate gate.
- Markdown cross-references are links with readable text, not backticked paths.
- Work lands as a `gh stack`: `feat/6k-config-session` (6k-0: spec and this plan) → `refactor/step6k-1-config-session` (Tasks 1–3) → `refactor/step6k-2-config-consumers` (Tasks 4–7).
- Every PR passes `bazel build -k --config=release //...`, `bazel test -k --config=release //...`, `prek run --all-files`, and `bazel run --config=release //:clang_tidy_report_changed`. Windows/macOS/Linux CI is a required gate. Report any CI you cannot run locally as pending, not passing.
- Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

## Review Focus

- **Settings saved to an unwritable config file** (read-only file, a directory at that path, full disk). Expected: the user's edits stay in memory, and a warning names the file and the reason. Pinned by `ConfigSessionSave.FailureKeepsEditsAndNamesTheFile` (Task 2) and `SettingsTest::failedSaveKeepsEditsAndWarnsTheOperator` (Task 5).
- **A `protocols.cfg` update shrinks the vehicle list below the saved `protocol_id`.** Expected: row 0 is selected, and the saved flash transport, log transport, and log protocol are kept. Pinned by `ConfigSessionInitialize.InvalidSavedIdSelectsRowZero` (parameterized) and `RestoringTheSavedRowKeepsSavedTransports` (Task 2).
- **Opening a ROM whose flash method matches no vehicle, or matches a vehicle whose protocol reference is unresolved.** Expected: no match changes nothing, including the log protocol. A match on an unresolved row selects it, shows `" "` placeholders, and leaves read/write unavailable. Pinned by `ConfigSessionSelect.*` (Task 3) and `MainWindowTest::unmatchedRomFlashMethodChangesNothing` / `unresolvedProtocolRowLeavesReadAndWriteUnavailable` (Task 5).
- **Editing the calibration or datalog directory in Settings.** Expected: config, kernel, definition, protocols, menu, logger, and syslog paths do not move. The datalog edit is lost on restart because of the pinned XML mismatch. Pinned by `ConfigSessionPaths.EditingConfigurableDirectoriesMovesOnlyThose` (Task 2) and `DesktopCompositionTest::restartSeesSavedSettingsButNotTheDatalogDirectory` (Task 5).
- **Startup with a corrupt `fastecu.cfg`, or a `protocols.cfg` with zero car models.** Expected: a message naming the file and the reason, a clean exit before `MainWindow`, no syslog thread, no serial facade, and no ECU I/O. Pinned by `DesktopCompositionTest::failedStartupBuildsNoServicesAndPerformsNoEcuIo` / `malformedSettingsRejectStartup` (Task 5), with a mutation check.

---

## File Structure

| File | Change | Task |
|---|---|---|
| `src/backend/config/app_config.{h,cpp}`, `app_config_test.cpp` | Add `parse_app_config`; `operator==` on `AppConfig` | 1 |
| `src/backend/config/config_paths.h` | `operator==` on `ConfigPaths` | 1 |
| `src/backend/config/provisioning.cpp`, `provisioning_test.cpp` | Returned errors name the failing path | 1 |
| `src/backend/ports/testing/in_memory_file_repository.h`, `..._test.cpp` | `write_errors` | 1 |
| `src/backend/config/config_session.{h,cpp}`, `config_session_test.cpp` | Create | 2, 3 |
| `src/backend/config/testing/{BUILD.bazel,config_session_fixture.h}` | Create (testonly fixture) | 2 |
| `src/backend/config/BUILD.bazel`, `bazel/portable_targets.bzl` | `config_session` target and registration | 2 |
| `src/backend/definitions/definition_indexes.h`, `BUILD.bazel` | Create `DefinitionIndexes`; FileActions owns it | 4 |
| `src/backend/definition/legacy/legacy_definition_adapter.{h,cpp}` + test | Take `DefinitionIndexes&` | 4 |
| `src/ui/desktop/definition/definition_authoring_dialog.{h,cpp}` + test | `record_definition` on `DefinitionIndexes`; read session settings | 4, 5 |
| `apps/desktop/{desktop_composition.{h,cpp},main.cpp,BUILD.bazel}` + test | Own/initialize the session; reject startup | 5 |
| `apps/desktop/startup_diagnostics.{h,cpp}`, `startup_event_sink.h`, `default_config_root{.h,_unix.cpp,_windows.cpp}` + tests | Create | 5 |
| `src/ui/desktop/main_window_services.h` | Add `application`, `config` | 5 |
| `src/ui/desktop/config_fields.{h,cpp}` + test | Create (QString display helpers) | 5 |
| `src/ui/desktop/{mainwindow,menu_actions,log_operations_ssm,settings,vehicle_select,protocol_select}.*` + tests | Switch to the session | 5 |
| `src/backend/definitions/file_actions{.h,.cpp,_ecuflash.cpp,_romraider.cpp}` + tests | Switch to the session | 5 |
| `src/backend/calibration/legacy/legacy_calibration_adapter.{h,cpp}` + test, BUILD | Drop catalog cache and `bind_protocol` | 5 |
| `src/backend/config/legacy/**`, `src/backend/definitions/config_values.h`, `bazel/qt/BUILD.bazel` | Delete; drop ratchet entry | 6 |
| `docs/design-notes.md`, `docs/modularization-plan.md`, `docs/tech-debt.md` | Close-out | 7 |
| This plan and the spec | Delete | 7 |

---

## PR 6k-1 — Portable session

### Task 1: Observable load path, path-bearing errors, and a failing-write fake

**Files:**
- Modify: `src/backend/config/app_config.h`, `src/backend/config/app_config.cpp`, `src/backend/config/app_config_test.cpp`
- Modify: `src/backend/config/config_paths.h`
- Modify: `src/backend/config/provisioning.cpp`, `src/backend/config/provisioning_test.cpp`
- Modify: `src/backend/ports/testing/in_memory_file_repository.h`, `src/backend/ports/testing/in_memory_file_repository_test.cpp`

**Interfaces:**
- Produces: `Result<AppConfig> fastecu::config::parse_app_config(const ConfigPaths&, IFileRepository&)`. Reads and parses only; never writes. Same empty-value and unnormalized contract as today's `load_app_config`.
- Produces: `load_app_config` unchanged in behavior (`parse_app_config` + fire-and-forget `save_app_config`).
- Produces: `bool AppConfig::operator==(const AppConfig&) const = default;` and `bool ConfigPaths::operator==(const ConfigPaths&) const = default;`.
- Produces: `InMemoryFileRepository::write_errors` (`std::map<std::string, Error>`). A write to a listed handle returns that error, stores nothing, and records no `write_calls` entry.
- Produces: every error `provision_config_directories` returns has detail `"<path>: <original detail>"`, with the original `ErrorKind`.

- [ ] **Step 1: Create the 6k-1 branch**

```bash
git switch feat/6k-config-session
git switch -c refactor/step6k-1-config-session
```

- [ ] **Step 2: Write the failing tests**

Append to `src/backend/config/app_config_test.cpp`, reusing the file's existing `using` declarations. Add any that are missing: `fastecu::config::parse_app_config`, `fastecu::config::load_app_config`, `fastecu::config::ConfigPaths`, `fastecu::InMemoryFileRepository`.

```cpp
namespace
{
void put_text(InMemoryFileRepository& repo, const std::string& handle, std::string_view text)
{
    repo.files[handle] = std::vector<std::uint8_t>(text.begin(), text.end());
}

constexpr std::string_view kUnnormalizedCalibrationDir =
    R"(<config name="FastECU" version="x"><software_settings>)"
    R"(<setting name="calibration_files_directory"><value data="/cal"/></setting>)"
    R"(</software_settings></config>)";
} // namespace

TEST(ParseAppConfig, ReadsWithoutWritingAndKeepsTheParserContract)
{
    InMemoryFileRepository repo;
    ConfigPaths paths;
    paths.config_file = "fastecu.cfg";
    put_text(repo, paths.config_file, kUnnormalizedCalibrationDir);

    const auto parsed = parse_app_config(paths, repo);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->calibration_files_directory, "/cal"); // unnormalized
    EXPECT_EQ(parsed->serial_port, "");                     // absent stays empty
    EXPECT_TRUE(repo.write_calls.empty());
}

TEST(LoadAppConfig, StillRewritesTheFileOnLoad)
{
    InMemoryFileRepository repo;
    ConfigPaths paths;
    paths.config_file = "fastecu.cfg";
    put_text(repo, paths.config_file, kUnnormalizedCalibrationDir);

    ASSERT_TRUE(load_app_config(paths, repo).has_value());

    ASSERT_EQ(repo.write_calls.size(), 1u);
    EXPECT_EQ(repo.write_calls.front().first, "fastecu.cfg");
}

TEST(ParseAppConfig, MissingFileIsAnError)
{
    InMemoryFileRepository repo;
    ConfigPaths paths;
    paths.config_file = "absent.cfg";

    EXPECT_FALSE(parse_app_config(paths, repo).has_value());
}
```

Append to `src/backend/ports/testing/in_memory_file_repository_test.cpp`, matching that file's includes and `using` style:

```cpp
TEST(InMemoryFileRepository, WriteErrorIsReturnedAndNothingIsStored)
{
    fastecu::InMemoryFileRepository repo;
    repo.write_errors["a.cfg"] = fastecu::Error{fastecu::ErrorKind::Internal, "read-only"};
    const std::vector<std::uint8_t> bytes{1, 2, 3};

    const fastecu::Status written = repo.write("a.cfg", bytes);

    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().detail, "read-only");
    EXPECT_FALSE(repo.files.contains("a.cfg"));
    EXPECT_TRUE(repo.write_calls.empty());
}
```

Append to `src/backend/config/provisioning_test.cpp`:

```cpp
TEST(ProvisionConfigDirectories, CreateDirectoryFailureNamesThePath)
{
    InMemoryFileSystem fs;
    fs.create_directory_error = fastecu::Error{ErrorKind::Internal, "permission denied"};
    InMemoryResourceBundle bundle;
    RecordingEventSink events;
    ConfigPaths paths = test_paths();

    EXPECT_THAT(provision_config_directories(paths, fs, bundle, events),
                fastecu::testing::IsErrWith(ErrorKind::Internal,
                                            ::testing::AllOf(::testing::HasSubstr(paths.base_config_directory),
                                                             ::testing::HasSubstr("permission denied"))));
}

TEST(ProvisionConfigDirectories, BundleCopyFailureNamesTheTarget)
{
    InMemoryFileSystem fs;
    InMemoryResourceBundle bundle;
    // Listed in the bundle but absent as a copy source, so copy_file fails.
    bundle.bundles["config"]["menu.cfg"] = {1};
    RecordingEventSink events;
    ConfigPaths paths = test_paths();

    EXPECT_THAT(provision_config_directories(paths, fs, bundle, events),
                fastecu::testing::IsErrWith(ErrorKind::Internal,
                                            ::testing::HasSubstr(paths.config_files_directory + "menu.cfg")));
}
```

- [ ] **Step 3: Run the tests and confirm they fail**

Run: `bazel test --config=release //src/backend/config:app_config_test //src/backend/config:provisioning_test //src/backend/ports/testing:in_memory_file_repository_test`
Expected: build FAIL (`parse_app_config` undeclared, `write_errors` has no member). After those two compile, the provisioning cases FAIL because the detail lacks the path.

- [ ] **Step 4: Implement**

`src/backend/config/config_paths.h`: add as the struct's first member line

```cpp
    bool operator==(const ConfigPaths&) const = default;
```

`src/backend/config/app_config.h`: add `bool operator==(const AppConfig&) const = default;` as the first line of `AppConfig`, and declare the following before `load_app_config`:

```cpp
// Reads and parses fastecu.cfg without writing anything back. Absent
// settings stay "" and directory values are returned as written in the
// file (unnormalized) -- the contract load_app_config has always had.
Result<AppConfig> parse_app_config(const ConfigPaths& paths, IFileRepository& file_repository);
```

`src/backend/config/app_config.cpp`: rename the existing body of `load_app_config`, up to but not including the `(void)save_app_config(...)` statement, to `parse_app_config`, ending with `return config;`. Then define:

```cpp
Result<AppConfig> load_app_config(const ConfigPaths& paths, IFileRepository& file_repository)
{
    Result<AppConfig> config = parse_app_config(paths, file_repository);
    if (!config.has_value())
    {
        return config;
    }
    // Matches legacy FileActions::read_config_file (file_actions.cpp:910),
    // which rewrites the config file on every load. The save's result is
    // fire-and-forget; callers needing to observe it use parse_app_config
    // and save_app_config themselves (ConfigSession does).
    (void)save_app_config(*config, paths, file_repository);
    return config;
}
```

`src/backend/ports/testing/in_memory_file_repository.h`: at the top of `write`, add

```cpp
        if (auto error = write_errors.find(std::string(h)); error != write_errors.end())
        {
            return std::unexpected(error->second);
        }
```

and add the member `std::map<std::string, Error> write_errors;` next to `read_errors`.

`src/backend/config/provisioning.cpp`: in the anonymous namespace add

```cpp
// Provisioning errors reach the operator at startup, so each names the path
// it failed on; the port's own detail is only the reason.
std::unexpected<Error> at_path(const Error& error, std::string_view path)
{
    return std::unexpected(Error{error.kind, std::format("{}: {}", path, error.detail)});
}
```

Then replace `return result;` in `ensure_directory` with `return at_path(result.error(), path);`, and replace `return result;` in `copy_bundle_if_absent` with `return at_path(result.error(), target);`. In the syslog pruning loop, replace `return r;` with `return at_path(r.error(), paths.syslog_files_directory + files[i].name);`. The `return r;` statements that forward from `ensure_directory`/`copy_bundle_if_absent` stay as they are.

- [ ] **Step 5: Run the tests and confirm they pass**

Run: `bazel test --config=release //src/backend/config:all //src/backend/ports/testing:all`
Expected: PASS, including the existing `LoadAppConfig.*`, `SaveAppConfigThenLoadAppConfig.DatalogDirectoryDoesNotRoundTrip`, and `ProvisionConfigDirectories.*`.

- [ ] **Step 6: Commit**

```bash
git add src/backend/config src/backend/ports/testing
git commit -m "refactor(config): observable settings parse and path-bearing provisioning errors (step 6k-1)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `ConfigSession` initialization, defaults, paths, and saving

**Files:**
- Create: `src/backend/config/config_session.h`, `src/backend/config/config_session.cpp`, `src/backend/config/config_session_test.cpp`
- Create: `src/backend/config/testing/BUILD.bazel`, `src/backend/config/testing/config_session_fixture.h`
- Modify: `src/backend/config/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `parse_app_config`, `save_app_config`, `resolve_config_paths`, `provision_config_directories`, `load_protocol_catalog`, `load_car_model_catalog`, `resolve_car_models`, `InMemoryFileRepository::write_errors` (Task 1).
- Produces (`src/backend/config/config_session.h`, label `//src/backend/config:config_session`):
  ```cpp
  class ConfigSession {
    public:
      ConfigSession(IFileSystem&, IResourceBundle&, IFileRepository&, IEventSink&);
      Status initialize(std::string_view app_root, std::string_view version);
      bool initialized() const;
      Status save();
      AppConfig& settings();
      const AppConfig& settings() const;
      ConfigPaths provisioned_paths() const;
      ConfigPaths effective_paths() const;
      std::span<const ResolvedCarModel> vehicles() const;
      Result<std::size_t> selected_row() const;
      const ResolvedCarModel *selected_vehicle() const; // nullptr unless selected_row() has a value
  };
  ```
- Produces (label `//src/backend/config/testing:config_session_fixture`, testonly): `fastecu::config::testing::ConfigSessionFixture`, `kRoot` (`"/root"`), `kVersion` (`"0.1.0-beta.5"`), `kStandardProtocols`, `setting(name, data)`.

- [ ] **Step 1: Create the fixture package**

`src/backend/config/testing/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")

# Config-package-owned test fixture: a ConfigSession over in-memory ports
# with a small, fully known protocols.cfg.
package(default_visibility = [
    "//apps/desktop:__pkg__",
    "//src/backend:__subpackages__",
    "//src/ui:__subpackages__",
])

cc_library(
    name = "config_session_fixture",
    testonly = True,
    hdrs = ["config_session_fixture.h"],
    deps = [
        "//src/backend/config:config_paths",
        "//src/backend/config:config_session",
        "//src/backend/ports/testing:in_memory_file_repository",
        "//src/backend/ports/testing:in_memory_file_system",
        "//src/backend/ports/testing:in_memory_resource_bundle",
        "//src/backend/ports/testing:recording_event_sink",
    ],
)
```

If `//src/backend/ports/testing`'s `default_visibility` does not already cover `//src/backend/config/testing`, add that package to it. Check the list at the top of that BUILD file.

`src/backend/config/testing/config_session_fixture.h`:

```cpp
#pragma once
#include <format>
#include <string>
#include <string_view>
#include <vector>

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

// Vehicle rows, in file order (row id == position):
//   0 Subaru Impreza    -> proto_a  (shared with row 2)
//   1 Mitsubishi Colt   -> proto_b
//   2 Subaru Forester   -> proto_a
//   3 Nissan Skyline    -> missing_proto (no <protocol> of that name)
inline constexpr std::string_view kStandardProtocols = R"(<?xml version="1.0"?>
<config name="FastECU" version="test">
  <protocols>
    <protocol name="proto_a" alias="alias_a">
      <ecu>ECU A</ecu><mcu>SH7058</mcu><mode>OBD2</mode><checksum>yes</checksum>
      <read>yes</read><test_write>no</test_write><write>yes</write>
      <flash_transport>iso15765,CAN</flash_transport><log_transport>K-Line</log_transport>
      <log_protocol>SSM</log_protocol><kernel>a.bin</kernel><kernel_addr>0xFFFF3000</kernel_addr>
      <description>Protocol A</description>
    </protocol>
    <protocol name="proto_b" alias="alias_b">
      <ecu>ECU B</ecu><mcu>M32R</mcu><mode>OBD2</mode><checksum>n/a</checksum>
      <read>yes</read><test_write>yes</test_write><write>yes</write>
      <flash_transport>K-Line</flash_transport><log_transport>K-Line</log_transport>
      <log_protocol>MUT_DMA</log_protocol><kernel>b.bin</kernel><kernel_addr>0x0</kernel_addr>
      <description>Protocol B</description>
    </protocol>
  </protocols>
  <car_models>
    <car_model><make>Subaru</make><model>Impreza</model><version>v1</version><protocol>proto_a</protocol></car_model>
    <car_model><make>Mitsubishi</make><model>Colt</model><version>v2</version><protocol>proto_b</protocol></car_model>
    <car_model><make>Subaru</make><model>Forester</model><version>v3</version><protocol>proto_a</protocol></car_model>
    <car_model><make>Nissan</make><model>Skyline</model><version>v4</version><protocol>missing_proto</protocol></car_model>
  </car_models>
</config>
)";

inline std::string setting(std::string_view name, std::string_view data)
{
    return std::format(R"(<setting name="{}"><value data="{}"/></setting>)", name, data);
}

struct ConfigSessionFixture
{
    explicit ConfigSessionFixture(std::string_view root_path = kRoot)
        : root(root_path), paths(resolve_config_paths(root, kVersion))
    {
        put_protocols(kStandardProtocols);
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
    void put_protocols(std::string_view document)
    {
        put(paths.protocols_file, document);
    }
    Status initialize()
    {
        return session.initialize(root, kVersion);
    }

    std::string root;
    ConfigPaths paths;
    InMemoryFileSystem file_system;
    InMemoryResourceBundle resource_bundle;
    InMemoryFileRepository file_repository;
    RecordingEventSink events;
    ConfigSession session{file_system, resource_bundle, file_repository, events};
};

} // namespace fastecu::config::testing
```

- [ ] **Step 2: Write the failing tests**

`src/backend/config/config_session_test.cpp`:

```cpp
#include "src/backend/config/config_session.h"

#include <algorithm>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace
{

using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::config::AppConfig;
using fastecu::config::ConfigPaths;
using fastecu::config::resolve_config_paths;
using fastecu::config::testing::ConfigSessionFixture;
using fastecu::config::testing::kVersion;
using fastecu::config::testing::setting;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::AllOf;
using ::testing::HasSubstr;

bool has_log(const RecordingEventSink& events, LogLevel level, std::string_view text)
{
    return std::ranges::any_of(events.logs, [&](const auto& entry)
                               { return entry.first == level && entry.second.find(text) != std::string::npos; });
}

// --- initialization -------------------------------------------------------

TEST(ConfigSessionInitialize, ProvisionsEveryDirectoryUnderTheRoot)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_TRUE(f.session.initialized());
    EXPECT_TRUE(f.file_system.exists(f.paths.config_files_directory));
    EXPECT_TRUE(f.file_system.exists(f.paths.kernel_files_directory));
    EXPECT_TRUE(f.file_system.exists(f.paths.syslog_files_directory));
}

TEST(ConfigSessionInitialize, CustomRootIsHonored)
{
    ConfigSessionFixture f{"/custom"};
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.provisioned_paths().config_file, "/custom/0.1.0-beta.5/config/fastecu.cfg");
    EXPECT_EQ(f.session.provisioned_paths(), resolve_config_paths("/custom", kVersion));
}

TEST(ConfigSessionInitialize, MigratesThePreviousVersionConfigThroughProvisioning)
{
    ConfigSessionFixture f;
    f.file_system.directory_entries[f.paths.base_config_directory].push_back(
        fastecu::DirEntry{.name = "0.1.0-beta.4", .is_directory = true, .modified_time_epoch_seconds = 100});
    f.file_system.files[f.paths.base_config_directory + "/0.1.0-beta.4/config/fastecu.cfg"] = {7};

    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_EQ(f.file_system.files[f.paths.config_files_directory + "fastecu.cfg"], (std::vector<std::uint8_t>{7}));
}

TEST(ConfigSessionInitialize, CopiesBundledResourcesThroughProvisioning)
{
    ConfigSessionFixture f;
    f.resource_bundle.bundles["kernels"]["k.bin"] = {1};
    f.file_system.files["kernels/k.bin"] = {1};

    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_TRUE(f.file_system.exists(f.paths.kernel_files_directory + "k.bin"));
}

TEST(ConfigSessionInitialize, AbsentSettingsTakeCompiledInDefaults)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const AppConfig& s = f.session.settings();
    EXPECT_EQ(s.window_width, "default");
    EXPECT_EQ(s.window_height, "default");
    EXPECT_EQ(s.toolbar_iconsize, "32");
    EXPECT_EQ(s.serial_port, "ttyUSB0");
    EXPECT_EQ(s.primary_definition_base, "ecuflash");
    EXPECT_EQ(s.use_romraider_definitions, "disabled");
    EXPECT_EQ(s.use_ecuflash_definitions, "disabled");
    EXPECT_EQ(s.calibration_files_directory, f.paths.calibration_files_directory);
    EXPECT_EQ(s.datalog_files_directory, f.paths.datalog_files_directory);
    EXPECT_EQ(s.selected_flash_transport, "");
    EXPECT_EQ(s.selected_log_transport, "");
    EXPECT_EQ(s.selected_log_protocol, "");
    EXPECT_EQ(s.ecuflash_definition_files_directory, "");
    EXPECT_EQ(s.romraider_logger_definition_file, "");
    EXPECT_TRUE(s.calibration_files.empty());
    EXPECT_TRUE(s.romraider_definition_files.empty());
}

TEST(ConfigSessionInitialize, LoadedScalarsOverrideDefaults)
{
    ConfigSessionFixture f;
    f.put_settings(setting("serial_port", "COM7") + setting("toolbar_iconsize", "24") +
                   setting("primary_definition_base", "romraider") +
                   setting("use_ecuflash_definitions", "enabled"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().serial_port, "COM7");
    EXPECT_EQ(f.session.settings().toolbar_iconsize, "24");
    EXPECT_EQ(f.session.settings().primary_definition_base, "romraider");
    EXPECT_EQ(f.session.settings().use_ecuflash_definitions, "enabled");
}

TEST(ConfigSessionInitialize, LoadedFileListsKeepTheirParserSemantics)
{
    ConfigSessionFixture f;
    f.put_settings(R"(<setting name="calibration_files"><value data="a.bin"/><value data="b.bin"/></setting>)"
                   R"(<setting name="romraider_definition_files"><value data=""/><value data="r.xml"/></setting>)");
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().calibration_files, (std::vector<std::string>{"a.bin", "b.bin"}));
    EXPECT_EQ(f.session.settings().romraider_definition_files, (std::vector<std::string>{"r.xml"}));
}

TEST(ConfigSessionInitialize, NormalizesDirectoriesInMemoryAndOnDisk)
{
    ConfigSessionFixture f;
    f.put_settings(setting("calibration_files_directory", "/cal") +
                   setting("ecuflash_definition_files_directory", "C:\\defs\\") +
                   setting("datalog_files_directory", "/logs"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().calibration_files_directory, "/cal/");
    EXPECT_EQ(f.session.settings().ecuflash_definition_files_directory, "C:\\defs\\"); // backslash accepted
    EXPECT_EQ(f.session.settings().datalog_files_directory, "/logs/");
    EXPECT_THAT(f.text(f.paths.config_file), HasSubstr(R"(data="/cal/")"));
}

TEST(ConfigSessionInitialize, RewriteFailureAfterLoadIsAWarningNotAFailure)
{
    ConfigSessionFixture f;
    f.put_settings(setting("calibration_files_directory", "/cal"));
    f.file_repository.write_errors[f.paths.config_file] = Error{ErrorKind::Internal, "disk full"};

    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_TRUE(has_log(f.events, LogLevel::Warning, f.paths.config_file));
    EXPECT_TRUE(has_log(f.events, LogLevel::Warning, "disk full"));
    EXPECT_EQ(f.session.settings().calibration_files_directory, "/cal"); // unnormalized: nothing was written
}

TEST(ConfigSessionInitialize, MissingSettingsFileFailsNamingIt)
{
    ConfigSessionFixture f;
    f.file_repository.files.erase(f.paths.config_file);
    EXPECT_THAT(f.initialize(), IsErrWith(ErrorKind::InvalidConfig, HasSubstr(f.paths.config_file)));
    EXPECT_FALSE(f.session.initialized());
}

TEST(ConfigSessionInitialize, MalformedSettingsFileFailsNamingIt)
{
    ConfigSessionFixture f;
    f.put(f.paths.config_file, "<config");
    EXPECT_THAT(f.initialize(), IsErrWith(ErrorKind::InvalidConfig, HasSubstr(f.paths.config_file)));
}

TEST(ConfigSessionInitialize, MissingProtocolsFileFailsNamingIt)
{
    ConfigSessionFixture f;
    f.file_repository.files.erase(f.paths.protocols_file);
    EXPECT_THAT(f.initialize(), IsErrWith(ErrorKind::InvalidConfig, HasSubstr(f.paths.protocols_file)));
}

TEST(ConfigSessionInitialize, EmptyVehicleCatalogIsRejected)
{
    ConfigSessionFixture f;
    f.put_protocols(R"(<config name="FastECU"><protocols/><car_models/></config>)");
    EXPECT_THAT(f.initialize(), IsErrWith(ErrorKind::InvalidConfig, HasSubstr(f.paths.protocols_file)));
    EXPECT_FALSE(f.session.initialized());
}

TEST(ConfigSessionInitialize, ProvisioningFailureCarriesThePathAndReason)
{
    ConfigSessionFixture f;
    f.file_system.create_directory_error = Error{ErrorKind::Internal, "permission denied"};
    EXPECT_THAT(f.initialize(),
                IsErrWith(ErrorKind::Internal, AllOf(HasSubstr("/root"), HasSubstr("permission denied"))));
}

TEST(ConfigSessionInitialize, FailedInitializationExposesNothing)
{
    ConfigSessionFixture f;
    f.put_protocols(R"(<config name="FastECU"><protocols/><car_models/></config>)");
    ASSERT_FALSE(f.initialize().has_value());

    EXPECT_TRUE(f.session.vehicles().empty());
    EXPECT_FALSE(f.session.selected_row().has_value());
    EXPECT_EQ(f.session.selected_vehicle(), nullptr);
    const std::size_t writes = f.file_repository.write_calls.size();
    EXPECT_FALSE(f.session.save().has_value());
    EXPECT_EQ(f.file_repository.write_calls.size(), writes);
}

TEST(ConfigSessionInitialize, AFailedReinitializationDropsTheEarlierState)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.put_protocols(R"(<config name="FastECU"><protocols/><car_models/></config>)");
    ASSERT_FALSE(f.initialize().has_value());
    EXPECT_FALSE(f.session.initialized());
    EXPECT_TRUE(f.session.vehicles().empty());
}

TEST(ConfigSessionInitialize, AccessBeforeInitializationIsChecked)
{
    ConfigSessionFixture f;
    EXPECT_FALSE(f.session.initialized());
    EXPECT_TRUE(f.session.vehicles().empty());
    EXPECT_THAT(f.session.selected_row(), IsErr(ErrorKind::Internal));
    EXPECT_EQ(f.session.selected_vehicle(), nullptr);
    EXPECT_THAT(f.session.save(), IsErr(ErrorKind::Internal));
}

// --- saved row ------------------------------------------------------------

class InvalidSavedId : public ::testing::TestWithParam<std::string>
{
};

TEST_P(InvalidSavedId, SelectsRowZero)
{
    ConfigSessionFixture f;
    f.put_settings(setting("protocol_id", GetParam()));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().selected_protocol_id, "0");
    ASSERT_THAT(f.session.selected_row(), IsOk());
    EXPECT_EQ(*f.session.selected_row(), 0u);
    EXPECT_EQ(f.session.selected_vehicle()->model, "Impreza");
}

// The fixture has four rows, so "4" is the first out-of-range id.
INSTANTIATE_TEST_SUITE_P(ConfigSessionInitialize, InvalidSavedId,
                         ::testing::Values("", "-1", "abc", "1x", " 1", "+1", "4", "99999999999999999999999"));

TEST(ConfigSessionInitialize, ValidSavedIdIsKept)
{
    ConfigSessionFixture f;
    f.put_settings(setting("protocol_id", "2"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(*f.session.selected_row(), 2u);
    EXPECT_EQ(f.session.selected_vehicle()->model, "Forester");
}

TEST(ConfigSessionInitialize, RestoringTheSavedRowKeepsSavedTransports)
{
    ConfigSessionFixture f;
    // Row 1's protocol defaults would be K-Line / K-Line / MUT_DMA.
    f.put_settings(setting("protocol_id", "1") + setting("flash_transport", "CAN") +
                   setting("log_transport", "J2534") + setting("log_protocol", "SSM"));
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.settings().selected_log_transport, "J2534");
    EXPECT_EQ(f.session.settings().selected_log_protocol, "SSM");
}

// --- vehicle records ------------------------------------------------------

TEST(ConfigSessionVehicles, KeepFileOrder)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const auto vehicles = f.session.vehicles();
    ASSERT_EQ(vehicles.size(), 4u);
    EXPECT_EQ(vehicles[0].model, "Impreza");
    EXPECT_EQ(vehicles[1].model, "Colt");
    EXPECT_EQ(vehicles[2].model, "Forester");
    EXPECT_EQ(vehicles[3].model, "Skyline");
}

TEST(ConfigSessionVehicles, SharedProtocolRowsResolveToTheSameEntry)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    ASSERT_TRUE(f.session.vehicles()[0].protocol.has_value());
    ASSERT_TRUE(f.session.vehicles()[2].protocol.has_value());
    EXPECT_EQ(f.session.vehicles()[0].protocol->description, "Protocol A");
    EXPECT_EQ(f.session.vehicles()[2].protocol->description, "Protocol A");
}

TEST(ConfigSessionVehicles, UnresolvedReferenceStaysNullopt)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.vehicles()[3].protocol_name, "missing_proto");
    EXPECT_FALSE(f.session.vehicles()[3].protocol.has_value());
}

// --- paths ----------------------------------------------------------------

TEST(ConfigSessionPaths, EffectivePathsStartAsTheProvisionedPaths)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.effective_paths(), f.session.provisioned_paths());
}

TEST(ConfigSessionPaths, EditingConfigurableDirectoriesMovesOnlyThose)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const ConfigPaths provisioned = f.session.provisioned_paths();

    f.session.settings().calibration_files_directory = "/cal/";
    f.session.settings().datalog_files_directory = "/logs/";

    ConfigPaths expected = provisioned;
    expected.calibration_files_directory = "/cal/";
    expected.datalog_files_directory = "/logs/";
    EXPECT_EQ(f.session.effective_paths(), expected);
    EXPECT_EQ(f.session.provisioned_paths(), provisioned);
}

TEST(ConfigSessionPaths, EmptyConfigurableDirectoryFallsBackToProvisioned)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().calibration_files_directory.clear();
    EXPECT_EQ(f.session.effective_paths().calibration_files_directory, f.paths.calibration_files_directory);
}

TEST(ConfigSessionPaths, DefinitionSearchSettingsAreNotResourceLocations)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().ecuflash_definition_files_directory = "/defs/";
    EXPECT_EQ(f.session.effective_paths().definition_files_directory, f.paths.definition_files_directory);
}

// --- saving ---------------------------------------------------------------

TEST(ConfigSessionSave, NormalizesAndUpdatesTheInMemorySettings)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().calibration_files_directory = "/new";
    f.session.settings().serial_port = "COM3";

    ASSERT_THAT(f.session.save(), IsOk());

    EXPECT_EQ(f.session.settings().calibration_files_directory, "/new/");
    EXPECT_THAT(f.text(f.paths.config_file), HasSubstr(R"(data="COM3")"));
}

TEST(ConfigSessionSave, FailureKeepsEditsAndNamesTheFile)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().calibration_files_directory = "/new";
    f.session.settings().serial_port = "COM3";
    f.file_repository.write_errors[f.paths.config_file] = Error{ErrorKind::InvalidConfig, "cannot open file"};

    EXPECT_THAT(f.session.save(), IsErrWith(ErrorKind::InvalidConfig, AllOf(HasSubstr(f.paths.config_file),
                                                                            HasSubstr("cannot open file"))));
    EXPECT_EQ(f.session.settings().calibration_files_directory, "/new");
    EXPECT_EQ(f.session.settings().serial_port, "COM3");
}

TEST(ConfigSessionSave, EveryOtherSettingRoundTripsThroughARestart)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().serial_port = "COM3";
    f.session.settings().selected_flash_transport = "CAN";
    f.session.settings().romraider_definition_files = {"r1.xml", "r2.xml"};
    f.session.settings().calibration_files_directory = "/cal/";
    ASSERT_THAT(f.session.save(), IsOk());

    ASSERT_THAT(f.initialize(), IsOk()); // a restart over the same stores
    EXPECT_EQ(f.session.settings().serial_port, "COM3");
    EXPECT_EQ(f.session.settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.settings().romraider_definition_files, (std::vector<std::string>{"r1.xml", "r2.xml"}));
    EXPECT_EQ(f.session.settings().calibration_files_directory, "/cal/");
}

// Known, preserved mismatch: the writer emits logfiles_directory, the reader
// only recognizes datalog_files_directory. Do not fix it here.
TEST(ConfigSessionSave, DatalogDirectoryDoesNotRoundTrip)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    f.session.settings().datalog_files_directory = "/elsewhere/";
    ASSERT_THAT(f.session.save(), IsOk());
    EXPECT_THAT(f.text(f.paths.config_file), HasSubstr(R"(name="logfiles_directory")"));

    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(f.session.settings().datalog_files_directory, f.paths.datalog_files_directory);
}

} // namespace
```

Add to `src/backend/config/BUILD.bazel`:

```python
cc_library(
    name = "config_session",
    srcs = ["config_session.cpp"],
    hdrs = ["config_session.h"],
    # The package default plus the composition root that owns the session.
    visibility = [
        "//apps/desktop:__pkg__",
        "//src/backend:__subpackages__",
        "//src/platform:__subpackages__",
        "//src/ui:__subpackages__",
        "//tests:__pkg__",
    ],
    deps = [
        ":app_config",
        ":car_model_catalog",
        ":config_paths",
        ":protocol_catalog",
        ":provisioning",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "config_session_test",
    srcs = ["config_session_test.cpp"],
    deps = [
        ":config_session",
        "//src/backend/config/testing:config_session_fixture",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

In `bazel/portable_targets.bzl`, add `"config_session",` to the `"src/backend/config"` list, keeping it alphabetical (after `"car_model_catalog"`).

- [ ] **Step 3: Run the tests and confirm they fail**

Run: `bazel test --config=release //src/backend/config:config_session_test`
Expected: build FAIL (`config_session.h` not found).

- [ ] **Step 4: Implement**

`src/backend/config/config_session.h`:

```cpp
#pragma once
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "src/backend/config/app_config.h"
#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/config_paths.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/file_system.h"
#include "src/backend/ports/resource_bundle.h"
#include "src/backend/ports/result.h"

namespace fastecu::config
{

// The application's configuration for one run: provisioned paths, the
// user's settings, and the vehicle catalog. AppConfig::selected_protocol_id
// is the only saved selection; everything about the selected vehicle is
// derived from its ResolvedCarModel rather than cached.
class ConfigSession
{
  public:
    ConfigSession(IFileSystem& file_system, IResourceBundle& resource_bundle, IFileRepository& file_repository,
                  IEventSink& events);
    ConfigSession(const ConfigSession&) = delete;
    ConfigSession& operator=(const ConfigSession&) = delete;

    // Provisions <app_root>/<version>/, loads settings and both catalogs, and
    // validates the saved row (an invalid one becomes "0"). A failed rewrite
    // of the loaded settings is a warning event, not a failure. Any other
    // failure leaves the session uninitialized, holding nothing.
    Status initialize(std::string_view app_root, std::string_view version);
    bool initialized() const;

    // Writes the settings. On success the in-memory settings become the
    // normalized result; on failure they are left exactly as edited.
    Status save();

    AppConfig& settings();
    const AppConfig& settings() const;

    // Where initialize() put things. Never moved by settings edits.
    ConfigPaths provisioned_paths() const;
    // The provisioned paths with the calibration and datalog directories
    // taken from settings (provisioned ones when a setting is empty).
    ConfigPaths effective_paths() const;

    // File order; a row's id is its position.
    std::span<const ResolvedCarModel> vehicles() const;
    Result<std::size_t> selected_row() const;
    // nullptr unless selected_row() has a value.
    const ResolvedCarModel *selected_vehicle() const;

  private:
    IFileSystem& file_system_;
    IResourceBundle& resource_bundle_;
    IFileRepository& file_repository_;
    IEventSink& events_;

    bool initialized_ = false;
    ConfigPaths provisioned_;
    AppConfig settings_;
    std::vector<ResolvedCarModel> vehicles_;
};

} // namespace fastecu::config
```

`src/backend/config/config_session.cpp`:

```cpp
#include "src/backend/config/config_session.h"

#include <charconv>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include "src/backend/config/protocol_catalog.h"
#include "src/backend/config/provisioning.h"

namespace fastecu::config
{
namespace
{

void default_if_empty(std::string& field, std::string_view fallback)
{
    if (field.empty())
    {
        field = fallback;
    }
}

// The compiled-in defaults ConfigValuesStructure carried. A nonempty loaded
// value wins; AppConfig itself keeps "" as its not-present value.
AppConfig with_defaults(AppConfig settings, const ConfigPaths& paths)
{
    default_if_empty(settings.window_width, "default");
    default_if_empty(settings.window_height, "default");
    default_if_empty(settings.toolbar_iconsize, "32");
    default_if_empty(settings.serial_port, "ttyUSB0");
    default_if_empty(settings.primary_definition_base, "ecuflash");
    default_if_empty(settings.use_romraider_definitions, "disabled");
    default_if_empty(settings.use_ecuflash_definitions, "disabled");
    default_if_empty(settings.calibration_files_directory, paths.calibration_files_directory);
    default_if_empty(settings.datalog_files_directory, paths.datalog_files_directory);
    return settings;
}

// The row a saved protocol_id names: a plain decimal index below row_count.
// Negative, signed, padded, malformed, overflowing, and out-of-range text
// all yield nullopt.
std::optional<std::size_t> parse_row(std::string_view text, std::size_t row_count)
{
    std::size_t row = 0;
    const char *first = text.data();
    const char *last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, row);
    if (error != std::errc{} || end != last || row >= row_count)
    {
        return std::nullopt;
    }
    return row;
}

std::unexpected<Error> failed(const Error& error, std::string_view what, std::string_view path)
{
    return std::unexpected(Error{error.kind, std::format("{} {}: {}", what, path, error.detail)});
}

} // namespace

ConfigSession::ConfigSession(IFileSystem& file_system, IResourceBundle& resource_bundle,
                             IFileRepository& file_repository, IEventSink& events)
    : file_system_(file_system), resource_bundle_(resource_bundle), file_repository_(file_repository),
      events_(events)
{
}

Status ConfigSession::initialize(std::string_view app_root, std::string_view version)
{
    initialized_ = false;
    provisioned_ = {};
    settings_ = {};
    vehicles_.clear();

    const ConfigPaths paths = resolve_config_paths(app_root, version);
    if (Status provisioned = provision_config_directories(paths, file_system_, resource_bundle_, events_);
        !provisioned.has_value())
    {
        return std::unexpected(Error{provisioned.error().kind, std::format("Unable to provision configuration: {}",
                                                                           provisioned.error().detail)});
    }

    Result<AppConfig> parsed = parse_app_config(paths, file_repository_);
    if (!parsed.has_value())
    {
        return failed(parsed.error(), "Unable to load settings", paths.config_file);
    }
    AppConfig settings = with_defaults(std::move(*parsed), paths);
    // The legacy loader rewrote the file on every load and ignored the
    // result. Keep the rewrite, but observe it: a failure is nonfatal.
    if (Result<AppConfig> rewritten = save_app_config(settings, paths, file_repository_); rewritten.has_value())
    {
        settings = std::move(*rewritten);
    }
    else
    {
        events_.log(LogLevel::Warning, std::format("Unable to save settings {}: {}", paths.config_file,
                                                   rewritten.error().detail));
    }

    Result<ProtocolCatalog> protocols = load_protocol_catalog(paths, file_repository_);
    if (!protocols.has_value())
    {
        return failed(protocols.error(), "Unable to load protocols", paths.protocols_file);
    }
    Result<CarModelCatalog> car_models = load_car_model_catalog(paths, file_repository_);
    if (!car_models.has_value())
    {
        return failed(car_models.error(), "Unable to load vehicles", paths.protocols_file);
    }
    std::vector<ResolvedCarModel> vehicles = resolve_car_models(*protocols, *car_models);
    if (vehicles.empty())
    {
        return fail(ErrorKind::InvalidConfig, std::format("No vehicles defined in {}", paths.protocols_file));
    }

    if (!parse_row(settings.selected_protocol_id, vehicles.size()).has_value())
    {
        settings.selected_protocol_id = "0";
    }

    provisioned_ = paths;
    settings_ = std::move(settings);
    vehicles_ = std::move(vehicles);
    initialized_ = true;
    return {};
}

bool ConfigSession::initialized() const
{
    return initialized_;
}

Status ConfigSession::save()
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    Result<AppConfig> saved = save_app_config(settings_, provisioned_, file_repository_);
    if (!saved.has_value())
    {
        return failed(saved.error(), "Unable to save settings", provisioned_.config_file);
    }
    settings_ = std::move(*saved);
    return {};
}

AppConfig& ConfigSession::settings()
{
    return settings_;
}

const AppConfig& ConfigSession::settings() const
{
    return settings_;
}

ConfigPaths ConfigSession::provisioned_paths() const
{
    return provisioned_;
}

ConfigPaths ConfigSession::effective_paths() const
{
    ConfigPaths paths = provisioned_;
    if (!settings_.calibration_files_directory.empty())
    {
        paths.calibration_files_directory = settings_.calibration_files_directory;
    }
    if (!settings_.datalog_files_directory.empty())
    {
        paths.datalog_files_directory = settings_.datalog_files_directory;
    }
    return paths;
}

std::span<const ResolvedCarModel> ConfigSession::vehicles() const
{
    return vehicles_;
}

Result<std::size_t> ConfigSession::selected_row() const
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    const std::optional<std::size_t> row = parse_row(settings_.selected_protocol_id, vehicles_.size());
    if (!row.has_value())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("selected protocol id '{}' names no vehicle", settings_.selected_protocol_id));
    }
    return *row;
}

const ResolvedCarModel *ConfigSession::selected_vehicle() const
{
    const Result<std::size_t> row = selected_row();
    return row.has_value() ? &vehicles_[*row] : nullptr;
}

} // namespace fastecu::config
```

- [ ] **Step 5: Run the tests and confirm they pass**

Run: `bazel test --config=release //src/backend/config:all && bazel build --config=release //:portable_closure`
Expected: PASS; the portable closure builds, so no platform label is reachable from `config_session`.

- [ ] **Step 6: Commit**

```bash
git add src/backend/config bazel/portable_targets.bzl src/backend/ports/testing/BUILD.bazel
git commit -m "feat(config): add the portable configuration session (step 6k-1)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `ConfigSession` selection

**Files:**
- Modify: `src/backend/config/config_session.h`, `src/backend/config/config_session.cpp`, `src/backend/config/config_session_test.cpp`

**Interfaces:**
- Consumes: `find_car_model_by_protocol_name` (`car_model_catalog.h`), Task 2's `ConfigSession`.
- Produces (in `config_session.h`):
  ```cpp
  inline constexpr std::string_view kMissingProtocolField = " ";
  std::string protocol_field_or_placeholder(const ResolvedCarModel&, std::string ProtocolEntry::*field);
  Status ConfigSession::select_row(std::size_t row);             // InvalidConfig if out of range; Internal before init
  bool ConfigSession::select_by_protocol_name(std::string_view); // last match; false (and no change) if none
  ```
  Selection sets `selected_protocol_id` to the row and `selected_log_protocol` to `protocol_field_or_placeholder(row, &ProtocolEntry::log_protocol)`. The flash and log transports are left unchanged. Selection never saves.

- [ ] **Step 1: Write the failing tests**

Append inside the anonymous namespace of `config_session_test.cpp`, and add `using fastecu::config::kMissingProtocolField;`, `using fastecu::config::ProtocolEntry;`, `using fastecu::config::protocol_field_or_placeholder;`:

```cpp
// --- selection ------------------------------------------------------------

TEST(ConfigSessionSelect, RowChangesTheSavedRowAndLogProtocolOnly)
{
    ConfigSessionFixture f;
    f.put_settings(setting("flash_transport", "CAN") + setting("log_transport", "J2534") +
                   setting("log_protocol", "SSM"));
    ASSERT_THAT(f.initialize(), IsOk());

    ASSERT_THAT(f.session.select_row(1), IsOk());

    EXPECT_EQ(f.session.settings().selected_protocol_id, "1");
    EXPECT_EQ(f.session.settings().selected_log_protocol, "MUT_DMA");
    EXPECT_EQ(f.session.settings().selected_flash_transport, "CAN");
    EXPECT_EQ(f.session.settings().selected_log_transport, "J2534");
    EXPECT_EQ(f.session.selected_vehicle()->model, "Colt");
}

TEST(ConfigSessionSelect, InvalidRowFailsWithoutChangingSettings)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const AppConfig before = f.session.settings();

    EXPECT_THAT(f.session.select_row(4), IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(f.session.settings(), before);
}

TEST(ConfigSessionSelect, BeforeInitializationFails)
{
    ConfigSessionFixture f;
    EXPECT_THAT(f.session.select_row(0), IsErr(ErrorKind::Internal));
    EXPECT_FALSE(f.session.select_by_protocol_name("proto_a"));
}

TEST(ConfigSessionSelect, SelectionDoesNotSave)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    const std::size_t writes = f.file_repository.write_calls.size();
    ASSERT_THAT(f.session.select_row(2), IsOk());
    EXPECT_TRUE(f.session.select_by_protocol_name("proto_b"));
    EXPECT_EQ(f.file_repository.write_calls.size(), writes);
}

TEST(ConfigSessionSelect, ProtocolNameUsesTheLastMatchingRow)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_TRUE(f.session.select_by_protocol_name("proto_a")); // rows 0 and 2

    EXPECT_EQ(*f.session.selected_row(), 2u);
    EXPECT_EQ(f.session.settings().selected_log_protocol, "SSM");
}

TEST(ConfigSessionSelect, UnmatchedProtocolNameChangesNothing)
{
    ConfigSessionFixture f;
    f.put_settings(setting("protocol_id", "1") + setting("log_protocol", "CDBG"));
    ASSERT_THAT(f.initialize(), IsOk());
    const AppConfig before = f.session.settings();

    EXPECT_FALSE(f.session.select_by_protocol_name("no_such_protocol"));
    EXPECT_EQ(f.session.settings(), before);
}

TEST(ConfigSessionSelect, UnresolvedRowSelectsWithPlaceholders)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());

    EXPECT_TRUE(f.session.select_by_protocol_name("missing_proto"));

    EXPECT_EQ(*f.session.selected_row(), 3u);
    EXPECT_EQ(f.session.settings().selected_log_protocol, std::string(kMissingProtocolField));
    const auto& vehicle = *f.session.selected_vehicle();
    EXPECT_FALSE(vehicle.protocol.has_value());
    EXPECT_EQ(protocol_field_or_placeholder(vehicle, &ProtocolEntry::mcu), " ");
    EXPECT_EQ(protocol_field_or_placeholder(vehicle, &ProtocolEntry::read), " "); // not "yes": unavailable
}

TEST(ConfigSessionSelect, PlaceholderHelperReturnsResolvedFields)
{
    ConfigSessionFixture f;
    ASSERT_THAT(f.initialize(), IsOk());
    EXPECT_EQ(protocol_field_or_placeholder(f.session.vehicles()[1], &ProtocolEntry::checksum), "n/a");
    EXPECT_EQ(protocol_field_or_placeholder(f.session.vehicles()[0], &ProtocolEntry::description), "Protocol A");
}
```

- [ ] **Step 2: Run the tests and confirm they fail**

Run: `bazel test --config=release //src/backend/config:config_session_test`
Expected: build FAIL (`select_row`, `select_by_protocol_name`, `protocol_field_or_placeholder`, `kMissingProtocolField` undeclared).

- [ ] **Step 3: Implement**

In `config_session.h`, add `#include <string>` and, before `class ConfigSession`:

```cpp
// What legacy code showed for a protocol field of a vehicle whose protocol
// reference did not resolve: a single space, not an empty string.
inline constexpr std::string_view kMissingProtocolField = " ";

// `row.protocol->*field`, or kMissingProtocolField when the reference did not
// resolve. Capabilities ("read", "write", ...) therefore never read "yes" for
// an unresolved row.
std::string protocol_field_or_placeholder(const ResolvedCarModel& row, std::string ProtocolEntry::*field);
```

In the class's public section, after `selected_vehicle()`:

```cpp
    // Makes `row` the saved row and sets the logging protocol from its
    // protocol (the placeholder when unresolved). Transports are untouched,
    // and nothing is written until save(). An invalid row changes nothing.
    Status select_row(std::size_t row);
    // select_row() on the LAST row whose protocol_name matches, as the
    // legacy ROM-open scan did. Returns false, changing nothing, if none do.
    bool select_by_protocol_name(std::string_view protocol_name);
```

In `config_session.cpp`:

```cpp
std::string protocol_field_or_placeholder(const ResolvedCarModel& row, std::string ProtocolEntry::*field)
{
    return row.protocol.has_value() ? (*row.protocol).*field : std::string(kMissingProtocolField);
}

Status ConfigSession::select_row(std::size_t row)
{
    if (!initialized_)
    {
        return fail(ErrorKind::Internal, "configuration session is not initialized");
    }
    if (row >= vehicles_.size())
    {
        return fail(ErrorKind::InvalidConfig,
                    std::format("vehicle row {} is out of range ({} rows)", row, vehicles_.size()));
    }
    settings_.selected_protocol_id = std::to_string(row);
    settings_.selected_log_protocol = protocol_field_or_placeholder(vehicles_[row], &ProtocolEntry::log_protocol);
    return {};
}

bool ConfigSession::select_by_protocol_name(std::string_view protocol_name)
{
    if (!initialized_)
    {
        return false;
    }
    const std::optional<std::size_t> row = find_car_model_by_protocol_name(vehicles_, protocol_name);
    return row.has_value() && select_row(*row).has_value();
}
```

- [ ] **Step 4: Run the tests and confirm they pass**

Run: `bazel test --config=release //src/backend/config:all`
Expected: PASS.

- [ ] **Step 5: Run the PR gates and commit**

```bash
prek run --all-files
bazel build -k --config=release //...
bazel test -k --config=release //...
bazel run --config=release //:clang_tidy_report_changed
git add src/backend/config
git commit -m "feat(config): select vehicles by row and protocol name (step 6k-1)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: all green. Nothing consumes the session yet, so no other target changes behavior.

---

## PR 6k-2 — Consumer migration and retirement

### Task 4: `FileActions`-owned definition indexes

**Files:**
- Create: `src/backend/definitions/definition_indexes.h`
- Modify: `src/backend/definitions/BUILD.bazel`, `src/backend/definitions/config_values.h`, `src/backend/definitions/file_actions.h`, `file_actions.cpp`, `file_actions_ecuflash.cpp`, `file_actions_romraider.cpp`, `file_actions_parsing_test.cpp`
- Modify: `src/backend/definition/legacy/legacy_definition_adapter.{h,cpp}`, `legacy_definition_adapter_test.cpp`, `BUILD.bazel`
- Modify: `src/ui/desktop/definition/definition_authoring_dialog.{h,cpp}`, `definition_authoring_dialog_test.cpp`, and its `BUILD.bazel` deps

**Interfaces:**
- Produces: `fastecu::definitions::DefinitionIndexes` in `src/backend/definitions/definition_indexes.h`, label `//src/backend/definitions:definition_indexes`:
  ```cpp
  struct DefinitionIndexes {
      bool operator==(const DefinitionIndexes&) const = default;
      QStringList ecuflash_def_cal_id, ecuflash_def_cal_id_addr, ecuflash_def_ecu_id, ecuflash_def_filename;
      QStringList romraider_def_cal_id, romraider_def_cal_id_addr, romraider_def_ecu_id, romraider_def_filename;
  };
  ```
- Produces: `FileActions::DefinitionIndexes` (alias) and public member `FileActions::definitionIndexes`.
- Produces: `LegacyDefinitionAdapter::replace_romraider_catalog(definitions::DefinitionIndexes&, std::span<const std::string>)` and `replace_ecuflash_catalog(definitions::DefinitionIndexes&, std::string_view, std::span<const std::string>)`.
- Produces: `void record_definition(fastecu::definitions::DefinitionIndexes&, const HeaderFormEditors&, const fastecu::definition::DefinitionHeaderInput&, const QString&)`.
- `ConfigValuesStructure` loses its eight `*_def_*` lists. Nothing else about it changes in this task.

- [ ] **Step 1: Create the 6k-2 branch**

```bash
git switch refactor/step6k-1-config-session
git switch -c refactor/step6k-2-config-consumers
```

- [ ] **Step 2: Update the tests to the new owner (they fail to compile)**

In `definition_authoring_dialog_test.cpp`'s `RecordDefinitionAppendsTheFourConfigListsInStep`, rename the test to `RecordDefinitionAppendsTheFourIndexListsInStep`. Replace `FileActions::ConfigValuesStructure config;` with `fastecu::definitions::DefinitionIndexes config;` and include `src/backend/definitions/definition_indexes.h`. The assertions are unchanged.

In `file_actions_parsing_test.cpp`, replace every `actions.ConfigValuesStruct.romraider_def_*` / `ecuflash_def_*` read with `actions.definitionIndexes.<same name>`. Replace each `actions.create_romraider_def_id_list(&actions.ConfigValuesStruct)` return-value comparison with a plain call, since the method returns `void` from this task on:

```cpp
        actions.create_romraider_def_id_list();
```

Do the same for `create_ecuflash_def_id_list`. The inputs (`romraider_definition_files`, `ecuflash_definition_files_directory`) stay on `ConfigValuesStruct` until Task 5.

In `legacy_definition_adapter_test.cpp`, change every `definitions::ConfigValuesStructure` used as a `replace_*_catalog` argument to `definitions::DefinitionIndexes`.

Add one new case to `file_actions_parsing_test.cpp` that pins ordering across a rebuild:

```cpp
    void definition_index_rebuild_keeps_file_order()
    {
        // Two RomRaider definition files; the index lists follow the order
        // of romraider_definition_files, whatever the IDs sort to.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString second = writeTextFile(dir, "b.xml", romraiderDefinitionWithId("ZZZ_FIRST"));
        const QString first = writeTextFile(dir, "a.xml", romraiderDefinitionWithId("AAA_SECOND"));
        fastecu::RecordingEventSink eventSink;
        FileActions actions(fileSystem_, resourceBundle_, fileRepository_, atomicFileWriter_, eventSink);
        actions.ConfigValuesStruct.romraider_definition_files = {second, first};

        actions.create_romraider_def_id_list();

        QCOMPARE(actions.definitionIndexes.romraider_def_filename, (QStringList{second, first}));
        QCOMPARE(actions.definitionIndexes.romraider_def_cal_id, (QStringList{"ZZZ_FIRST", "AAA_SECOND"}));
    }
```

`romraiderDefinitionWithId` is a new helper in the test's anonymous namespace. It returns the minimal RomRaider XML the existing `MINIMAL_TEST` case writes, with the `<xmlid>` text replaced by the argument. Copy that literal from the existing case.

- [ ] **Step 3: Run the tests and confirm they fail**

Run: `bazel test --config=release //src/backend/definitions:all //src/backend/definition/legacy:all //src/ui/desktop/definition:all`
Expected: build FAIL (`definition_indexes.h` not found, `definitionIndexes` has no member).

- [ ] **Step 4: Implement**

`src/backend/definitions/definition_indexes.h`:

```cpp
#pragma once

#include <QStringList>

namespace fastecu::definitions
{

// FileActions's legacy EcuFlash/RomRaider definition indexes: four parallel
// lists per format (calibration ID, its ROM address, ECU ID, and source file).
// Built from the configured definition sources and appended to by
// definition authoring. They are derived data, not application settings,
// so they live beside FileActions rather than in the configuration session.
struct DefinitionIndexes
{
    bool operator==(const DefinitionIndexes&) const = default;

    QStringList ecuflash_def_cal_id;
    QStringList ecuflash_def_cal_id_addr;
    QStringList ecuflash_def_ecu_id;
    QStringList ecuflash_def_filename;
    QStringList romraider_def_cal_id;
    QStringList romraider_def_cal_id_addr;
    QStringList romraider_def_ecu_id;
    QStringList romraider_def_filename;
};

} // namespace fastecu::definitions
```

In `src/backend/definitions/BUILD.bazel`, add a leaf target modeled on `config_values`, with the same `copts`/`deps` shape:

```python
# FileActions's legacy definition indexes, a leaf so
# //src/backend/definition/legacy can use them without depending on FileActions.
cc_library(
    name = "definition_indexes",
    hdrs = ["definition_indexes.h"],
    copts = COMMON_COPTS,
    deps = QT_DEPS_NO_WIDGETS,
)
```

Add `":definition_indexes"` to the `definitions` target's deps. Delete the eight `*_def_*` lists from `config_values.h`.

In `file_actions.h`: include the header; next to the other aliases add

```cpp
    using DefinitionIndexes = fastecu::definitions::DefinitionIndexes;
    DefinitionIndexes definitionIndexes;
```

and change the two declarations to `void create_romraider_def_id_list();` and `void create_ecuflash_def_id_list();`.

In `file_actions_romraider.cpp` / `file_actions_ecuflash.cpp`, change both definitions to `void` with no parameter. Read the inputs from `ConfigValuesStruct` (`romraider_definition_files`, `ecuflash_definition_files_directory`). Write the outputs to `definitionIndexes`: pass `definitionIndexes` to `definitionAdapter_.replace_*_catalog` and to `strip_legacy_address_prefixes`, and read `definitionIndexes.*` in the log lines and in the `read_*_ecu_def` lookups (`ConfigValuesStruct.romraider_def_cal_id` → `definitionIndexes.romraider_def_cal_id`, and so on). Replace `return configValues;` with `return;`.

In `file_actions.cpp`: change `catalogFromLegacyLists` to take `const FileActions::DefinitionIndexes&`, and pass it `definitionIndexes`. `definition_source` reads `definitionIndexes.*`. `build_definition_catalog` reads `definitionIndexes.ecuflash_def_filename`.

In `legacy_definition_adapter.{h,cpp}`: replace `definitions::ConfigValuesStructure` with `definitions::DefinitionIndexes` in both `replace_*_catalog` signatures and in `populate_catalog`. The copy-then-commit (`next = current; ... current = std::move(next);`) is unchanged. Swap the BUILD dep `//src/backend/definitions:config_values` → `//src/backend/definitions:definition_indexes`.

In `definition_authoring_dialog.{h,cpp}`: `record_definition` takes `fastecu::definitions::DefinitionIndexes& indexes`, and both call sites pass `fileActions_.definitionIndexes`. Update its header comment to say "the four ecuflash_def_* index lists".

In `mainwindow.cpp`, lines 214/217 become `fileActions->create_ecuflash_def_id_list();` and `fileActions->create_romraider_def_id_list();`. Update any other caller that `grep -rn "def_id_list(" src` finds.

- [ ] **Step 5: Run the tests and confirm they pass**

Run: `bazel test --config=release //src/backend/definitions:all //src/backend/definition/legacy:all //src/ui/desktop/definition:all //src/ui/desktop:all`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add -A src/backend/definitions src/backend/definition src/ui/desktop
git commit -m "refactor(definitions): move definition indexes into a FileActions-owned struct (step 6k-2)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Switch every consumer to the session

This task is one commit. Every consumer moves at once, so no synchronized mirror ever exists. Write the tests first (they fail to compile), then move each file.

**Files:**
- Create: `apps/desktop/startup_event_sink.h`, `apps/desktop/startup_diagnostics.{h,cpp}`, `apps/desktop/startup_diagnostics_test.cpp`, `apps/desktop/default_config_root.h`, `apps/desktop/default_config_root_unix.cpp`, `apps/desktop/default_config_root_windows.cpp`
- Create: `src/ui/desktop/config_fields.{h,cpp}`, `src/ui/desktop/config_fields_test.cpp`, `src/ui/desktop/vehicle_select_test.cpp`, `src/ui/desktop/protocol_select_test.cpp`
- Modify: `apps/desktop/desktop_composition.{h,cpp}`, `apps/desktop/main.cpp`, `apps/desktop/desktop_composition_test.cpp`, `apps/desktop/BUILD.bazel`
- Modify: `src/ui/desktop/main_window_services.h`, `mainwindow.{h,cpp}`, `menu_actions.cpp`, `log_operations_ssm.cpp`, `settings.{h,cpp}`, `vehicle_select.{h,cpp}`, `protocol_select.{h,cpp}`, `mainwindow_test.cpp`, `settings_test.cpp`, `BUILD.bazel`
- Modify: `src/ui/desktop/definition/definition_authoring_dialog.{h,cpp}` + test + BUILD
- Modify: `src/backend/definitions/file_actions.{h,cpp}`, `file_actions_ecuflash.cpp`, `file_actions_romraider.cpp`, `file_actions_parsing_test.cpp`, `BUILD.bazel`
- Modify: `src/backend/calibration/legacy/legacy_calibration_adapter.{h,cpp}`, `legacy_calibration_adapter_test.cpp`, `BUILD.bazel`

**Interfaces:**
- Consumes: `ConfigSession` (Tasks 2–3), `DefinitionIndexes` (Task 4).
- Produces: `struct ApplicationIdentity { std::string name; std::string title; std::string version; };` in `main_window_services.h`. `MainWindowServices` becomes, in this order: `application` (`const ApplicationIdentity&`), `config` (`fastecu::config::ConfigSession&`), `file_actions`, `config_repository`, `file_action_events`, `log`, `connection`, `remote`, `logging_engine`.
- Produces: `DesktopComposition::started() const`, `const std::optional<fastecu::Error>& startup_error() const`, `QStringList startup_warnings() const`. `services()` has the precondition `started()`.
- Produces: `QString default_config_root()` (`apps/desktop/default_config_root.h`).
- Produces: `QString startup_failure_text(const fastecu::Error&)`, `QString startup_warning_text(const QStringList&)`, `void present_startup_failure(const fastecu::Error&)`, `void present_startup_warnings(const QStringList&)` (`apps/desktop/startup_diagnostics.h`).
- Produces: `FileActions(IFileSystem&, IResourceBundle&, IFileRepository&, IAtomicFileWriter&, IEventSink&, fastecu::config::ConfigSession&)`. `set_base_dirs`, `check_config_dirs`, `read_config_file`, `save_config_file`, `read_protocols_file`, and `ConfigValuesStruct` are removed.
- Produces: `LegacyCalibrationAdapter::open_rom_bytes(EcuCalDefStructure&, QString filename, std::string_view calibration_files_directory)`. `bind_protocol` and the catalog cache are removed.
- Produces: `VehicleSelect(const fastecu::config::ConfigSession&, QWidget* = nullptr)` with `std::optional<std::size_t> chosen_row() const`. `ProtocolSelect(const fastecu::config::ConfigSession&, QWidget* = nullptr)` with `std::optional<std::string> chosen_protocol_name() const`.
- Produces: `Settings(fastecu::config::ConfigSession&, QWidget* = nullptr)`.
- Produces: `DefinitionAuthoringDialog(FileActions&, const fastecu::config::ConfigSession&, fastecu::IFileRepository&, QWidget*)`.
- Produces (`MainWindow`, private, reachable from tests): `fastecu::config::ConfigSession *configSession`, `void apply_vehicle_choice(int result, std::optional<std::size_t> row)`, `void apply_protocol_choice(int result, std::optional<std::string> protocol_name)`, `void save_settings()`, `const fastecu::config::ResolvedCarModel& selected_vehicle() const`.
- Produces (`src/ui/desktop/config_fields.h`, label `//src/ui/desktop:config_fields`): `fastecu::ui::qs(std::string_view)`, `protocol_field(const ResolvedCarModel&, std::string ProtocolEntry::*)`, `protocol_capability(const ResolvedCarModel&, std::string ProtocolEntry::*)`, `qstring_list(const std::vector<std::string>&)`, `string_vector(const QStringList&)`.

#### 5a. Tests first

- [ ] **Step 1: Composition and startup-diagnostics tests**

Add to `DesktopCompositionTest` in `apps/desktop/desktop_composition_test.cpp` (add `#include <QScopeGuard>` and `#include "apps/desktop/startup_diagnostics.h"`). Also add a file-local `writeFile(path, text)` helper that returns `bool` and writes UTF-8 with `QFile`, plus `constexpr auto kVersion = "0.1.0-beta.5";`.

```cpp
    void failedStartupBuildsNoServicesAndPerformsNoEcuIo()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString config_dir = root.path() + "/" + kVersion + "/config/";
        QVERIFY(QDir().mkpath(config_dir));
        QVERIFY(writeFile(config_dir + "protocols.cfg", R"(<config name="FastECU"><protocols/><car_models/></config>)"));

        DesktopComposition composition{{}, {}, root.path()};

        QVERIFY(!composition.started());
        QVERIFY(composition.startup_error().has_value());
        QVERIFY(QString::fromStdString(composition.startup_error()->detail).contains(config_dir + "protocols.cfg"));
        // Nothing that could log, thread, or talk to an ECU was created.
        QVERIFY(!composition.file_actions_);
        QVERIFY(!composition.syslog_thread_);
        QVERIFY(!composition.syslogger_);
        QVERIFY(!composition.serial_);
        QVERIFY(!composition.connection_);
        QVERIFY(!composition.remote_utility_);
        QVERIFY(!composition.logging_engine_);
    } // teardown after a failed start must not crash

    void malformedSettingsRejectStartup()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString config_dir = root.path() + "/" + kVersion + "/config/";
        QVERIFY(QDir().mkpath(config_dir));
        QVERIFY(writeFile(config_dir + "fastecu.cfg", "<config"));

        DesktopComposition composition{{}, {}, root.path()};

        QVERIFY(!composition.started());
        const QString text = startup_failure_text(*composition.startup_error());
        QVERIFY(text.contains(config_dir + "fastecu.cfg"));
        QVERIFY(!composition.serial_);
    }

    void settingsRewriteFailureIsAStartupWarning()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString config_dir = root.path() + "/" + kVersion + "/config/";
        QVERIFY(QDir().mkpath(config_dir));
        const QString config_file = config_dir + "fastecu.cfg";
        QVERIFY(writeFile(config_file, R"(<config name="FastECU" version="t"><software_settings/></config>)"));
        QVERIFY(QFile::setPermissions(config_file, QFileDevice::ReadOwner));
        const auto restore = qScopeGuard([&] { QFile::setPermissions(config_file, QFileDevice::ReadOwner | QFileDevice::WriteOwner); });
        if (QFile probe{config_file}; probe.open(QIODevice::WriteOnly | QIODevice::Append))
        {
            QSKIP("this filesystem ignores the read-only permission");
        }

        DesktopComposition composition{{}, {}, root.path()};

        QVERIFY(composition.started());
        QVERIFY(std::ranges::any_of(composition.startup_warnings(),
                                    [&](const QString& warning) { return warning.contains(config_file); }));
    }

    void servicesShareTheCompositionsSession()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        DesktopComposition composition{{}, {}, root.path()};
        QVERIFY(composition.started());
        QCOMPARE(&composition.services().config, &composition.config_);
        QCOMPARE(composition.services().application.version, std::string(kVersion));
        QCOMPARE(QString::fromStdString(composition.config_.provisioned_paths().base_config_directory), root.path());
    }

    void restartSeesSavedSettingsButNotTheDatalogDirectory()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        std::string provisioned_datalogs;
        {
            DesktopComposition first{{}, {}, root.path()};
            QVERIFY(first.started());
            provisioned_datalogs = first.config_.provisioned_paths().datalog_files_directory;
            first.config_.settings().serial_port = "ttyRESTART";
            first.config_.settings().datalog_files_directory = root.path().toStdString() + "/elsewhere/";
            QVERIFY(first.config_.save().has_value());
        }
        DesktopComposition second{{}, {}, root.path()};
        QVERIFY(second.started());
        QCOMPARE(second.config_.settings().serial_port, std::string("ttyRESTART"));
        QCOMPARE(second.config_.settings().datalog_files_directory, provisioned_datalogs);
    }
```

In `servicesReferToTheCompositionsOwnObjects`, delete the `file_actions.ConfigValuesStruct.base_config_directory` comparison and add `QCOMPARE(&first.config, &second.config);` and `QCOMPARE(&first.application, &second.application);`.

`apps/desktop/startup_diagnostics_test.cpp` (QtTest, `fastecu_qttest`):

```cpp
#include <QDir>
#include <QTest>

#include "apps/desktop/default_config_root.h"
#include "apps/desktop/startup_diagnostics.h"

class StartupDiagnosticsTest : public QObject
{
    Q_OBJECT

  private slots:
    void failureTextCarriesTheDetail()
    {
        const QString text = startup_failure_text(
            fastecu::Error{fastecu::ErrorKind::InvalidConfig, "Unable to load protocols /r/protocols.cfg: bad"});
        QVERIFY(text.contains("/r/protocols.cfg"));
        QVERIFY(text.contains("bad"));
    }

    void warningTextListsEveryWarning()
    {
        const QString text = startup_warning_text({"first /a.cfg", "second"});
        QVERIFY(text.contains("first /a.cfg"));
        QVERIFY(text.contains("second"));
    }

    void defaultRootIsUnderHomeAndEndsInFastEcu()
    {
        const QString root = default_config_root();
        QVERIFY(root.startsWith(QDir::homePath() + "/"));
        QVERIFY(root.endsWith("/FastECU/"));
    }
};

QTEST_MAIN(StartupDiagnosticsTest)
#include "startup_diagnostics_test.moc"
```

- [ ] **Step 2: UI tests**

`src/ui/desktop/config_fields_test.cpp` (`fastecu_gtest`):

```cpp
#include "src/ui/desktop/config_fields.h"

#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"

using fastecu::config::ProtocolEntry;
using fastecu::config::testing::ConfigSessionFixture;

TEST(ConfigFields, ResolvedFieldsAndCapabilities)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    EXPECT_EQ(fastecu::ui::protocol_field(f.session.vehicles()[0], &ProtocolEntry::mcu), QString("SH7058"));
    EXPECT_TRUE(fastecu::ui::protocol_capability(f.session.vehicles()[0], &ProtocolEntry::read));
    EXPECT_FALSE(fastecu::ui::protocol_capability(f.session.vehicles()[0], &ProtocolEntry::test_write));
}

TEST(ConfigFields, UnresolvedRowShowsThePlaceholderAndNoCapability)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    const auto& unresolved = f.session.vehicles()[3];
    EXPECT_EQ(fastecu::ui::protocol_field(unresolved, &ProtocolEntry::description), QString(" "));
    EXPECT_FALSE(fastecu::ui::protocol_capability(unresolved, &ProtocolEntry::read));
    EXPECT_FALSE(fastecu::ui::protocol_capability(unresolved, &ProtocolEntry::write));
}

TEST(ConfigFields, ListConversionsRoundTrip)
{
    const std::vector<std::string> items{"a", "b"};
    EXPECT_EQ(fastecu::ui::string_vector(fastecu::ui::qstring_list(items)), items);
}
```

`src/ui/desktop/vehicle_select_test.cpp` (QtTest, offscreen). Reach private slots with `QMetaObject::invokeMethod`:

```cpp
#include <QTest>

#include "src/backend/config/testing/config_session_fixture.h"
#include "src/ui/desktop/vehicle_select.h"

using fastecu::config::testing::ConfigSessionFixture;

class VehicleSelectTest : public QObject
{
    Q_OBJECT

  private slots:
    void choosingRecordsTheRowWithoutTouchingTheSession()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        QVERIFY(f.session.select_row(2).has_value());
        const auto before = f.session.settings();

        VehicleSelect dialog{f.session}; // opens on the session's row (Subaru Forester)
        QVERIFY(QMetaObject::invokeMethod(&dialog, "car_model_selected", Qt::DirectConnection));

        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(dialog.chosen_row(), std::optional<std::size_t>(2));
        QVERIFY(f.session.settings() == before);
    }

    void rejectingLeavesNoChoice()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        VehicleSelect dialog{f.session};
        dialog.reject();
        QVERIFY(!dialog.chosen_row().has_value());
    }
};

QTEST_MAIN(VehicleSelectTest)
#include "vehicle_select_test.moc"
```

The first case relies on the constructor's existing preselection: it selects the version item whose id equals the saved row (`vehicle_select.cpp` ~line 343), and `car_version_treewidget_item_selected` copies that id into `flash_protocol_id`. If the offscreen run shows that the slot never fires during construction, select the item explicitly before invoking `car_model_selected`. To do that, wrap the include in `#define private public` / `#undef private`, include `ui_vehicle_select.h`, call `setCurrentItem` on the `car_version_tree_widget` item whose id column reads `"2"`, and emit `itemSelectionChanged()`.

`src/ui/desktop/protocol_select_test.cpp`:

```cpp
#include <QTest>
#include <QTreeWidget>

#include "src/backend/config/testing/config_session_fixture.h"
#define private public
#include "src/ui/desktop/protocol_select.h"
#undef private
#include "ui_protocol_select.h"

using fastecu::config::testing::ConfigSessionFixture;

class ProtocolSelectTest : public QObject
{
    Q_OBJECT

  private slots:
    void listsEachVehicleBackedProtocolOnce()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        ProtocolSelect dialog{f.session};
        // proto_a (two rows), proto_b, missing_proto -- still listed, as today.
        QCOMPARE(dialog.ui->treeWidget->topLevelItemCount(), 3);
    }

    void choosingRecordsTheProtocolName()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        const auto before = f.session.settings();
        ProtocolSelect dialog{f.session};
        const auto items = dialog.ui->treeWidget->findItems("proto_b", Qt::MatchExactly, 0);
        QCOMPARE(items.size(), 1);
        dialog.ui->treeWidget->setCurrentItem(items.front());
        items.front()->setSelected(true);

        QVERIFY(QMetaObject::invokeMethod(&dialog, "car_model_selected", Qt::DirectConnection));

        QCOMPARE(dialog.chosen_protocol_name(), std::optional<std::string>("proto_b"));
        QVERIFY(f.session.settings() == before);
    }

    void rejectingLeavesNoChoice()
    {
        ConfigSessionFixture f;
        QVERIFY(f.initialize().has_value());
        ProtocolSelect dialog{f.session};
        dialog.reject();
        QVERIFY(!dialog.chosen_protocol_name().has_value());
    }
};

QTEST_MAIN(ProtocolSelectTest)
#include "protocol_select_test.moc"
```

If `ui_protocol_select.h`'s generated name differs, use the name `protocol_select.cpp` includes. Check column 0 is the protocol name in `protocol_select.cpp`'s tree population and adjust `findItems` to match.

`src/ui/desktop/settings_test.cpp`: replace the file's single test with the three below. Add these includes: `<QApplication>`, `<QDir>`, `<QMessageBox>`, `<QTimer>`, `src/backend/config/config_session.h`. The helpers go in an anonymous namespace:

```cpp
// Accepts every message box that appears while it lives, recording its text.
class ModalCollector
{
  public:
    ModalCollector()
    {
        QObject::connect(&timer_, &QTimer::timeout, [this]
                         {
                             if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
                             {
                                 texts_.append(box->text());
                                 box->accept();
                             }
                         });
        timer_.start(10);
    }
    const QStringList& texts() const
    {
        return texts_;
    }

  private:
    QTimer timer_;
    QStringList texts_;
};
```

```cpp
struct SessionOnDisk
{
    explicit SessionOnDisk(const QString& root)
    {
        status = session.initialize(root.toStdString(), "0.1.0-beta.5");
    }
    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository file_repository;
    QtEventSink events;
    fastecu::config::ConfigSession session{file_system, resource_bundle, file_repository, events};
    fastecu::Status status;
};

    void closingSettingsSavesThroughTheSession()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        const QString config_file = QString::fromStdString(disk.session.provisioned_paths().config_file);
        QVERIFY(QFile::remove(config_file));

        {
            Settings settings{disk.session};
        }

        QVERIFY(QFile::exists(config_file));
    }

    void editsReachTheSessionLive()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        Settings settings{disk.session};

        QVERIFY(QMetaObject::invokeMethod(&settings, "toolbar_iconsize_value_changed", Qt::DirectConnection,
                                          Q_ARG(int, 40)));

        QCOMPARE(disk.session.settings().toolbar_iconsize, std::string("40"));
    }

    void failedSaveKeepsEditsAndWarnsTheOperator()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        SessionOnDisk disk{root.path()};
        QVERIFY(disk.status.has_value());
        const QString config_file = QString::fromStdString(disk.session.provisioned_paths().config_file);
        QVERIFY(QFile::remove(config_file));
        QVERIFY(QDir().mkpath(config_file)); // a directory where the file goes: every write fails

        ModalCollector boxes;
        {
            Settings settings{disk.session};
            QVERIFY(QMetaObject::invokeMethod(&settings, "toolbar_iconsize_value_changed", Qt::DirectConnection,
                                              Q_ARG(int, 40)));
            settings.close();
        }

        QCOMPARE(disk.session.settings().toolbar_iconsize, std::string("40"));
        QVERIFY(!boxes.texts().isEmpty());
        QVERIFY(boxes.texts().front().contains(config_file));
    }
```

- [ ] **Step 3: `MainWindow` tests**

In `src/ui/desktop/mainwindow_test.cpp`:

1. Add `const ApplicationIdentity kTestApplication{.name = "FastECU", .title = "FastECU", .version = "0.1.0-beta.5"};` and replace every `FileActions::ConfigValuesStructure{}.software_version` with `QString::fromStdString(kTestApplication.version)`.
2. Rewrite `TestServices`:

```cpp
struct TestServices
{
    explicit TestServices(const QString& config_root)
        : config_status(config.initialize(config_root.toStdString(), kTestApplication.version)),
          file_actions(file_system, resource_bundle, file_repository, file_writer, events, config)
    {
    }

    MainWindowServices services()
    {
        return {
            .application = kTestApplication,
            .config = config,
            .file_actions = file_actions,
            .config_repository = file_repository,
            .file_action_events = events,
            .log = log_channel,
            .connection = adapter.connection(),
            .remote = remote_peer,
            .logging_engine = logging_engine,
        };
    }

    QtFileSystem file_system;
    QtResourceBundle resource_bundle;
    QtFileRepository file_repository;
    QtAtomicFileWriter file_writer;
    QtEventSink events;
    QtEventSink config_events;
    fastecu::config::ConfigSession config{file_system, resource_bundle, file_repository, config_events};
    fastecu::Status config_status; // declared after `config`: initialized from it
    FileActions file_actions;
    fastecu::ui::LogChannel log_channel;
    fastecu::desktop::connection::testing::AdapterConnectionHarness adapter;
    FakeBackend *fake = adapter.fake();
    fastecu::ui::RemotePeer remote_peer;
    fastecu::desktop::logging::LoggingEngine logging_engine;
};
```

  Every test that builds `TestServices` adds `QVERIFY(services.config_status.has_value());` right after it.

3. `initTestCase`'s `protocols.cfg`:
   - Keep the four `<protocol>` entries.
   - Add three more by copying `sub_ecu_denso_sh7058_can` with only `name` changed: `sub_ecu_denso_sh7058_can_future`, `sub_ecu_denso_sh7058_densocan_extra`, `sub_ecu_not_a_real_protocol`.
   - Add a fourth copy named `sub_ecu_denso_sh7058_can_checksum_na` with `<checksum>n/a</checksum>`.
   - `<car_models>` becomes exactly these rows, in this order (make/model → protocol). The tests below depend on the order.
     - 0 `Subaru`/`Test` → `sub_tcu_denso_sh7058_can` (unchanged first row; `explicitConfigRootLoadsFixtureAndProvisionsDirectories` reads its model)
     - 1 `Subaru`/`Can` → `sub_ecu_denso_sh7058_can`
     - 2 `Subaru`/`DensoCan` → `sub_ecu_denso_sh7058_densocan`
     - 3 `Subaru`/`KLine` → `sub_ecu_denso_sh7058`
     - 4 `Subaru`/`Future` → `sub_ecu_denso_sh7058_can_future`
     - 5 `Subaru`/`Extra` → `sub_ecu_denso_sh7058_densocan_extra`
     - 6 `Subaru`/`Unsupported` → `sub_ecu_not_a_real_protocol`
     - 7 `Subaru`/`ChecksumNa` → `sub_ecu_denso_sh7058_can_checksum_na`
     - 8 `Mitsubishi`/`Colt` → `sub_ecu_denso_sh7058`
     - 9 `Nissan`/`Test` → `sub_ecu_denso_sh7058`
     - 10 `Subaru`/`Orphan` → `sub_ecu_orphan` (no `<protocol>` of that name: unresolved)
   - Write the kernels to the provisioned kernel directory (`config_root_.path() + "/0.1.0-beta.5/kernels/"`) instead of `config_root_.path() + "/kernels/"`. Write both `test-kernel.bin` and `tcu_kernel.bin` there.
4. Add helpers next to `prepareConnect`:

```cpp
void selectProtocol(MainWindow& window, const QString& protocol)
{
    QVERIFY(window.configSession->select_by_protocol_name(protocol.toStdString()));
}

void selectMake(MainWindow& window, const QString& make)
{
    const auto vehicles = window.configSession->vehicles();
    const auto it = std::ranges::find(vehicles, make.toStdString(), &fastecu::config::ResolvedCarModel::make);
    QVERIFY(it != vehicles.end());
    QVERIFY(window.configSession->select_row(static_cast<std::size_t>(it - vehicles.begin())).has_value());
}
```

   `prepareConnect` becomes `selectMake(window, make);` followed by `window.configSession->settings().selected_log_transport = transport.toStdString();` and `window.configSession->settings().selected_log_protocol = "SSM";`.
5. Replace each block of `window.configValues->flash_protocol_selected_{make,protocol_name,mcu,checksum,id}` and `flash_protocol_kernel*` assignments with one `selectProtocol(window, <protocol>)`, or `selectMake(window, "Nissan")` for `otherMakesSkipDispatchButStillRunCleanup`. Delete every `kernel_files_directory` assignment. `cancellingTheChecksumWarningStopsVoltagePolling` selects `sub_ecu_denso_sh7058_can_checksum_na`. `readOfAnUnsupportedProtocolReleasesTheReadSlot` selects `sub_ecu_not_a_real_protocol`. The `_data()` rows keep their `mcu`/`kernel_address` columns only if the test body still asserts them; otherwise delete those columns.
6. `explicitConfigRootLoadsFixtureAndProvisionsDirectories`: replace `window.configValues->...` with `window.configSession->provisioned_paths()` fields (`std::string`, compare with `.toStdString()` of the expected `QString`) and `window.configSession->vehicles().front().model == "Test"`.
7. `biuWindowRemembersTheOpenedPort`: `window.configSession->settings().serial_port = "none"; window.save_settings();` and compare `window.configSession->settings().serial_port` with `std::string("ttyUSB0")`.
8. `loggingCapturesTargetForEachRun`: `window.configSession->settings().selected_log_protocol = "SSM";`.
9. New cases:

```cpp
    void acceptedVehicleChoiceSelectsTheRowAndSavesIt()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        const std::string flash_transport = services.config.settings().selected_flash_transport;
        const std::string log_transport = services.config.settings().selected_log_transport;

        window.apply_vehicle_choice(QDialog::Accepted, 1);

        QCOMPARE(*services.config.selected_row(), std::size_t{1});
        QCOMPARE(services.config.settings().selected_log_protocol, std::string("SSM"));
        QCOMPARE(services.config.settings().selected_flash_transport, flash_transport);
        QCOMPARE(services.config.settings().selected_log_transport, log_transport);

        // Saved: a fresh session over the same root restores row 1.
        QtEventSink reread_events;
        fastecu::config::ConfigSession reread{services.file_system, services.resource_bundle,
                                              services.file_repository, reread_events};
        QVERIFY(reread.initialize(config_root_.path().toStdString(), kTestApplication.version).has_value());
        QCOMPARE(reread.settings().selected_protocol_id, std::string("1"));
    }

    void cancelledVehicleChoiceChangesNothing()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        const auto before = services.config.settings();

        window.apply_vehicle_choice(QDialog::Rejected, 1);

        QVERIFY(services.config.settings() == before);
    }

    void acceptedProtocolChoiceSelectsTheLastMatchingRow()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        const auto vehicles = services.config.vehicles();
        std::size_t last = 0;
        for (std::size_t i = 0; i < vehicles.size(); ++i)
        {
            if (vehicles[i].protocol_name == "sub_ecu_denso_sh7058")
            {
                last = i;
            }
        }

        window.apply_protocol_choice(QDialog::Accepted, std::string("sub_ecu_denso_sh7058"));

        QCOMPARE(*services.config.selected_row(), last);
    }

    void romFlashMethodSelectsTheLastMatchingRow()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        auto calibration = std::make_unique<FileActions::EcuCalDefStructure>();
        calibration->RomInfo.resize(FileActions::DefFile + 1);
        calibration->RomInfo[FileActions::FlashMethod] = "sub_ecu_denso_sh7058";
        window.ecuCalDef[0] = calibration.get();

        window.update_protocol_info(0);
        window.ecuCalDef[0] = nullptr;

        QCOMPARE(*services.config.selected_row(), std::size_t{9}); // rows 3, 8, 9 match; the last wins
        QCOMPARE(services.config.selected_vehicle()->make, std::string("Nissan"));
    }

    void unmatchedRomFlashMethodChangesNothing()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.config_status.has_value());
        MainWindow window{services.services()};
        constructor_driver.stop();
        const auto before = services.config.settings();
        auto calibration = std::make_unique<FileActions::EcuCalDefStructure>();
        calibration->RomInfo.resize(FileActions::DefFile + 1);
        calibration->RomInfo[FileActions::FlashMethod] = "no_such_protocol";
        window.ecuCalDef[0] = calibration.get();

        window.update_protocol_info(0);
        window.ecuCalDef[0] = nullptr;

        QVERIFY(services.config.settings() == before);
    }

    void loggingUsesTheSessionLogProtocol()
    {
        // Copy loggingCapturesTargetForEachRun's setup (injected factory,
        // prepareConnect, identification), then set the protocol to CDBG
        // before starting logging.
        // ...setup copied verbatim from loggingCapturesTargetForEachRun...
        window.configSession->settings().selected_log_protocol = "CDBG";
        QVERIFY(QMetaObject::invokeMethod(&window, "continue_start_logging", Qt::DirectConnection));
        QCOMPARE(window.activeLogValueProtocolFilter, QString("CDBG"));
    }
```

  `loggingUsesTheSessionLogProtocol`'s first lines are the verbatim setup of `loggingCapturesTargetForEachRun` up to where that test starts logging. Copy them in; do not factor them out.

  `unresolvedProtocolRowLeavesReadAndWriteUnavailable` uses fixture row 10:
  - The test calls `selectProtocol(window, "sub_ecu_orphan")`, then `window.set_flash_arrow_state()`.
  - It asserts that the read, test-write, and write menu actions are disabled. Look them up with the same `findChildren<QMenu *>()` walk `set_flash_arrow_state` uses (lines 752–800 of `mainwindow.cpp`), matching the same action texts.

- [ ] **Step 4: Run the new and changed tests and confirm they fail**

Run: `bazel test --config=release //apps/desktop:all //src/ui/desktop:all //src/backend/definitions:all //src/backend/calibration/legacy:all`
Expected: build FAIL (`ConfigSession` not in `MainWindowServices`, `VehicleSelect` has no session constructor, `startup_diagnostics.h` missing, …).

#### 5b. Composition and startup

- [ ] **Step 5: Default root, startup sink, diagnostics**

`apps/desktop/default_config_root.h`:

```cpp
#pragma once
#include <QString>

// The platform's FastECU configuration root, with its trailing slash, as the
// legacy ConfigValuesStructure spelled it. One source file per platform.
QString default_config_root();
```

`apps/desktop/default_config_root_unix.cpp`:

```cpp
#include "apps/desktop/default_config_root.h"

#include <QDir>

QString default_config_root()
{
    return QDir::homePath() + "/.config/FastECU/";
}
```

`apps/desktop/default_config_root_windows.cpp`: same file, with `"/AppData/Local/FastECU/"`.

`apps/desktop/startup_event_sink.h`:

```cpp
#pragma once
#include <QString>
#include <QStringList>

#include "src/backend/ports/event_sink.h"

// Collects what the configuration session reports while nothing that could
// display it (window, syslog thread) exists yet. Warnings, errors, and
// notices are kept for the startup presenter; debug/info lines are dropped.
class StartupEventSink : public fastecu::IEventSink
{
  public:
    void log(fastecu::LogLevel level, std::string_view message) override
    {
        if (level == fastecu::LogLevel::Warning || level == fastecu::LogLevel::Error)
        {
            warnings_.append(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
        }
    }
    void progress(int, int) override
    {
    }
    void notice(std::string_view message) override
    {
        warnings_.append(QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
    }
    const QStringList& warnings() const
    {
        return warnings_;
    }

  private:
    QStringList warnings_;
};
```

`apps/desktop/startup_diagnostics.h`:

```cpp
#pragma once
#include <QString>
#include <QStringList>

#include "src/backend/ports/error.h"

// Startup has no window yet, so configuration failures and warnings are shown
// in standalone message boxes. The *_text functions are the testable part.
QString startup_failure_text(const fastecu::Error& error);
QString startup_warning_text(const QStringList& warnings);
void present_startup_failure(const fastecu::Error& error);
void present_startup_warnings(const QStringList& warnings); // no-op when empty
```

`apps/desktop/startup_diagnostics.cpp`:

```cpp
#include "apps/desktop/startup_diagnostics.h"

#include <QMessageBox>

QString startup_failure_text(const fastecu::Error& error)
{
    return QStringLiteral("FastECU could not load its configuration and will exit.\n\n%1")
        .arg(QString::fromStdString(error.detail));
}

QString startup_warning_text(const QStringList& warnings)
{
    return QStringLiteral("FastECU started with configuration warnings:\n\n%1").arg(warnings.join("\n"));
}

void present_startup_failure(const fastecu::Error& error)
{
    qCritical().noquote() << startup_failure_text(error);
    QMessageBox::critical(nullptr, QStringLiteral("FastECU"), startup_failure_text(error));
}

void present_startup_warnings(const QStringList& warnings)
{
    if (warnings.isEmpty())
    {
        return;
    }
    qWarning().noquote() << startup_warning_text(warnings);
    QMessageBox::warning(nullptr, QStringLiteral("FastECU"), startup_warning_text(warnings));
}
```

- [ ] **Step 6: Composition owns and initializes the session**

`src/ui/desktop/main_window_services.h`: add `#include <string>` and a forward declaration of `namespace fastecu::config { class ConfigSession; }`. Then:

```cpp
// Composition-supplied application metadata.
struct ApplicationIdentity
{
    std::string name;
    std::string title;
    std::string version;
};

struct MainWindowServices
{
    const ApplicationIdentity& application;
    fastecu::config::ConfigSession& config; // initialized before MainWindow is built
    FileActions& file_actions;
    QtFileRepository& config_repository;
    QtEventSink& file_action_events;
    fastecu::ui::LogChannel& log;
    fastecu::desktop::connection::AdapterConnection& connection;
    fastecu::ui::RemotePeer& remote;
    fastecu::desktop::logging::LoggingEngine& logging_engine;
};
```

`apps/desktop/desktop_composition.h`:
- Include `<optional>`, `<QStringList>`, `apps/desktop/startup_event_sink.h`, `src/backend/config/config_session.h`, and `src/backend/ports/error.h`.
- Add the public members `bool started() const;`, `const std::optional<fastecu::Error>& startup_error() const;`, and `QStringList startup_warnings() const;`.
- Document `services()` as "requires started()".
- Members, in this order: `file_system_`, `resource_bundle_`, `file_repository_`, `file_writer_`, `file_action_events_`, `startup_events_` (`StartupEventSink`), `config_` (`fastecu::config::ConfigSession`), `startup_error_` (`std::optional<fastecu::Error>`), `file_actions_` (`std::unique_ptr<FileActions>`), then the existing channels, thread, logger, serial, connection, remote, clock, and engine.

`apps/desktop/desktop_composition.cpp`:

```cpp
namespace
{
const ApplicationIdentity kApplication{.name = "FastECU", .title = "FastECU", .version = "0.1.0-beta.5"};
} // namespace

DesktopComposition::DesktopComposition(const QString& peer_address, const QString& peer_password,
                                       const QString& config_root)
    : config_(file_system_, resource_bundle_, file_repository_, startup_events_)
{
    const QString root = config_root.isEmpty() ? default_config_root() : config_root;
    if (fastecu::Status initialized = config_.initialize(root.toStdString(), kApplication.version);
        !initialized.has_value())
    {
        // Required configuration is missing or broken: build nothing that
        // could log, spawn a thread, or reach an ECU. main() presents it.
        startup_error_ = initialized.error();
        return;
    }

    file_actions_ = std::make_unique<FileActions>(file_system_, resource_bundle_, file_repository_, file_writer_,
                                                  file_action_events_, config_);

    syslog_thread_ = std::make_unique<QThread>();
    syslogger_ = std::make_unique<SystemLogger>(
        QString::fromStdString(config_.effective_paths().syslog_files_directory),
        QString::fromStdString(kApplication.name), QString::fromStdString(kApplication.version));
    // ... the remainder of the existing constructor, unchanged ...
}

DesktopComposition::~DesktopComposition()
{
    // Dependents first. After a failed start none of these exist.
    logging_engine_.reset();
    remote_utility_.reset();
    connection_.reset();
    serial_.reset();
    if (syslog_thread_)
    {
        syslog_thread_->quit();
        syslog_thread_->wait();
    }
    syslogger_.reset();
    syslog_thread_.reset();
    file_actions_.reset(); // before config_, which it references
}

bool DesktopComposition::started() const
{
    return !startup_error_.has_value();
}

const std::optional<fastecu::Error>& DesktopComposition::startup_error() const
{
    return startup_error_;
}

QStringList DesktopComposition::startup_warnings() const
{
    return startup_events_.warnings();
}

MainWindowServices DesktopComposition::services()
{
    return {
        .application = kApplication,
        .config = config_,
        .file_actions = *file_actions_,
        .config_repository = file_repository_,
        .file_action_events = file_action_events_,
        .log = log_channel_,
        .connection = *connection_,
        .remote = remote_peer_,
        .logging_engine = *logging_engine_,
    };
}
```

Keep the existing comment explaining why the syslog thread is stopped and joined. Delete the old `file_actions_(...)` initializer and the `set_base_dirs` call.

`apps/desktop/main.cpp`, replacing the two lines that build the composition and window:

```cpp
        DesktopComposition composition{addr, password};
        if (!composition.started())
        {
            present_startup_failure(*composition.startup_error());
            return_code = EXIT_FAILURE;
            break;
        }
        present_startup_warnings(composition.startup_warnings());
        MainWindow w(composition.services(), addr);
```

Add `#include <cstdlib>` and `#include "apps/desktop/startup_diagnostics.h"`.

`apps/desktop/BUILD.bazel`:

```python
alias(
    name = "default_config_root_source",
    actual = select({
        "@platforms//os:windows": ":default_config_root_windows.cpp",
        "//conditions:default": ":default_config_root_unix.cpp",
    }),
    visibility = ["//visibility:private"],
)

qt_cc_library(
    name = "startup",
    srcs = [
        "startup_diagnostics.cpp",
        ":default_config_root_source",
    ],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = [
        "default_config_root.h",
        "startup_diagnostics.h",
        "startup_event_sink.h",
    ],
    visibility = ["//visibility:private"],
    deps = QT_DEPS + ["//src/backend/ports"],
)

fastecu_qttest(
    name = "startup_diagnostics_test",
    size = "small",
    src = "startup_diagnostics_test.cpp",
    deps = [":startup"],
)
```

Add `":startup"` and `"//src/backend/config:config_session"` to `:composition`'s deps, and `":startup"` to `:fastecu`'s deps. If `srcs` does not accept an `alias` to a source file, use `select()` directly in `srcs`: `srcs = ["startup_diagnostics.cpp"] + select({...: ["default_config_root_windows.cpp"], ...})`.

#### 5c. Backend consumers

- [ ] **Step 7: `LegacyCalibrationAdapter`**

- In `legacy_calibration_adapter.h`, `open_rom_bytes` takes `std::string_view calibration_files_directory` instead of `const definitions::ConfigValuesStructure&`, and its backup handle is `std::string(calibration_files_directory) + "read.bin"`.
- Delete `bind_protocol`, `resolved_car_models`, `resolved_car_models_cache_`, `resolved_car_models_handle_`, their comments, and the includes that only they used: `car_model_catalog.h`, `config_paths.h`, `config_values.h`, `legacy_config_paths.h`, `protocol_catalog.h`.
- Drop the matching BUILD deps: `//src/backend/config:car_model_catalog`, `:config_paths`, `:protocol_catalog`, `//src/backend/config/legacy:legacy_config_paths`, `//src/backend/definitions:config_values`.
- In `legacy_calibration_adapter_test.cpp`, delete every `bind_protocol` test; their behavior now lives in `ConfigSessionSelect.*` (Task 3). Change the `open_rom_bytes` tests to pass a directory string where they built a `ConfigValuesStructure`.

- [ ] **Step 8: `FileActions`**

`file_actions.h`:
- Add the constructor parameter `fastecu::config::ConfigSession& config` (last).
- Add the private member `fastecu::config::ConfigSession& configSession_;`, declared **first** among the private data members so it is initialized before anything that uses it.
- Add the private helper `const fastecu::config::ResolvedCarModel& selectedVehicle() const;`.
- Delete `ConfigValuesStructure`/`ConfigValuesStruct`, `set_base_dirs`, `check_config_dirs`, `read_config_file`, `save_config_file`, `read_protocols_file`, and `configAdapter_`, plus the includes of `legacy_config_adapter.h` and `config_values.h`.
- Keep `static bool validate_flash_protocols(const fastecu::definitions::ConfigValuesStructure&, QStringList*)` and the `config_values.h` include exactly as they are. Nothing calls it after this task, and Task 6 deletes it, its tests, and the header together.

`file_actions.cpp`:

```cpp
const fastecu::config::ResolvedCarModel& FileActions::selectedVehicle() const
{
    // The composition initializes the session before building FileActions,
    // and the session only ever holds a valid row.
    return *configSession_.selected_vehicle();
}
```

Replace code using this table (applies to all three `file_actions*.cpp`):

| Old | New |
|---|---|
| `ConfigValuesStruct.romraider_definition_files` (QStringList) | `configSession_.settings().romraider_definition_files` (`std::vector<std::string>`; drop the `toStdString` loop in `build_definition_catalog` and pass it directly) |
| `ConfigValuesStruct.ecuflash_definition_files_directory` | `configSession_.settings().ecuflash_definition_files_directory` |
| `use_romraider_definitions` / `use_ecuflash_definitions` / `primary_definition_base` | `configSession_.settings().<same>` (compare against string literals) |
| `romraider_logger_definition_file` (read and write) | `configSession_.settings().romraider_logger_definition_file` |
| `flash_protocol_selected_log_protocol` | `configSession_.settings().selected_log_protocol` |
| `logger_file`, `config_files_directory` | `configSession_.effective_paths().<same>` |
| `flash_protocol_selected_make` | `selectedVehicle().make` |
| `flash_protocol_selected_checksum` / `_mcu` | `fastecu::config::protocol_field_or_placeholder(selectedVehicle(), &ProtocolEntry::checksum / ::mcu)` |
| `calibrationAdapter_.open_rom_bytes(*ecuCalDef, filename, *configValues)` | `calibrationAdapter_.open_rom_bytes(*ecuCalDef, filename, configSession_.effective_paths().calibration_files_directory)` |
| `calibrationAdapter_.bind_protocol(*configValues, flash_method)` | `configSession_.select_by_protocol_name(flash_method.toStdString());` (the return value is not used: no match changes nothing, as before) |

Rewrite `apply_flash_method_alias` over vehicles:

```cpp
    const auto vehicles = configSession_.vehicles();
    for (const fastecu::config::ResolvedCarModel& vehicle : vehicles)
    {
        const QStringList aliases =
            QString::fromStdString(fastecu::config::protocol_field_or_placeholder(vehicle, &ProtocolEntry::alias))
                .split(",");
        // ... existing body, with flash_protocol_protocol_name.at(index) -> vehicle.protocol_name ...
    }
```

Delete the five config wrapper method definitions and `read_protocols_file`'s validation block. In `BUILD.bazel`, `definitions` swaps `//src/backend/config/legacy:legacy_config_adapter` for `//src/backend/config:config_session` and keeps `:config_values` until Task 6.

`file_actions_parsing_test.cpp`:
- Give the fixture a `SessionOnDisk`-style member initialized in each test's `QTemporaryDir`. Use the same three-port pattern as `settings_test.cpp`, calling `session.initialize(dir.path().toStdString(), "test")`.
- Construct `FileActions` with it.
- `logger_file` tests write their XML to `QString::fromStdString(session.effective_paths().logger_file)` (after initialization) instead of assigning `logger_file`.
- `romraider_logger_definition_file`, `romraider_definition_files`, and `ecuflash_definition_files_directory` assignments go to `session.settings()`.
- Delete `application_config_reads_valid_values_and_preserves_defaults` and the second application-config case (the one at line ~136). Their assertions are covered by `ConfigSessionInitialize.LoadedScalarsOverrideDefaults` / `AbsentSettingsTakeCompiledInDefaults` (Task 2). Before deleting, compare each assertion and add any setting they check that Task 2 does not (e.g. `window_size` width/height parsing) to `LoadedScalarsOverrideDefaults`.

#### 5d. UI consumers

- [ ] **Step 9: `config_fields`**

`src/ui/desktop/config_fields.h`:

```cpp
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include <QString>
#include <QStringList>

#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/protocol_catalog.h"

namespace fastecu::ui
{

inline QString qs(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// A protocol field for display; the legacy single-space placeholder when the
// vehicle's protocol reference did not resolve.
QString protocol_field(const config::ResolvedCarModel& vehicle, std::string config::ProtocolEntry::*field);

// True only for a resolved protocol whose capability field reads "yes".
bool protocol_capability(const config::ResolvedCarModel& vehicle, std::string config::ProtocolEntry::*capability);

QStringList qstring_list(const std::vector<std::string>& items);
std::vector<std::string> string_vector(const QStringList& items);

} // namespace fastecu::ui
```

`src/ui/desktop/config_fields.cpp`:

```cpp
#include "src/ui/desktop/config_fields.h"

#include "src/backend/config/config_session.h"

namespace fastecu::ui
{

QString protocol_field(const config::ResolvedCarModel& vehicle, std::string config::ProtocolEntry::*field)
{
    return qs(config::protocol_field_or_placeholder(vehicle, field));
}

bool protocol_capability(const config::ResolvedCarModel& vehicle, std::string config::ProtocolEntry::*capability)
{
    return vehicle.protocol.has_value() && (*vehicle.protocol).*capability == "yes";
}

QStringList qstring_list(const std::vector<std::string>& items)
{
    QStringList out;
    out.reserve(static_cast<qsizetype>(items.size()));
    for (const std::string& item : items)
    {
        out.append(qs(item));
    }
    return out;
}

std::vector<std::string> string_vector(const QStringList& items)
{
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(items.size()));
    for (const QString& item : items)
    {
        out.push_back(item.toStdString());
    }
    return out;
}

} // namespace fastecu::ui
```

In `src/ui/desktop/BUILD.bazel`:
- Add a `qt_cc_library(name = "config_fields", srcs = ["config_fields.cpp"], hdrs = [], normal_hdrs = ["config_fields.h"], copts = COMMON_COPTS, deps = QT_DEPS + ["//src/backend/config:car_model_catalog", "//src/backend/config:config_session", "//src/backend/config:protocol_catalog"])`.
- Add a `fastecu_gtest` target, `config_fields_test`, with deps `[":config_fields", "//src/backend/config/testing:config_session_fixture"]`.
- Add `fastecu_qttest` targets `test_vehicle_select` and `test_protocol_select`, shaped like `test_settings` (offscreen, `linkstatic = True`), with deps `[":desktop", "//src/backend/config/testing:config_session_fixture"]`.
- Give `test_settings` and `test_mainwindow` the extra dep `//src/backend/config:config_session`.
- In `:desktop`, replace `//src/backend/config/legacy:legacy_config_paths` with `:config_fields` and `//src/backend/config:config_session`.

- [ ] **Step 10: `VehicleSelect` and `ProtocolSelect` hold tentative choices**

`vehicle_select.h`:
- The constructor becomes `explicit VehicleSelect(const fastecu::config::ConfigSession& config, QWidget *parent = nullptr);`.
- Add `std::optional<std::size_t> chosen_row() const;`.
- Replace `FileActions::ConfigValuesStructure *configValues;` with `const fastecu::config::ConfigSession& config;` and `std::optional<std::size_t> chosenRow;`.
- Replace the `file_actions.h` include with `src/backend/config/config_session.h`.

`vehicle_select.cpp`:
- Iterate `config.vehicles()` wherever the code looped `flash_protocol_id.length()`, using the loop index `i` for `flash_protocol_id.at(i)` (as `QString::number(i)`).
- Read row fields through `fastecu::ui::qs(vehicle.make)` etc., and protocol fields through `fastecu::ui::protocol_field(vehicle, &ProtocolEntry::mcu)` etc.
- Get the initial index from `config.selected_row()` (value or 0), and the initial make/model from `config.selected_vehicle()`.
- `car_model_selected()` becomes:

```cpp
void VehicleSelect::car_model_selected()
{
    // Tentative: the caller applies an accepted choice to the session.
    chosenRow = static_cast<std::size_t>(flash_protocol_id.toULongLong());
    accept();
    close();
}

std::optional<std::size_t> VehicleSelect::chosen_row() const
{
    return chosenRow;
}
```

Make the same changes in `ProtocolSelect`: `const fastecu::config::ConfigSession& config;` and `std::optional<std::string> chosenProtocolName;`. Protocol-list population iterates `config.vehicles()`, one entry per distinct `protocol_name` (shared and unresolved ones included, as today), with the description from `fastecu::ui::protocol_field(vehicle, &ProtocolEntry::description)`. The current-item match uses `config.selected_vehicle()->protocol_name`. `car_model_selected()` stores the selected item's protocol name into `chosenProtocolName` and calls `accept(); close();`, with no other writes.

- [ ] **Step 11: `Settings` edits live and reports save failures**

- `settings.h`: the constructor becomes `explicit Settings(fastecu::config::ConfigSession& config, QWidget *parent = nullptr);`. The member is `fastecu::config::ConfigSession& config;`. `create_files_config_page()` and `create_ui_config_page()` take no parameter. Include `config_session.h` instead of `file_actions.h`.
- `settings.cpp`: every `configValues->X = qstr;` becomes `config.settings().X = qstr.toStdString();`. Every read becomes `fastecu::ui::qs(config.settings().X)`. `romraider_definition_files` edits use `std::vector<std::string>` (`push_back`, `clear`). `log_files_dir_lineedit` shows `config.settings().datalog_files_directory`.
- `save_config_file()`:

```cpp
int Settings::save_config_file()
{
    const fastecu::Status saved = config.save();
    if (!saved.has_value())
    {
        // The edits stay in the session; the operator learns why they did
        // not reach the file.
        QMessageBox::warning(this, tr("Settings"),
                             tr("Unable to save settings.\n\n%1").arg(QString::fromStdString(saved.error().detail)));
        return 1;
    }
    return 0;
}
```

The destructor and `closeEvent` keep calling `save_config_file()`. Do not add accept/cancel semantics.

- [ ] **Step 12: `DefinitionAuthoringDialog`**

- The constructor gains `const fastecu::config::ConfigSession& config` after `FileActions&`, stored as `const fastecu::config::ConfigSession& config_;`.
- Replace `configValues->ecuflash_definition_files_directory` with `fastecu::ui::qs(config_.settings().ecuflash_definition_files_directory)`, and delete the `configValues` locals.
- Update the dialog's test to construct a `ConfigSessionFixture` and pass `f.session`.
- `MainWindow` passes `*configSession`.
- BUILD: add `//src/backend/config:config_session` and `//src/ui/desktop:config_fields`. If the visibility of `config_fields` is too narrow, give it `visibility = ["//src/ui/desktop:__subpackages__"]`.

- [ ] **Step 13: `MainWindow`, `menu_actions.cpp`, `log_operations_ssm.cpp`**

In `mainwindow.h`:
- Replace `FileActions::ConfigValuesStructure *configValues;` with `fastecu::config::ConfigSession *configSession = nullptr;`.
- Declare `const fastecu::config::ResolvedCarModel& selected_vehicle() const;`, `void save_settings();`, `void apply_vehicle_choice(int result, std::optional<std::size_t> row);`, and `void apply_protocol_choice(int result, std::optional<std::string> protocol_name);` in the private section.

In `mainwindow.cpp`, add `using fastecu::ui::qs; using fastecu::config::ProtocolEntry;` and:

```cpp
const fastecu::config::ResolvedCarModel& MainWindow::selected_vehicle() const
{
    // DesktopComposition initializes the session before building MainWindow,
    // and the session only ever holds a valid row.
    return *configSession->selected_vehicle();
}

void MainWindow::save_settings()
{
    if (const fastecu::Status saved = configSession->save(); !saved.has_value())
    {
        emit LOG_E(qs(saved.error().detail), true, true);
    }
}

void MainWindow::apply_vehicle_choice(int result, std::optional<std::size_t> row)
{
    if (result == QDialog::Accepted && row.has_value())
    {
        if (const fastecu::Status selected = configSession->select_row(*row); !selected.has_value())
        {
            emit LOG_E(qs(selected.error().detail), true, true);
        }
    }
    select_vehicle_finished(result);
}

void MainWindow::apply_protocol_choice(int result, std::optional<std::string> protocol_name)
{
    if (result == QDialog::Accepted && protocol_name.has_value())
    {
        configSession->select_by_protocol_name(*protocol_name);
    }
    select_protocol_finished(result);
}

void MainWindow::select_vehicle()
{
    VehicleSelect vehicleSelect(*configSession);
    const int result = vehicleSelect.exec();
    apply_vehicle_choice(result, vehicleSelect.chosen_row());
    emit LOG_D("Selected protocol: " + qs(configSession->settings().selected_protocol_id), true, true);
}

void MainWindow::select_protocol()
{
    ProtocolSelect protocolSelect(*configSession);
    const int result = protocolSelect.exec();
    apply_protocol_choice(result, protocolSelect.chosen_protocol_name());
    emit LOG_D("Selected protocol: " + qs(configSession->settings().selected_protocol_id), true, true);
}
```

The old `connect(&dialog, SIGNAL(finished(int)), ...)` lines go. The `*_finished` slots now run after the choice is applied, and their `fileActions->save_config_file(configValues)` becomes `save_settings()`.

Constructor:
- `configSession = &services_.config;`.
- `software_name`/`software_title`/`software_version` come from `qs(services_.application.<name|title|version>)`.
- Delete `check_config_dirs`, `read_config_file`, `read_protocols_file`, and the out-of-range fix-up (the session already validated the row).
- The `LOG_D` block logs `qs(selected_vehicle().make)`, `.model`, `.version`, `.protocol_name`, `protocol_field(selected_vehicle(), &ProtocolEntry::mcu / ::checksum / ::description)`, and the three transport/protocol settings.
- The menu loader uses `configSession->effective_paths()` instead of `paths_from_config_values(*configValues)`.
- The calibration-files loop removes missing entries from `configSession->settings().calibration_files`. Keep its index-while-removing behavior by translating `removeAt(i)` to `erase(begin() + i)` with the same `i` adjustment the loop has today.

Translation table for everything else in `mainwindow.cpp` and `menu_actions.cpp`:

| Old | New |
|---|---|
| `configValues->flash_protocol_selected_id` | `qs(configSession->settings().selected_protocol_id)` |
| `…flash_protocol_selected_id.toInt()` used as an index | `*configSession->selected_row()` |
| `flash_protocol_selected_{make,model,version,protocol_name}` | `qs(selected_vehicle().{make,model,version,protocol_name})` |
| `flash_protocol_selected_{description,mcu,checksum}` | `fastecu::ui::protocol_field(selected_vehicle(), &ProtocolEntry::{description,mcu,checksum})` |
| `flash_protocol_<field>.at(<selected id>)` | `fastecu::ui::protocol_field(selected_vehicle(), &ProtocolEntry::<field>)` |
| `flash_protocol_{read,test_write,write}.at(<selected id>) == "yes"` | `fastecu::ui::protocol_capability(selected_vehicle(), &ProtocolEntry::{read,test_write,write})` |
| `flash_protocol_selected_{flash_transport,log_transport,log_protocol}` (read / write) | `configSession->settings().selected_{flash_transport,log_transport,log_protocol}` (`qs(...)` / `= x.toStdString()`) |
| `serial_port`, `window_width`, `window_height`, `toolbar_iconsize` | `configSession->settings().<same>` |
| `calibration_files`, `romraider_definition_files` (QStringList ops) | `configSession->settings().<same>` (`std::vector<std::string>`: `push_back`, `erase(begin() + row)`, `empty()`, `size()`) |
| `ecuflash_definition_files_directory` | `configSession->settings().ecuflash_definition_files_directory` |
| `kernel_files_directory`, `definition_files_directory`, `menu_file` | `qs(configSession->effective_paths().<same>)` |
| `calibration_files_directory`, `datalog_files_directory` (as a path to use) | `qs(configSession->effective_paths().<same>)` |
| `fileActions->save_config_file(configValues)` | `save_settings()` |
| `fastecu::config::paths_from_config_values(*configValues)` | `configSession->effective_paths()` |
| `FileActions::ConfigValuesStructure *configValues = &fileActions->ConfigValuesStruct;` (local) | delete; use the member |

Specific rewrites:
- `update_protocol_info`: replace the scan loop with

```cpp
    const std::string flash_method = ecuCalDef[rom_number]->RomInfo.at(fileActions->FlashMethod).toStdString();
    const bool info_updated = configSession->select_by_protocol_name(flash_method);
```

  and keep its `LOG_D` lines and status-bar update.
- The kernel-directory slash fix-ups (two sites, ~930 and ~959) operate on a local `QString kernel_dir = qs(configSession->effective_paths().kernel_files_directory);`. Never write back to the session.
- `menu_actions.cpp`: `Settings settings(*configSession);`.
- `menu_actions.cpp`: the SSM-variant, `MUT_DMA`/`CDBG` checks, and the definition-files list use the table above.
- `log_operations_ssm.cpp`: `QString log_file_name = qs(configSession->effective_paths().datalog_files_directory);`, keeping its trailing-slash check on the local copy.

When the table is applied, `grep -n "configValues\|ConfigValuesStruct" src/ui/desktop src/backend/definitions src/backend/calibration apps` must print nothing.

#### 5e. Green, commit, mutation-check

- [ ] **Step 14: Run everything touched and confirm it passes**

Run: `bazel test --config=release //apps/desktop:all //src/ui/desktop/... //src/backend/definitions:all //src/backend/definition/... //src/backend/calibration/... //src/backend/config/...`
Expected: PASS. Then run `bazel build -k --config=release //...` and expect a clean build (the legacy config package still compiles; nothing links it).

- [ ] **Step 15: Commit**

```bash
git add -A apps src
git commit -m "refactor: move every configuration consumer onto ConfigSession (step 6k-2)" -m "DesktopComposition owns and initializes the session before building any
service, and a required configuration failure now stops startup with
a message naming the file, where LegacyConfigAdapter used to proceed
into empty lists. Vehicle and protocol dialogs hold tentative choices;
ROM-triggered selection goes through the shared session." -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 16: Mutation-check the startup rejection guard**

In `apps/desktop/desktop_composition.cpp`, delete the `return;` inside the failed-initialization branch so construction falls through. Then run:

Run: `bazel test --config=release //apps/desktop:desktop_composition_test`
Expected: FAIL in `failedStartupBuildsNoServicesAndPerformsNoEcuIo` (and `malformedSettingsRejectStartup`) on the `!composition.syslog_thread_` / `!composition.serial_` checks.

Restore and prove it is restored:

```bash
git checkout -- apps/desktop/desktop_composition.cpp
git diff --exit-code
bazel test --config=release //apps/desktop:desktop_composition_test
```

Expected: no diff; PASS. Record the failing output's test names in the PR description.

---

### Task 6: Retire the legacy configuration code

**Files:**
- Delete: `src/backend/config/legacy/` (whole package), `src/backend/definitions/config_values.h`
- Modify: `src/backend/definitions/{BUILD.bazel,file_actions.cpp,model_validation_test.cpp,log_values.h}`, `bazel/qt/BUILD.bazel`, `src/backend/ports/event_sink.h`, `src/backend/checksum/{checksum_selection.h,dispatch.cpp}`, `src/backend/flash/eeprom/eeprom_read_plan.cpp`, `src/backend/calibration/legacy/legacy_calibration_adapter.h`, `src/ui/desktop/BUILD.bazel`

**Interfaces:**
- Consumes: Task 5 left `validate_flash_protocols` and `config_values.h` as the only users of the legacy struct.
- Produces: no `ConfigValuesStructure`, `LegacyConfigAdapter`, `paths_from_config_values`, `legacy_config_paths`, or `config_values` target anywhere. The `qt_layer` group loses `//src/backend/config/legacy`.

- [ ] **Step 1: Write the failing guard**

Run: `grep -rn "ConfigValuesStructure\|LegacyConfigAdapter\|legacy_config_paths\|legacy_config_adapter\|paths_from_config_values\|config_values\|flash_protocol_selected_\|validate_flash_protocols" src apps tests bazel BUILD.bazel`
Expected: matches (this is the list to clear).

- [ ] **Step 2: Delete and clean up**

```bash
git rm -r src/backend/config/legacy
git rm src/backend/definitions/config_values.h
```

- `bazel/qt/BUILD.bazel`: remove the line `"//src/backend/config/legacy",` from `qt_layer`. This is a ratchet removal, never an addition.
- `src/backend/definitions/BUILD.bazel`: delete the `config_values` target and its comment block. Remove `:config_values` from every deps list.
- `file_actions.{h,cpp}`: delete `validate_flash_protocols` (declaration and definition) and the `config_values.h` include. `validateListLength`/`validateRequiredField` stay if the other validators use them.
- `model_validation_test.cpp`: delete `configValues_compareByValue`, the flash-protocol validation cases, and `appendFlashProtocol`.
- Comments that name the old struct:
  - `checksum_selection.h:11` and `dispatch.cpp:74`: `// the selected vehicle's make (ConfigSession)`.
  - `eeprom_read_plan.cpp:37`: `the selected vehicle's protocol_name`.
  - `event_sink.h`: `NullEventSink`'s comment says "for call sites that do not yet have anywhere to route events"; drop the `LegacyConfigAdapter` example.
  - `log_values.h` and `legacy_calibration_adapter.h`: reword any sentence that cites `config_values.h`/`ConfigValuesStructure` as precedent to cite `ecu_cal_def.h` instead.
- `src/ui/desktop/BUILD.bazel`: confirm no `legacy_config` dep remains.

- [ ] **Step 3: Verify the guard is empty and everything builds**

Run the Step 1 grep again.
Expected: no output.

Run: `bazel build -k --config=release //... && bazel test -k --config=release //...`
Expected: PASS. `//:portable_closure` still builds, and the Qt reachability guard passes with the shorter `qt_layer`.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor(config): delete ConfigValuesStructure and the legacy config adapters (step 6k-2)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Close out step 6k in the docs

**Files:**
- Modify: `docs/design-notes.md`, `docs/modularization-plan.md`, `docs/tech-debt.md`, and `docs/flash-qualification-matrix.md` / `docs/subaru-tcu-hitachi-m32r-can-bench-checklist.md` only where they name deleted code
- Delete: `docs/superpowers/specs/2026-09-27-configuration-models-design.md`, this plan

- [ ] **Step 1: `docs/design-notes.md`**

Add a section after the "UI channels" section:

```markdown
## Configuration session

`fastecu::config::ConfigSession` replaced `ConfigValuesStructure`, `LegacyConfigAdapter`, and `legacy_config_paths` in step 6k. `DesktopComposition` owns it and initializes it before building any other service. The same session reaches `MainWindow` through `MainWindowServices` and reaches `FileActions` by constructor.

- **One saved selection.** `AppConfig::selected_protocol_id` is the only selection state. Make, model, MCU, checksum, capabilities, and description are derived from the selected `ResolvedCarModel` and never cached. Vehicle rows keep file order, and a row's id is its position. Choosers sort only their presentation.
- **Paths.** Provisioned paths are fixed for the run. Effective paths take only the calibration and datalog directories from settings, so editing them never moves the config, kernel, definition, or syslog files.
- **Startup rejection (intentional behavior change).** `LegacyConfigAdapter` ignored provisioning and load failures, and startup proceeded into empty vehicle lists. A provisioning failure, an unreadable or malformed `fastecu.cfg`, an unreadable `protocols.cfg`, or one with no car models now shows the failing path and reason, then exits before `MainWindow` or the syslog thread exists, with no ECU I/O. A failed rewrite of a successfully loaded `fastecu.cfg` stays nonfatal and is shown as a startup warning. `DesktopCompositionTest::failedStartupBuildsNoServicesAndPerformsNoEcuIo` pins this and was mutation-checked.
- **Kept quirks.** An invalid saved row selects row 0 without replacing the saved transports or log protocol. Protocol-name selection takes the last matching row. An unresolved protocol reference stays `std::nullopt` and shows the single-space placeholder, with no capabilities. The writer's `logfiles_directory` versus the reader's `datalog_files_directory` still keeps the datalog directory from round-tripping, and `ConfigSessionSave.DatalogDirectoryDoesNotRoundTrip` pins it.
- **Definition indexes are not settings.** The eight EcuFlash/RomRaider index lists live in `FileActions::DefinitionIndexes` until `FileActions` itself is retired.
```

If an earlier section describes `LegacyConfigAdapter`, the config bridge, or `ConfigValuesStructure` as current, rewrite it in the past tense and link the new section.

- [ ] **Step 2: `docs/modularization-plan.md` and `docs/tech-debt.md`**

- `modularization-plan.md` `### 6k — Configuration and protocol models`: rewrite the section as done. One paragraph: what moved, that startup rejection was an intentional correction (link the design-notes section), and that 6l–6n remain.
- Update the "Four bridges remain" line (line ~54) to three, removing `LegacyConfigAdapter`.
- `tech-debt.md` (lines ~207, 224, 248, 459): drop `ConfigValuesStructure` from the lists of remaining legacy structs, and leave `LogValuesStructure` and `EcuCalDefStructure`.
- The two bench docs: only if they name `ConfigValuesStructure` fields as code, reword to "the selected vehicle's …". Do not change any qualification status.

Run: `prek run --all-files`
Expected: PASS, including lychee link checks.

- [ ] **Step 3: Commit the docs, then remove the spec and plan**

```bash
git add docs
git commit -m "docs: close out step 6k (configuration session)" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git rm docs/superpowers/specs/2026-09-27-configuration-models-design.md docs/superpowers/plans/2026-09-27-step6k-config-session.md
git commit -m "docs: remove the step 6k spec and plan" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 4: Final gates for PR 6k-2**

```bash
prek run --all-files
bazel build -k --config=release //...
bazel test -k --config=release //...
bazel run --config=release //:clang_tidy_report_changed
scripts/package-macos.sh   # on macOS; the Windows packaging script runs in CI
```

Expected: all green. Record which platforms' CI and packaging you could not run locally as **pending** in the PR description.

---

### Task 8: Publish the stack

- [ ] **Step 1: Stack and submit**

```bash
gh stack init --base master feat/6k-config-session refactor/step6k-1-config-session refactor/step6k-2-config-consumers
gh stack submit --auto
```

PR descriptions:
- 6k-0: the spec and this plan.
- 6k-1: Tasks 1–3, "adds `ConfigSession`; nothing consumes it yet".
- 6k-2: Tasks 4–7. List the intentional startup-rejection change, the mutation-check result from Task 5 Step 16, and the pending CI/packaging platforms.

End each PR description with:

```
🤖 Generated with [Claude Code](https://claude.com/claude-code)
```

- [ ] **Step 2: Watch CI**

Wait for Windows, macOS, and Linux CI on each PR. A Windows-only crash in `//tests:serial_backend_tests` is a known pre-existing flake: rerun it and do not attribute it to this stack.
