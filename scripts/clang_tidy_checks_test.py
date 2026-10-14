#!/usr/bin/env python3
"""Run positive/negative inline-check fixtures with the real C++ dependencies."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import clang_tidy_runner as runner
import yaml


class ClangTidyChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workspace = runner.workspace_root(os.environ)
        cls.tools = runner.discover_tools(
            "report", platform_name=runner.sys.platform, environ=os.environ
        )
        entries = json.loads((cls.workspace / "compile_commands.json").read_text())
        cls.entry = next(
            entry
            for entry in entries
            if str(entry["file"]).endswith("src/backend/ports/result_test.cpp")
        )

    def assert_diagnostics(self, body: str, filename: str = "fixture.cpp") -> None:
        # Literal markers identify independently expected user-file locations.
        expected = set()
        for line, text in enumerate(body.splitlines(), 1):
            for severity in ("error", "warning"):
                marker = f"// {severity}: "
                if marker in text:
                    expected.add(
                        ("custom-fastecu-" + text.split(marker)[1], line, severity.title())
                    )
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
            result = subprocess.run(
                [
                    runner.sys.executable,
                    str(Path(__file__).with_name("clang_tidy_adapter.py")),
                    "--upstream",
                    self.tools.run_clang_tidy,
                    "--",
                    "-clang-tidy-binary",
                    self.tools.clang_tidy,
                    "-config-file",
                    str(self.workspace / ".clang-tidy"),
                    "-checks=-*,custom-fastecu-*",
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
            )
            # Advisory findings must succeed; existing error-level checks still fail.
            self.assertEqual(
                any(level == "Error" for _, _, level in expected),
                result.returncode != 0,
                result.stdout + result.stderr,
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
                    self.assertEqual([], message["Replacements"])
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
        self.assert_diagnostics("""#include <iterator>
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
""")

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
        )

    def test_first_party_header_filter_reports_actual_header_declarations(self) -> None:
        with tempfile.TemporaryDirectory(prefix="fastecu-header-fixture-") as directory:
            root = Path(directory)
            configuration = {
                "Checks": "-*,custom-fastecu-header-probe",
                "WarningsAsErrors": "*,-custom-fastecu-header-probe",
                "HeaderFilterRegex": runner.inline_header_filter(self.workspace),
                "CustomChecks": [
                    {
                        "Name": "fastecu-header-probe",
                        "Query": (
                            'match cxxRecordDecl(hasName("::fastecu::Error"), '
                            'isDefinition()).bind("header")'
                        ),
                        "Diagnostic": [
                            {
                                "BindName": "header",
                                "Message": "header coverage probe",
                                "Level": "Warning",
                            }
                        ],
                    }
                ],
            }
            config = root / "config.yaml"
            config.write_text(yaml.safe_dump(configuration))
            (root / "compile_commands.json").write_text(json.dumps([self.entry]))
            fixes = root / "fixes.yaml"
            result = subprocess.run(
                [
                    self.tools.clang_tidy,
                    "--experimental-custom-checks",
                    f"--config-file={config}",
                    "-extra-arg=-Wno-error",
                    "-p",
                    directory,
                    f"-export-fixes={fixes}",
                    self.entry["file"],
                ],
                capture_output=True,
                text=True,
                cwd=self.workspace,
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            diagnostics = (
                (yaml.safe_load(fixes.read_text()) or {}).get("Diagnostics", [])
                if fixes.exists()
                else []
            )
            self.assertEqual(1, len(diagnostics), result.stdout + result.stderr)
            message = diagnostics[0]["DiagnosticMessage"]
            self.assertEqual(
                self.workspace / "src/backend/ports/error.h",
                (self.workspace / message["FilePath"]).resolve(),
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


if __name__ == "__main__":
    unittest.main()
