#!/usr/bin/env python3
"""Run positive/negative inline-check fixtures with the real C++ dependencies."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import clang_tidy_runner as runner
import clang_tidy_toolchain as toolchain
import yaml


class ClangTidyChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workspace = runner.workspace_root(os.environ)
        cls.host = toolchain.runfiles_analysis_host(os.environ)
        cls.tools = runner.discover_tools(
            "report",
            platform_name=runner.sys.platform,
            environ=dict(os.environ, FASTECU_LLVM_ROOT=str(cls.host.llvm_root)),
        )
        entries = json.loads((cls.workspace / "compile_commands.json").read_text())
        cls.entry = next(
            entry
            for entry in entries
            if str(entry["file"]).endswith("src/backend/ports/result_test.cpp")
        )

    def assert_diagnostics(
        self,
        body: str,
        filename: str = "fixture_test.cpp",
        plugin: bool = False,
        checks: str | None = None,
        exceptions: list[dict[str, str]] | None = None,
    ) -> None:
        # Literal markers identify independently expected user-file locations.
        # Plugin checks are registered without the custom- prefix.
        prefix = "fastecu-"
        load = [argument.replace("--load=", "-load=") for argument in self.host.arguments]
        expected = set()
        for line, text in enumerate(body.splitlines(), 1):
            for severity in ("error", "warning"):
                marker = f"// {severity}: "
                if marker in text:
                    expected.add((prefix + text.split(marker)[1], line, severity.title()))
        if checks is None:
            checks = ",".join(sorted({name for name, _, _ in expected}))
        if not checks:
            raise ValueError("negative fixtures must name the check they exercise")
        with tempfile.TemporaryDirectory(prefix="fastecu-inline-fixture-") as directory:
            source = Path(directory) / filename
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(body)
            entry = dict(self.entry)
            arguments = list(entry["arguments"])
            original = str(entry["file"])
            arguments = [str(source) if arg == original else arg for arg in arguments]
            entry.update(file=str(source), arguments=arguments)
            (Path(directory) / "compile_commands.json").write_text(json.dumps([entry]))
            fixes = Path(directory) / "fixes"
            fixes.mkdir()
            configuration = yaml.safe_load((self.workspace / ".clang-tidy").read_text())
            options = configuration.get("CheckOptions", {})
            if isinstance(options, list):
                options = {item["key"]: item["value"] for item in options}
            policy_file = self.workspace / ".clang-tidy-exceptions.yaml"
            if exceptions is not None:
                policy_file = Path(directory) / ".clang-tidy-exceptions.yaml"
                policy_file.write_text(yaml.safe_dump({"version": 1, "exceptions": exceptions}))
            options["fastecu.ExceptionsFile"] = str(policy_file)
            usage = Path(directory) / "usage"
            usage.mkdir()
            configuration["CheckOptions"] = options
            fixture_config = Path(directory) / "config.yaml"
            fixture_config.write_text(yaml.safe_dump(configuration))
            result = subprocess.run(
                [
                    runner.sys.executable,
                    self.tools.run_clang_tidy,
                    "-clang-tidy-binary",
                    str(self.host.executable),
                    "-config-file",
                    str(fixture_config),
                    f"-checks=-*,{checks}",
                    *load,
                    "-extra-arg=-Wno-error",
                    "-p",
                    directory,
                    "-export-fixes",
                    str(fixes) + os.sep,
                    "-j",
                    "1",
                ],
                capture_output=True,
                text=True,
                cwd=self.workspace,
                env={**os.environ, "FASTECU_TIDY_EXCEPTION_USAGE_DIR": str(usage)},
            )
            # Advisory findings must succeed; existing error-level checks still fail.
            self.assertEqual(
                any(level == "Error" for _, _, level in expected),
                result.returncode != 0,
                result.stdout + result.stderr,
            )
            if exceptions is not None:
                used = [
                    record
                    for path in usage.glob("*.json")
                    for record in json.loads(path.read_text())
                ]
                self.assertEqual(
                    {(entry["check"], entry["file"], entry["symbol"]) for entry in exceptions},
                    {(entry["check"], entry["file"], entry["symbol"]) for entry in used},
                )
            actual = set()
            diagnostics = [
                diagnostic
                for file in fixes.glob("*.yaml")
                for diagnostic in (yaml.safe_load(file.read_text()) or {}).get("Diagnostics", [])
            ]
            for diagnostic in diagnostics:
                message = diagnostic["DiagnosticMessage"]
                if Path(message["FilePath"]).resolve() == source.resolve():
                    line = source.read_bytes()[: message["FileOffset"]].count(b"\n") + 1
                    actual.add((diagnostic["DiagnosticName"], line, diagnostic["Level"]))
                    self.assertEqual(
                        ["typename"]
                        if diagnostic["DiagnosticName"] == "fastecu-template-typename"
                        else [],
                        [r["ReplacementText"] for r in message["Replacements"]],
                    )
            self.assertEqual(expected, actual, result.stdout + result.stderr)

    def test_linear_search_returns_matching_catalog_entry_address(self) -> None:
        self.assert_diagnostics("""#include <array>
#include <string_view>
struct CatalogEntry { std::string_view protocol; int id; };
constexpr std::array<CatalogEntry, 2> kCatalog{{{"stock", 1}, {"custom", 2}}};
const CatalogEntry *FindCatalog(std::string_view protocol) {
    for (const CatalogEntry& entry : kCatalog) { // error: linear-search-loop
        if (entry.protocol == protocol) {
            return &entry;
        }
    }
    return nullptr;
}
const CatalogEntry *FindId(int id) {
    for (const CatalogEntry& entry : kCatalog) { // error: linear-search-loop
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}
const CatalogEntry *FindUnrelated(std::string_view protocol, const CatalogEntry& other) {
    for (const CatalogEntry& entry : kCatalog) {
        if (other.protocol == protocol) {
            return &entry;
        }
    }
    return nullptr;
}
const CatalogEntry *ReturnOther(std::string_view protocol, const CatalogEntry& other) {
    for (const CatalogEntry& entry : kCatalog) {
        if (entry.protocol == protocol) {
            return &other;
        }
    }
    return nullptr;
}
""")

    def test_linear_search_wrapped_compound_and_indexed_returns(self) -> None:
        self.assert_diagnostics("""#include <array>
#include <expected>
#include <string_view>
struct Entry { std::string_view name; int kind; };
constexpr std::array<Entry, 2> kEntries{{{"stock", 1}, {"custom", 2}}};
struct Device { const char *name; };
const Device kDevices[] = {{"stock"}, {"custom"}, {nullptr}};
std::expected<const Entry *, int> FindVariant(std::string_view name, int kind) {
    for (const Entry& entry : kEntries) { // error: linear-search-loop
        if (entry.name == name) {
            if (entry.kind != kind) { return std::unexpected(1); }
            return &entry;
        }
    }
    return std::unexpected(2);
}
const Entry *FindCompound(std::string_view name, int kind) {
    for (const Entry& entry : kEntries) { // error: linear-search-loop
        if (entry.name == name && entry.kind == kind) { return &entry; }
    }
    return nullptr;
}
int FindIndex(std::string_view name) {
    for (int index = 0; kDevices[index].name != nullptr; ++index) { // error: linear-search-loop
        if (name == kDevices[index].name) { return index; }
    }
    return -1;
}
const Entry *FindIndexedAddress(std::string_view name) {
    for (unsigned index = 0; index < kEntries.size(); ++index) { // error: linear-search-loop
        if (kEntries[index].name == name) { return &kEntries[index]; }
    }
    return nullptr;
}
const Entry *ReturnUnrelatedIndex(std::string_view name, const Entry& other) {
    for (unsigned index = 0; index < kEntries.size(); ++index) {
        if (kEntries[index].name == name) { return &other; }
    }
    return nullptr;
}
const Entry *ReturnUnrelatedCompound(std::string_view name, int kind, const Entry& other) {
    for (const Entry& entry : kEntries) {
        if (other.name == name && entry.kind) { return &entry; }
    }
    return nullptr;
}
int RetryCounter(int target) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt == target) { return attempt; }
    }
    return -1;
}
std::expected<const Entry *, int> ReturnOtherWrapped(std::string_view name, const Entry& other) {
    for (const Entry& entry : kEntries) {
        if (entry.name == name) { return &other; }
    }
    return std::unexpected(1);
}
struct ValidationFailure { const Entry *entry; int error; };
ValidationFailure StatefulValidation(std::string_view name) {
    bool seen = false;
    for (const Entry& entry : kEntries) {
        if (entry.name == name && seen) { return ValidationFailure{&entry, 1}; }
        seen = true;
    }
    return {nullptr, 0};
}
void NestedLambda(std::string_view name) {
    for (const Entry& entry : kEntries) {
        if (entry.name == name) { auto callback = [&] { return &entry; }; }
        auto callback = [&]() -> const Entry * {
            if (entry.name == name) { return &entry; }
            return nullptr;
        };
    }
}
""")

    def test_redundant_address_dereference_includes_macros_but_excludes_overloads(self) -> None:
        self.assert_diagnostics("""#include <array>
#include <expected>
#include <memory>
#include <optional>
#define POINTER_ROUND_TRIP(p) (&*(p))
#define DEREFERENCE(p) (*(p))
#define ADDRESS(v) (&(v))
struct AddressOverload {
    AddressOverload *operator&() { return nullptr; }
};
struct DereferenceOverload {
    int& operator*();
};
template<class T> T *RoundTrip(T *pointer) {
    return &*pointer; // error: redundant-address-dereference
}
template<class T> auto OverloadedRoundTrip(T& value) { return &*value; }
void PointerExpressions(int *pointer, int value, void (*callback)(),
                        AddressOverload *overloaded_address,
                        DereferenceOverload overloaded_dereference) {
    auto direct = &*pointer; // error: redundant-address-dereference
    auto parenthesized = &(*pointer); // error: redundant-address-dereference
    auto nested_parentheses = &((*(pointer))); // error: redundant-address-dereference
    const int *const_pointer = pointer;
    auto qualified = &*const_pointer; // error: redundant-address-dereference
    std::array<int, 2> array{1, 2};
    int *raw_iterator = array.data();
    auto iterator_address = &*raw_iterator; // error: redundant-address-dereference
    auto template_result = RoundTrip(pointer);
    auto function_address = &*callback; // error: redundant-address-dereference
    auto address = std::to_address(pointer);
    auto reversed = *&value;
    auto address_overload = &*overloaded_address;
    auto dereference_overload = &*overloaded_dereference;
    std::optional<int> optional{value};
    std::expected<int, int> expected{value};
    auto optional_address = &*optional;
    auto expected_address = &*expected;
    auto optional_template = OverloadedRoundTrip(optional);
    auto expected_template = OverloadedRoundTrip(expected);
    auto macro_pair = POINTER_ROUND_TRIP(pointer); // error: redundant-address-dereference
    auto macro_dereference = &DEREFERENCE(pointer); // error: redundant-address-dereference
    auto macro_address = ADDRESS(*pointer); // error: redundant-address-dereference
}
""")

    def test_redundant_address_dereference_includes_contiguous_iterators(self) -> None:
        self.assert_diagnostics("""#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>
#define ITERATOR_ADDRESS(it) (&*(it))
template<class Container> auto FindAddress(const Container& values, int value) {
    const auto found = std::ranges::find(values, value);
    return found == values.end() ? nullptr : &*found; // error: redundant-address-dereference
}
void ContiguousAddresses(std::vector<int>& values, const std::vector<int>& constants,
                         std::string& text, const std::string& constant_text,
                         std::array<int, 2>& array, std::span<int> span) {
    auto mutable_iterator = values.begin();
    auto mutable_address = &*mutable_iterator; // error: redundant-address-dereference
    auto constant_iterator = constants.begin();
    auto constant_address = &(*constant_iterator); // error: redundant-address-dereference
    auto text_iterator = text.begin();
    auto text_address = &*text_iterator; // error: redundant-address-dereference
    auto constant_text_iterator = constant_text.begin();
    auto constant_text_address = &*constant_text_iterator; // error: redundant-address-dereference
    auto array_iterator = array.begin();
    auto array_address = &*array_iterator; // error: redundant-address-dereference
    auto span_iterator = span.begin();
    auto span_address = &*span_iterator; // error: redundant-address-dereference
    auto macro_address = ITERATOR_ADDRESS(mutable_iterator); // error: redundant-address-dereference
    auto template_address = FindAddress(constants, 42);
    auto replacement = std::to_address(mutable_iterator);
}
""")

    def test_redundant_address_dereference_excludes_other_overloads(self) -> None:
        self.assert_diagnostics(
            """#include <iterator>
#include <list>
#include <memory>
#include <vector>
struct AddressOverload { AddressOverload *operator&(); };
struct Proxy { Proxy *operator&(); };
struct ProxyIterator { Proxy operator*(); };
struct CustomIterator { int& operator*(); int *operator->(); };
namespace unrelated {
template<class T> struct _Vector_iterator { T& operator*(); T *operator->(); };
namespace __gnu_cxx {
template<class T> struct __normal_iterator { int& operator*(); int *operator->(); };
}
}
struct DerivedIterator : std::vector<int>::iterator { int *operator->() const; };
void OtherAddresses(std::vector<int>& values, std::list<int>& nodes,
                    std::vector<AddressOverload>& overloaded,
                    std::unique_ptr<int>& unique, std::shared_ptr<int>& shared,
                    ProxyIterator proxy, CustomIterator custom,
                    unrelated::_Vector_iterator<int> lookalike,
                    unrelated::__gnu_cxx::__normal_iterator<int*> nested_lookalike,
                    DerivedIterator derived) {
    auto reverse = values.rbegin();
    auto reverse_address = &*reverse;
    auto node = nodes.begin();
    auto node_address = &*node;
    auto address_iterator = overloaded.begin();
    auto overloaded_address = &*address_iterator;
    auto unique_address = &*unique;
    auto shared_address = &*shared;
    auto proxy_address = &*proxy;
    auto custom_address = &*custom;
    auto lookalike_address = &*lookalike;
    auto nested_lookalike_address = &*nested_lookalike;
    auto derived_address = &*derived;
}
""",
            checks="fastecu-redundant-address-dereference",
        )

    def test_result_assertions_distinguish_optional_and_matchers(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include <optional>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
bool CheckFlags(bool flags) { return flags; }
struct Flag { bool value; explicit operator bool() const { return value; } };
TEST(InlineChecks, ResultAssertions) {
    fastecu::Result<int> result{42};
    fastecu::Status status{};
    std::optional<int> optional{42};
    fastecu::Result<std::optional<int>> nested{std::optional<int>{42}};
    const auto *pointer = &result;
    EXPECT_TRUE(result.has_value()); // error: result-assertion
    ASSERT_TRUE((status.has_value())); // error: result-assertion
    EXPECT_FALSE(!pointer->has_value()); // error: result-assertion
    ASSERT_FALSE(result.has_value()); // error: result-assertion
    EXPECT_TRUE(nested.has_value()); // error: result-assertion
    EXPECT_TRUE(optional.has_value());
    EXPECT_TRUE(nested->has_value());
    EXPECT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(result.has_value() || optional.has_value());
    EXPECT_TRUE(CheckFlags(result.has_value()));
    EXPECT_TRUE(Flag{result.has_value()});
    auto consume = [](auto action) { action(); };
    consume([&] { EXPECT_TRUE(result.has_value()); }); // error: result-assertion
    if (result.has_value()) { EXPECT_EQ(*result, 42); }
}
""")

    def test_low_level_result_api_assertions_are_exempt(self) -> None:
        self.assert_diagnostics(
            """#include <gtest/gtest.h>
#include "src/backend/ports/result.h"
TEST(InlineChecks, LowLevelResultApi) {
    fastecu::Result<int> result{42};
    ASSERT_TRUE(result.has_value());
}
""",
            "src/backend/ports/result_test.cpp",
            checks="fastecu-result-assertion",
        )

    def test_first_party_header_filter_reports_actual_header_declarations(self) -> None:
        with tempfile.TemporaryDirectory(prefix="fastecu-header-fixture-") as directory:
            root = Path(directory)
            header = root / "src/probe.hpp"
            header.parent.mkdir()
            header.write_text("template <class T> struct HeaderProbe {};\n")
            generated = root / "src/generated_ui/moc_probe.h"
            generated.parent.mkdir()
            generated.write_text("template <class T> struct GeneratedProbe {};\n")
            source = root / "main.cpp"
            source.write_text('#include "src/probe.hpp"\n#include "src/generated_ui/moc_probe.h"\n')
            configuration = {
                "Checks": "-*,fastecu-template-typename",
                "WarningsAsErrors": "*",
                "HeaderFilterRegex": runner.first_party_header_filter(root),
                "ExcludeHeaderFilterRegex": yaml.safe_load(
                    (self.workspace / ".clang-tidy").read_text()
                )["ExcludeHeaderFilterRegex"],
            }
            config = root / "config.yaml"
            config.write_text(yaml.safe_dump(configuration))
            entry = dict(self.entry)
            arguments = [
                str(source) if arg == str(entry["file"]) else arg for arg in entry["arguments"]
            ]
            entry.update(file=str(source), arguments=[*arguments, "-I", directory])
            (root / "compile_commands.json").write_text(json.dumps([entry]))
            fixes = root / "fixes.yaml"
            result = subprocess.run(
                [
                    str(self.host.executable),
                    *self.host.arguments,
                    f"--config-file={config}",
                    "-extra-arg=-Wno-error",
                    "-p",
                    directory,
                    f"-export-fixes={fixes}",
                    str(source),
                ],
                capture_output=True,
                text=True,
                cwd=self.workspace,
            )
            self.assertEqual(1, result.returncode, result.stdout + result.stderr)
            diagnostics = (
                (yaml.safe_load(fixes.read_text()) or {}).get("Diagnostics", [])
                if fixes.exists()
                else []
            )
            self.assertEqual(1, len(diagnostics), result.stdout + result.stderr)
            self.assertEqual(
                header.resolve(), Path(diagnostics[0]["DiagnosticMessage"]["FilePath"]).resolve()
            )

    def test_container_predicates_distinguish_plain_booleans_and_matchers(self) -> None:
        self.assert_diagnostics("""#include <algorithm>
#include <gmock/gmock.h>
#include <vector>
bool CheckFlags(bool flags) { return flags; }
TEST(InlineChecks, ContainerAssertions) {
    std::vector<int> values{1, 2};
    auto positive = [](int value) { return value > 0; };
    EXPECT_TRUE(std::ranges::all_of(values, positive)); // error: container-assertion
    ASSERT_FALSE(std::ranges::any_of(values, positive)); // error: container-assertion
    EXPECT_FALSE(std::ranges::none_of(values, positive)); // error: container-assertion
    ASSERT_TRUE(std::all_of( // error: container-assertion
        values.begin(), values.end(), positive));
    EXPECT_FALSE(std::any_of( // error: container-assertion
        values.begin(), values.end(), positive));
    ASSERT_FALSE(std::none_of( // error: container-assertion
        values.begin(), values.end(), positive));
    EXPECT_THAT(values, ::testing::Each(::testing::Gt(0)));
    const bool all_positive = std::ranges::all_of(values, positive);
    EXPECT_TRUE(all_positive);
    EXPECT_TRUE(std::ranges::all_of(values, positive) || values.empty());
    EXPECT_TRUE(CheckFlags(std::ranges::all_of(values, positive)));
    auto consume = [](auto action) { action(); };
    consume([&] {
        EXPECT_TRUE(std::ranges::all_of(values, positive)); // error: container-assertion
    });
}
""")

    def test_message_concatenation_excludes_path_joins_and_formatted_messages(self) -> None:
        self.assert_diagnostics("""#include <format>
#include <string>
#include <filesystem>
#include "src/backend/ports/result.h"
#include "src/backend/ports/event_sink.h"
void Messages(fastecu::IEventSink& events, std::string name) {
    auto bad = fastecu::Fail( // error: message-concatenation
        fastecu::ErrorKind::kTimeout, "timeout: " + name);
    events.Log(fastecu::LogLevel::kInfo, "selected " + name); // error: message-concatenation
    auto formatted = fastecu::Fail(fastecu::ErrorKind::kTimeout, std::format("timeout: {}", name));
    const auto path = "directory/" + name;
    auto literal = fastecu::Fail(fastecu::ErrorKind::kTimeout, "timeout");
    events.Log(fastecu::LogLevel::kInfo, std::format("selected {}", name));
    auto nested_path = fastecu::Fail(fastecu::ErrorKind::kTimeout,
        std::filesystem::path("directory/" + name).string());
    auto format_then_concat = fastecu::Fail( // error: message-concatenation
        fastecu::ErrorKind::kTimeout, "prefix " + std::format("{}", name));
}
""")

    def test_message_callee_identity_and_wrappers(self) -> None:
        self.assert_diagnostics("""#include <string>
#include <string_view>
#include <format>
#include "src/backend/ports/result.h"
#include "src/backend/ports/event_sink.h"
namespace unrelated {
void Fail(fastecu::ErrorKind, std::string) {}
struct IEventSink { void Log(fastecu::LogLevel, std::string_view) {} };
}
struct DerivedSink : fastecu::IEventSink {
    void Log(fastecu::LogLevel, std::string_view) override {}
};
template<class T> void TemplateMessage(T name) {
    auto error = fastecu::Fail( // error: message-concatenation
        fastecu::ErrorKind::kTimeout, "template " + name);
}
void Messages(fastecu::IEventSink& events, DerivedSink& derived, std::string name) {
    auto parentheses = fastecu::Fail( // error: message-concatenation
        fastecu::ErrorKind::kTimeout, (("prefix " + name)));
    auto chain = fastecu::Fail( // error: message-concatenation
        fastecu::ErrorKind::kTimeout, "prefix " + name + " suffix");
    using fastecu::Fail;
    auto imported = Fail( // error: message-concatenation
        fastecu::ErrorKind::kTimeout, name + " suffix");
    events.Log(fastecu::LogLevel::kInfo, (("prefix " + name))); // error: message-concatenation
    events.Log( // error: message-concatenation
        fastecu::LogLevel::kInfo, "prefix " + name + " suffix");
    events.Log( // error: message-concatenation
        fastecu::LogLevel::kInfo, std::format("{}", name) + " suffix");
    derived.fastecu::IEventSink::Log( // error: message-concatenation
        fastecu::LogLevel::kInfo, "prefix " + name);
    derived.Log(fastecu::LogLevel::kInfo, "prefix " + name);
    unrelated::Fail(fastecu::ErrorKind::kTimeout, "prefix " + name);
    unrelated::IEventSink other;
    other.Log(fastecu::LogLevel::kInfo, "prefix " + name);
    const auto local = "prefix " + name;
    events.Log(fastecu::LogLevel::kInfo, local);
    TemplateMessage(name);
}
""")

    def test_template_type_parameters_use_typename(self) -> None:
        self.assert_diagnostics(
            plugin=True,
            body="""#include <concepts>
template <class First, // error: template-typename
          typename Second, class... Rest> // error: template-typename
struct Pair {};
template <class> // error: template-typename
void Unnamed() {}
template <class Value = int> // error: template-typename
void Defaulted() {}
template <template <class> class Wrapper> // error: template-typename
void Nested() {}
template <typename Value> void Fine() {}
template <typename... Values> void FinePack() {}
template <std::integral Constrained> void FineConstrained() {}
template <auto Value> void NonType() {}
template <template <typename> class Wrapper> void FineNested() {}
void Implicit() {
    auto generic = [](auto value) { return value; };
    (void)generic(1);
}
void Abbreviated(auto value) { (void)value; }
template <class Guided> struct Wrap { Wrap(Guided) {} }; // error: template-typename
void Deduce() { Wrap wrapped(1); }
""",
        )

    def test_index_loop_returning_bool_suggests_any_all_of(self) -> None:
        self.assert_diagnostics("""#include <array>
#include <cstddef>
#include <vector>
struct Block { int start; int len; };
bool MatchesExpected(const std::vector<Block>& blocks, const std::array<Block, 2>& expected) {
    for (std::size_t i = 0; i < blocks.size(); ++i) { // error: any-all-index-loop
        if (blocks[i].start != expected[i].start) {
            return false;
        }
    }
    return true;
}
bool HasFlag(const std::vector<int>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) { // error: any-all-index-loop
        if (values[i] == 3) {
            return true;
        }
    }
    return false;
}
bool Unrelated(const std::vector<int>& values) {
    for (const int value : values) {
        if (value == 3) {
            return true;
        }
    }
    return false;
}
int Attempt(int value) { return value; }
bool Retry() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (Attempt(attempt) == 2) {
            return true;
        }
    }
    return false;
}
int FindIndex(const std::vector<int>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == 3) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
bool TwoStatements(const std::vector<int>& values, int& seen) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        ++seen;
        if (values[i] == 3) {
            return true;
        }
    }
    return false;
}
""")

    def test_counting_loop_suggests_count_if(self) -> None:
        self.assert_diagnostics("""#include <cstddef>
#include <vector>
int CountLarge(const std::vector<int>& values) {
    int large = 0;
    for (const int value : values) { // error: count-if-loop
        if (value > 3) {
            ++large;
        }
    }
    return large;
}
int CountIndexed(const std::vector<int>& values) {
    int hits = 0;
    for (std::size_t i = 0; i < values.size(); ++i) { // error: count-if-loop
        if (values[i] == 3) {
            hits++;
        }
    }
    return hits;
}
int CountAll(const std::vector<int>& values) {
    int total = 0;
    for (const int value : values) {
        ++total;
    }
    return total;
}
int CountTwoStatements(const std::vector<int>& values, int& seen) {
    int hits = 0;
    for (const int value : values) {
        ++seen;
        if (value == 3) {
            ++hits;
        }
    }
    return hits;
}
""")

    def test_summing_loop_suggests_fold(self) -> None:
        self.assert_diagnostics("""#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
std::uint32_t SumBytes(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t sum = 0;
    for (const std::uint8_t byte : bytes) { // error: accumulate-loop
        sum += byte;
    }
    return sum;
}
std::uint32_t SumBlocks(const std::vector<std::uint32_t>& lengths) {
    std::uint32_t total = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) { // error: accumulate-loop
        total += lengths[i];
    }
    return total;
}
std::string Joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) { // error: accumulate-loop
        out += part;
    }
    return out;
}
std::uint32_t SumWords(const std::vector<std::uint8_t>& rom, std::size_t size) {
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset + 4 <= size; offset += 4) {
        sum += rom[offset];
    }
    return sum;
}
std::uint32_t SumConstant(int count) {
    std::uint32_t sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += 1;
    }
    return sum;
}
""")

    def test_collecting_loop_suggests_transform(self) -> None:
        self.assert_diagnostics("""#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
std::vector<std::uint8_t> Indices(int count) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < count; ++i) { // error: transform-collect-loop
        out.push_back(static_cast<std::uint8_t>(i));
    }
    return out;
}
std::vector<int> Doubled(const std::vector<int>& values) {
    std::vector<int> out;
    for (const int value : values) { // error: transform-collect-loop
        out.emplace_back(value * 2);
    }
    return out;
}
std::vector<std::string> Labels(const std::vector<int>& values) {
    std::vector<std::string> out;
    for (const int value : values) { // error: transform-collect-loop
        out.push_back(std::to_string(value));
    }
    return out;
}
void Constants(std::vector<int>& calls) {
    for (int i = 0; i < 3; ++i) {
        calls.push_back(7);
    }
}
void Pairs(std::vector<int>& calls, const std::vector<int>& values) {
    for (const int value : values) {
        calls.push_back(value);
        calls.push_back(value);
    }
}
""")

    def test_result_access_requires_state_specific_fatal_guards(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
using fastecu::testing::IsOk;
using fastecu::testing::IsErr;
fastecu::Result<int> ReadValue();
TEST(ResultAccess, Guards) {
    auto unsafe = ReadValue();
    EXPECT_THAT(unsafe, IsOk());
    EXPECT_EQ(*unsafe, 7); // error: result-access
    auto good = ReadValue();
    ASSERT_THAT(good, IsOk());
    EXPECT_EQ(*good, 7);
    auto error = ReadValue();
    ASSERT_THAT(error, IsErr(fastecu::ErrorKind::kTimeout));
    EXPECT_EQ(error.error().kind, fastecu::ErrorKind::kTimeout);
    EXPECT_EQ(*error, 7); // error: result-access
    auto guarded = ReadValue();
    if (!guarded.has_value()) return;
    EXPECT_EQ(*guarded, 7);
    if (guarded.has_value()) EXPECT_EQ(*guarded, 7);
    else EXPECT_EQ(guarded.error().kind, fastecu::ErrorKind::kTimeout);
}
""")

    def test_result_access_does_not_invent_proofs_across_jumps(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
bool Coin();
TEST(ResultAccess, Jump) {
    auto result = ReadValue();
    if (Coin()) goto access;
    ASSERT_THAT(result, fastecu::testing::IsOk());
access:
    EXPECT_EQ(*result, 7); // error: result-access
}
TEST(ResultAccess, Exception) {
    auto result = ReadValue();
    try {
        if (Coin()) throw 1;
        ASSERT_THAT(result, fastecu::testing::IsOk());
    } catch (...) {}
    EXPECT_EQ(*result, 7); // error: result-access
}
""")

    def test_result_access_reestablishes_proof_after_switch(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
int Choice();
TEST(ResultAccess, Switch) {
    auto result = ReadValue();
    switch (Choice()) {
    case 0:
        ASSERT_THAT(result, fastecu::testing::IsOk());
        break;
    default: break;
    }
    EXPECT_EQ(*result, 7); // error: result-access
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(*result, 7);
}
""")

    def test_result_access_invalidates_mutation_and_merges_branches(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
using fastecu::testing::IsOk;
fastecu::Result<int> ReadValue();
void Mutate(fastecu::Result<int>&);
void Observe(const fastecu::Result<int>&);
bool Coin();
TEST(ResultAccess, Mutation) {
    auto changed = ReadValue();
    ASSERT_THAT(changed, IsOk());
    changed = fastecu::Fail(fastecu::ErrorKind::kTimeout, "timeout");
    EXPECT_EQ(*changed, 7); // error: result-access
    auto passed = ReadValue();
    ASSERT_THAT(passed, IsOk());
    Observe(passed);
    EXPECT_EQ(*passed, 7);
    Mutate(passed);
    EXPECT_EQ(*passed, 7); // error: result-access
    auto joined = ReadValue();
    ASSERT_THAT(joined, IsOk());
    if (Coin()) joined = ReadValue();
    EXPECT_EQ(*joined, 7); // error: result-access
    auto stable = ReadValue();
    ASSERT_THAT(stable, IsOk());
    for (int index = 0; index < 2; ++index) EXPECT_EQ(*stable, index);
    for (int index = 0; index < 2; ++index) {
        auto fresh = ReadValue();
        ASSERT_THAT(fresh, IsOk());
        EXPECT_EQ(*fresh, index);
    }
}
""")

    def test_result_assertion_inside_a_callback(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
TEST(ResultAssertions, Callback) {
    auto verify = [] {
        auto result = ReadValue();
        ASSERT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kTimeout));
        EXPECT_EQ(result.error().kind, fastecu::ErrorKind::kTimeout); // error: result-assertion
    };
    verify();
}
""")

    def test_result_access_invalidates_reference_and_pointer_escape(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
void Mutate(fastecu::Result<int>&);
void MutatePointer(fastecu::Result<int>*);
TEST(ResultFlow, Aliases) {
    auto result = ReadValue();
    auto& alias = result;
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(*alias, 1);
    Mutate(alias);
    EXPECT_EQ(*result, 1); // error: result-access
    ASSERT_THAT(result, fastecu::testing::IsOk());
    MutatePointer(&result);
    EXPECT_EQ(*result, 1); // error: result-access
}
""")

    def test_result_boolean_conversion_requires_matcher(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include <optional>
fastecu::Result<int> ReadValue();
TEST(ResultAssertions, Conversion) {
    auto result = ReadValue();
    ASSERT_TRUE(result); // error: result-assertion
    EXPECT_FALSE(!result); // error: result-assertion
    std::optional<int> optional{1};
    ASSERT_TRUE(optional);
}
""")

    def test_fixture_setup_proof_has_an_exact_audited_exception(self) -> None:
        self.assert_diagnostics(
            """#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
class SharedSetup : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_THAT(result, fastecu::testing::IsOk()); }
    const fastecu::Result<int> result = ReadValue();
};
TEST_F(SharedSetup, Allowed) { EXPECT_EQ(*result, 1); }
TEST_F(SharedSetup, Other) { EXPECT_EQ(*result, 1); } // error: result-access
""",
            exceptions=[
                {
                    "check": "fastecu-result-access",
                    "file": "fixture_test.cpp",
                    "symbol": "SharedSetup.Allowed",
                    "rationale": "Fatal SetUp proves the immutable Result before this body.",
                }
            ],
        )

    def test_result_access_accounts_for_unspecified_argument_order(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
int MutateAndCount(fastecu::Result<int>&);
void Consume(int, int);
TEST(ResultFlow, SiblingMutation) {
    auto result = ReadValue();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    Consume(*result, MutateAndCount(result)); // error: result-access
    ASSERT_THAT(result, fastecu::testing::IsOk());
    const auto sum = *result + MutateAndCount(result); // error: result-access
    EXPECT_EQ(sum, 1);
}
""")

    def test_uniform_bytes_respect_range_projections(self) -> None:
        self.assert_diagnostics(
            """#include <gmock/gmock.h>
#include <algorithm>
#include "src/algorithms/protocol/bytes.h"
TEST(Bytes, Projection) {
    bytes::Bytes data{1, 3};
    EXPECT_TRUE(
        std::ranges::all_of(data, // error: uniform-bytes
            [](bytes::Byte byte) { return byte == 1; }));
    EXPECT_TRUE(std::ranges::all_of(data,
        [](bytes::Byte byte) { return byte == 1; },
        [](bytes::Byte byte) { return byte & 1; }));
}
""",
            checks="fastecu-uniform-bytes",
        )

    def test_uniform_bytes_use_specialized_matcher(self) -> None:
        self.assert_diagnostics(
            """#include <gmock/gmock.h>
#include <algorithm>
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"
TEST(Bytes, Uniform) {
    bytes::Bytes data{0xff, 0xff};
    EXPECT_THAT(data, ::testing::Each(::testing::Eq(0xff))); // error: uniform-bytes
    EXPECT_EQ(data, bytes::Bytes(2, 0xff)); // error: uniform-bytes
    EXPECT_TRUE(
        std::all_of(data.begin(), data.end(), // error: uniform-bytes
                    [](bytes::Byte byte) { return byte == 0xff; }));
    EXPECT_THAT(data, test_bytes::BytesFilledWith(0xff));
    EXPECT_EQ(data, (bytes::Bytes{0xff, 0x00}));
    std::vector<int> numbers{1, 1};
    EXPECT_THAT(numbers, ::testing::Each(::testing::Eq(1)));
}
""",
            checks="fastecu-uniform-bytes,fastecu-container-assertion",
        )

    def test_element_assertion_loop_uses_one_container_diagnostic(self) -> None:
        self.assert_diagnostics(
            """#include <gtest/gtest.h>
#include <vector>
TEST(Containers, Elements) {
    std::vector<int> values{1, 1};
    for (int value : values) { // error: container-assertion
        EXPECT_EQ(value, 1);
    }
    for (int& value : values) value = 2;
}
""",
            checks="fastecu-container-assertion,fastecu-assertion-loop-trace",
        )

    def test_assertion_loops_require_iteration_local_trace(self) -> None:
        self.assert_diagnostics("""#include <gtest/gtest.h>
TEST(Loops, Trace) {
    SCOPED_TRACE("outside");
    for (int index = 0; index < 2; ++index) { // error: assertion-loop-trace
        EXPECT_GE(index, 0);
    }
    for (int index = 0; index < 2; ++index) {
        SCOPED_TRACE(index);
        EXPECT_GE(index, 0);
    }
    for (int index = 0; index < 2; ++index) { // error: assertion-loop-trace
        auto verify = [&] { EXPECT_GE(index, 0); };
        verify();
    }
    for (int outer = 0; outer < 2; ++outer) {
        SCOPED_TRACE(outer);
        for (int inner = 0; inner < 2; ++inner) { // error: assertion-loop-trace
            EXPECT_GE(inner, 0);
        }
    }
    for (int index = 0; index < 2; ++index) {
        auto uncalled = [&] { EXPECT_GE(index, 0); };
        (void)uncalled;
    }
}
""")

    def test_result_access_respects_known_branches_and_do_while_order(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
TEST(ResultFlow, KnownBranch) {
    auto result = ReadValue();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    if (result.has_value()) EXPECT_EQ(*result, 1);
    EXPECT_EQ(*result, 1);
    auto other = ReadValue();
    do { EXPECT_EQ(*other, 1); } while (other.has_value()); // error: result-access
}
TEST(ResultFlow, NegativeConjunction) {
    auto result = ReadValue();
    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOkAnd(::testing::Eq(1))));
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::kTimeout); // error: result-access
}
""")

    def test_parameterized_cases_require_a_name_generator(self) -> None:
        self.assert_diagnostics("""#include <gtest/gtest.h>
class Cases : public ::testing::TestWithParam<int> {};
TEST_P(Cases, Value) { EXPECT_GE(GetParam(), 0); }
INSTANTIATE_TEST_SUITE_P(Unnamed, Cases, ::testing::Values(0, 1)); // error: parameterized-case-name
INSTANTIATE_TEST_SUITE_P(Named, Cases, ::testing::Values(0, 1),
                         ::testing::PrintToStringParamName());
""")

    def test_container_equality_algorithms_require_matchers(self) -> None:
        self.assert_diagnostics("""#include <gtest/gtest.h>
#include <algorithm>
#include <vector>
TEST(Containers, Equality) {
    std::vector<int> actual{1, 2};
    std::vector<int> expected{1, 2};
    EXPECT_TRUE(
        std::equal(actual.begin(), actual.end(), // error: container-assertion
                   expected.begin(), expected.end()));
    EXPECT_TRUE(std::ranges::equal(actual, expected)); // error: container-assertion
    EXPECT_EQ(actual, expected);
}
""")

    def test_plan_helpers_propagate_fallibility(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include <stdexcept>
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/testing/result_matchers.h"
using fastecu::Result;
using fastecu::flash::FlashPlan;
Result<FlashPlan> Build();
FlashPlan HiddenFailure() { // error: plan-helper
    auto result = Build();
    EXPECT_THAT(result, fastecu::testing::IsOk());
    return std::move(*result);
}
FlashPlan ThrowingFailure() { // error: plan-helper
    auto result = Build();
    if (!result.has_value()) throw std::runtime_error("failed");
    return std::move(*result);
}
Result<FlashPlan> Propagate() { return Build(); }
FlashPlan Forward(FlashPlan plan) { return plan; }
""")

    def test_single_use_result_only_when_safe(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include <memory>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Status Act();
fastecu::Result<std::unique_ptr<int>> Own();
void Observe();
TEST(SingleUse, Safe) {
    auto ordered = Act();
    Observe();
    ASSERT_THAT(ordered, fastecu::testing::IsOk());
    auto owned = Own();
    ASSERT_THAT(owned, fastecu::testing::IsOk());
    auto reused = Act();
    ASSERT_THAT(reused, fastecu::testing::IsOk());
    EXPECT_THAT(reused, fastecu::testing::IsOk());
    auto status = Act(); // error: single-use-result
    ASSERT_THAT(status, fastecu::testing::IsOk());
}
""")

    def test_result_assertion_exception_is_exact_and_reports_usage(self) -> None:
        self.assert_diagnostics(
            """#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
TEST(Exceptions, Allowed) {
    EXPECT_THAT(ReadValue(), ::testing::Not(fastecu::testing::IsOk()));
}
TEST(Exceptions, Other) {
    EXPECT_THAT(ReadValue(), ::testing::Not(fastecu::testing::IsOk())); // error: result-assertion
}
""",
            exceptions=[
                {
                    "check": "fastecu-result-assertion",
                    "file": "fixture_test.cpp",
                    "symbol": "Exceptions.Allowed",
                    "rationale": "Only the absence of a successful value matters here.",
                }
            ],
        )

    def test_result_assertions_cover_fields_messages_and_any_error(self) -> None:
        self.assert_diagnostics("""#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
using fastecu::testing::IsOk;
using fastecu::testing::IsErr;
fastecu::Result<int> ReadValue();
TEST(ResultAssertions, Diagnostics) {
    auto result = ReadValue();
    ASSERT_THAT(result, IsErr(fastecu::ErrorKind::kTimeout));
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::kTimeout); // error: result-assertion
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("timeout")); // error: result-assertion
    EXPECT_THAT(result, ::testing::Not(IsOk())); // error: result-assertion
    ASSERT_THAT(result, IsOk()) << result.error().detail; // error: result-assertion
}
""")


if __name__ == "__main__":
    unittest.main()
