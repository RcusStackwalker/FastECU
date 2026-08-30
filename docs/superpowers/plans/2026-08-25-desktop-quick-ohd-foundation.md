# Portable `.ohd` Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Qt-free, format-version-1 dashboard document model with strict validation, deterministic `.ohd` XML coding, atomic persistence, and all-or-nothing import of the bundled FastECU CDBG catalog.

**Architecture:** Put plain value types and pure transformations in `src/backend/dashboard`, with separate validator, v1 codec, legacy importer, and stateless persistence service. Reuse a newly exposed logging-expression validator, existing Colt CDBG protocol constants, portable file ports, and pugixml; expose no Qt, platform, UI, or legacy definitions dependency.

**Tech Stack:** C++23, `std::expected`, pugixml 1.15, GoogleTest, Bazel 9.1.1, Python portable-closure gate

**Spec:** `docs/superpowers/specs/2026-08-24-desktop-quick-ohd-foundation-design.md`

## Global Constraints

- Support exactly `<omnihaste-dashboard format-version="1">`; a well-formed numeric version other than 1 returns `ErrorKind::Unsupported`.
- Accept only protocol `cdbg`, transport `raw-can`, CAN identifier widths 11 and 29, and raw assembly `unsigned-integer-decimal`.
- Protocol fields use fixed-width integer types; IDs and descriptive fields use strings; closed value sets use enums.
- Unknown version-1 elements and attributes are errors; optionality is limited to metadata description, preferred adapter, card title, card gauge overrides, and an empty card list.
- Emit four-space indentation, UTF-8, one trailing newline, lowercase enum spellings, decimal ordinary integers, and canonical lowercase `0x` hexadecimal for identifiers and addresses.
- Import only exactly one legacy `<protocol id="CDBG">`; ignore other protocol sections and switches; never modify or automatically save the source catalog.
- Load through `IFileRepository`; save through `IAtomicFileWriter`; validate and encode fully before the first writer call.
- Keep `src/backend/dashboard` free of Qt, `src/backend/definitions`, desktop application, and platform-adapter dependencies.
- Add production targets and tests to `//:portable_closure` and its dependency scan.
- Do not add QML, controllers, dialogs, recent/dirty state, hardware discovery, sessions, live data, editing, migrations, or a version-0 API/fixture.

---

## File map

- Create `src/backend/dashboard/dashboard_document.h`: complete domain value types and closed enums.
- Create `src/backend/dashboard/dashboard_validation.{h,cpp}` and test: complete cross-field/domain validation.
- Create `src/backend/dashboard/dashboard_codec.{h,cpp}` and test: strict v1 XML decode/encode and version dispatch.
- Create `src/backend/dashboard/legacy_cdbg_catalog_importer.{h,cpp}` and test: strict one-time catalog conversion.
- Create `src/backend/dashboard/dashboard_document_service.{h,cpp}` and test: repository/writer orchestration.
- Create `src/backend/dashboard/test_fixtures.h`: canonical model/XML builders shared by dashboard tests.
- Create `src/backend/dashboard/BUILD.bazel`: focused portable libraries and tests.
- Modify `resources/shared/BUILD.bazel`: narrowly export the bundled CDBG catalog to dashboard tests.
- Modify `src/backend/logging/logging_session.{h,cpp}` and its test: expose the existing expression semantics as a reusable pure validator.
- Modify root `BUILD.bazel` and `scripts/check-portable-closure.py`: dashboard package and targets in portable gates.

### Task 1: Extract reusable logging conversion validation

**Files:**
- Modify: `src/backend/logging/logging_session.h`
- Modify: `src/backend/logging/logging_session.cpp`
- Modify: `src/backend/logging/logging_session_test.cpp`

**Interfaces:**
- Consumes: the existing private recursive-descent `ExpressionValidator` and existing session validation behavior.
- Produces: `bool fastecu::logging::valid_conversion_expression(std::string_view expression)`; `bool fastecu::logging::valid_display_precision(std::uint8_t precision)`.

- [ ] **Step 1: Add focused failing tests for the public semantics**

Add to `logging_session_test.cpp`:

```cpp
TEST(LoggingConversionValidation, AcceptsLegacyExpressionGrammar)
{
    EXPECT_TRUE(fastecu::logging::valid_conversion_expression("x*1000/256"));
    EXPECT_TRUE(fastecu::logging::valid_conversion_expression("-(x+1)"));
}

TEST(LoggingConversionValidation, RejectsMalformedOrNonFiniteExpression)
{
    EXPECT_FALSE(fastecu::logging::valid_conversion_expression(""));
    EXPECT_FALSE(fastecu::logging::valid_conversion_expression("x/0"));
    EXPECT_FALSE(fastecu::logging::valid_conversion_expression("x trailing"));
}

TEST(LoggingConversionValidation, AcceptsOnlySupportedDisplayPrecision)
{
    EXPECT_TRUE(fastecu::logging::valid_display_precision(0));
    EXPECT_TRUE(fastecu::logging::valid_display_precision(15));
    EXPECT_FALSE(fastecu::logging::valid_display_precision(16));
}
```

- [ ] **Step 2: Run the tests and verify the missing API fails compilation**

Run: `bazel test //src/backend/logging:logging_session_test --test_output=errors`

Expected: FAIL because `valid_conversion_expression` and `valid_display_precision` are not declared.

- [ ] **Step 3: Expose the existing expression check without duplicating it**

Add to `logging_session.h`:

```cpp
bool valid_conversion_expression(std::string_view expression);
bool valid_display_precision(std::uint8_t precision);
```

Add after the anonymous namespace in `logging_session.cpp`, delegating to the existing parser, and replace the session validator's direct checks with these functions:

```cpp
bool valid_conversion_expression(std::string_view expression)
{
    return ExpressionValidator(expression).valid();
}

bool valid_display_precision(std::uint8_t precision)
{
    return precision <= 15;
}
```

Keep the existing session error text and behavior unchanged.

- [ ] **Step 4: Run the focused and regression tests**

Run: `bazel test //src/backend/logging:logging_session_test //src/backend/logging:logging_conversion_test --test_output=errors`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/backend/logging/logging_session.h src/backend/logging/logging_session.cpp src/backend/logging/logging_session_test.cpp
git commit -m "refactor: expose logging conversion validation"
```

### Task 2: Define and validate the complete dashboard document

**Files:**
- Create: `src/backend/dashboard/BUILD.bazel`
- Create: `src/backend/dashboard/dashboard_document.h`
- Create: `src/backend/dashboard/dashboard_validation.h`
- Create: `src/backend/dashboard/dashboard_validation.cpp`
- Create: `src/backend/dashboard/test_fixtures.h`
- Create: `src/backend/dashboard/dashboard_validation_test.cpp`

**Interfaces:**
- Consumes: `fastecu::logging::valid_conversion_expression`, `valid_display_precision`, and `Result`/`Status`.
- Produces: value types below; `Status validate_dashboard_document(const DashboardDocument&)`.

- [ ] **Step 1: Add the value model and canonical test builder**

Create `dashboard_document.h` in namespace `fastecu::dashboard`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fastecu::dashboard
{
enum class DashboardProtocol { Cdbg };
enum class DashboardTransport { RawCan };
enum class CanIdentifierWidth : std::uint8_t { Standard = 11, Extended = 29 };
enum class RawAssembly { UnsignedIntegerDecimal };
enum class AdapterKind { J2534, SocketCan };
enum class CardDisplayType { Numeric, Sparkline, HorizontalGauge };

struct DocumentMetadata {
    std::uint32_t format_version{1};
    std::string name;
    std::optional<std::string> description;
    bool operator==(const DocumentMetadata&) const = default;
};
struct RetryPolicy {
    std::uint32_t poll_timeout_ms;
    std::uint32_t silence_threshold;
    std::uint32_t reconnect_attempts;
    std::uint32_t reconnect_period_ms;
    bool operator==(const RetryPolicy&) const = default;
};
struct PreferredAdapter {
    AdapterKind kind;
    std::string vendor;
    std::string display_name;
    bool operator==(const PreferredAdapter&) const = default;
};
struct CdbgConnectionProfile {
    DashboardProtocol protocol{DashboardProtocol::Cdbg};
    DashboardTransport transport{DashboardTransport::RawCan};
    std::uint32_t bitrate;
    CanIdentifierWidth identifier_width;
    std::uint32_t request_id;
    std::uint32_t reply_id;
    std::uint8_t stream_instance;
    std::uint32_t sampling_interval_ms;
    RetryPolicy retry;
    std::optional<PreferredAdapter> preferred_adapter;
    bool operator==(const CdbgConnectionProfile&) const = default;
};
struct DashboardConversion {
    std::string id;
    std::string expression;
    std::string unit;
    std::uint8_t precision;
    double gauge_min;
    double gauge_max;
    double gauge_step;
    bool operator==(const DashboardConversion&) const = default;
};
struct DashboardChannel {
    std::string id;
    std::string name;
    std::string description;
    std::uint32_t address;
    std::uint8_t length;
    RawAssembly raw_assembly{RawAssembly::UnsignedIntegerDecimal};
    std::vector<DashboardConversion> conversions;
    bool operator==(const DashboardChannel&) const = default;
};
struct GaugeBoundsOverride {
    double minimum;
    double maximum;
    double step;
    bool operator==(const GaugeBoundsOverride&) const = default;
};
struct DashboardCard {
    std::string id;
    std::string channel_id;
    std::string conversion_id;
    CardDisplayType display_type;
    std::optional<std::string> title;
    std::uint32_t order;
    std::optional<GaugeBoundsOverride> gauge_bounds;
    std::optional<std::uint16_t> sparkline_history_seconds;
    bool operator==(const DashboardCard&) const = default;
};
struct DashboardDocument {
    DocumentMetadata metadata;
    CdbgConnectionProfile connection;
    std::vector<DashboardChannel> channels;
    std::vector<DashboardCard> cards;
    bool operator==(const DashboardDocument&) const = default;
};
} // namespace fastecu::dashboard
```

Create `test_fixtures.h` with inline `valid_document()` returning the canonical Colt example from the spec: version/name/description, 500000 bitrate, standard IDs `0x630/0x631`, stream 0, 50 ms sampling, retry `100/3/3/250`, one RPM channel/conversion, and no cards/adapter.

- [ ] **Step 2: Add table-driven failing validation tests**

Declare in `dashboard_validation.h`:

```cpp
Status validate_dashboard_document(const DashboardDocument& document);
```

In `dashboard_validation_test.cpp`, use a helper that checks both kind and stable path prefix:

```cpp
void expect_invalid(const DashboardDocument& document, std::string_view path)
{
    const auto result = validate_dashboard_document(document);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(result.error().detail.starts_with(path)) << result.error().detail;
}
```

Add separate tests covering: format version; empty metadata name; zero bitrate; invalid enum values via `static_cast`; zero sampling, poll timeout, silence threshold, reconnect attempts, and reconnect period; standard ID above `0x7ff`; extended ID above `0x1fffffff`; duplicate request/reply IDs; empty adapter vendor/display name; empty/duplicate channel IDs; length 0, 3, and 8; invalid raw assembly; no conversion; empty/duplicate conversion ID; malformed expression and precision 16; NaN/infinite gauge values, min equal/above max, and nonpositive step; empty/duplicate card IDs; duplicate/noncontiguous order; two cards for one channel; missing channel/conversion references; numeric card with gauge or history; sparkline without history or with gauge; history 0 and 301; horizontal gauge without bounds; invalid override bounds. Assert the exact prefixes defined below.

Also add positive tests for empty cards, absent adapter, both identifier widths at their inclusive maxima, and sparkline histories 1 and 300.

- [ ] **Step 3: Run validation tests and verify linkage fails**

Run: `bazel test //src/backend/dashboard:dashboard_validation_test --test_output=errors`

Expected: FAIL because `validate_dashboard_document` has no implementation.

- [ ] **Step 4: Implement first-error validation with stable paths**

Implement helpers in `dashboard_validation.cpp`:

```cpp
Status invalid(std::string path, std::string explanation)
{
    return fail(ErrorKind::InvalidConfig, std::move(path) + ": " + std::move(explanation));
}

Status validate_gauge(double minimum, double maximum, double step, std::string path)
{
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(step))
        return invalid(std::move(path), "values must be finite");
    if (minimum >= maximum)
        return invalid(std::move(path), "minimum must be less than maximum");
    if (step <= 0.0)
        return invalid(std::move(path), "step must be positive");
    return {};
}
```

Validate in document order and return immediately on failure. Use these exact paths: `metadata.format-version`, `metadata.name`, `connection.protocol`, `connection.transport`, `connection.bitrate`, `connection.identifier-width`, `connection.request-id`, `connection.reply-id`, `connection.stream-instance`, `connection.sampling-interval-ms`, `connection.retry.poll-timeout-ms`, `connection.retry.silence-threshold`, `connection.retry.reconnect-attempts`, `connection.retry.reconnect-period-ms`, `connection.preferred-adapter.kind|vendor|display-name`, `channels[<id-or-index>]`, `channels[<id>].id|address|length|raw-assembly`, `channels[<id>].conversions`, `channels[<id>].conversions[<id-or-index>].id|expression|precision|gauge`, and `cards[<id-or-index>].id|channel-id|conversion-id|display-type|order|gauge|sparkline-history-seconds`.

Enforce `format_version == 1`; all required strings nonempty; all operational integer fields positive except stream instance; retry period positive; request/reply unequal and within width; stream instance fits its fixed-width type; channel address is already bounded by `uint32_t`; length in `{1,2,4}`; unique IDs; conversions nonempty; logging expression/precision semantics; gauge invariants; card references and uniqueness; exact contiguous order set `0..cards.size()-1`; one card/channel; and display-specific optionals exactly as tested.

- [ ] **Step 5: Add focused Bazel targets and run them**

Create `src/backend/dashboard/BUILD.bazel` with public `dashboard_document`, `dashboard_validation`, and `fastecu_portable_gtest(name = "dashboard_validation_test", ...)`. `dashboard_validation` depends only on `//src/backend/logging:logging_session` and `//src/backend/ports`.

Run: `bazel test //src/backend/dashboard:dashboard_validation_test --test_output=errors`

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/backend/dashboard src/backend/logging/BUILD.bazel
git commit -m "feat: add validated dashboard document model"
```

### Task 3: Add strict versioned `.ohd` decoding and deterministic encoding

**Files:**
- Create: `src/backend/dashboard/dashboard_codec.h`
- Create: `src/backend/dashboard/dashboard_codec.cpp`
- Create: `src/backend/dashboard/dashboard_codec_test.cpp`
- Modify: `src/backend/dashboard/BUILD.bazel`
- Modify: `resources/shared/BUILD.bazel`

**Interfaces:**
- Consumes: `DashboardDocument`, `validate_dashboard_document`, pugixml, `bytes::ByteView`.
- Produces: `Result<DashboardDocument> decode_dashboard_document(bytes::ByteView xml)`; `Result<std::vector<std::uint8_t>> encode_dashboard_document(const DashboardDocument&)`.

- [ ] **Step 1: Add canonical decode, encode, and round-trip tests**

Declare the two APIs above. In `dashboard_codec_test.cpp`, define the spec XML verbatim as `kCanonicalXml`, then assert:

```cpp
const auto decoded = decode_dashboard_document(bytes::as_byte_view(kCanonicalXml));
ASSERT_TRUE(decoded);
EXPECT_EQ(*decoded, test::valid_document());

const auto encoded = encode_dashboard_document(test::valid_document());
ASSERT_TRUE(encoded);
EXPECT_EQ(std::string(encoded->begin(), encoded->end()), kCanonicalXml);

const auto reparsed = decode_dashboard_document(*encoded);
ASSERT_TRUE(reparsed);
EXPECT_EQ(*reparsed, test::valid_document());
EXPECT_EQ(encode_dashboard_document(*reparsed), encoded);
```

The canonical string must include the XML declaration, exactly four-space indentation, `<cards/>`, and one final newline.

- [ ] **Step 2: Add strict schema and version tests**

Add small inline fixtures proving: malformed XML, missing root, wrong root, absent/non-numeric `format-version`, missing each required section/attribute, duplicate singleton sections, unknown element at every nesting level, and unknown attribute at every element return `InvalidConfig` with the relevant stable path. Add `<omnihaste-dashboard format-version="2"/>` expecting `Unsupported`, and prove calling encode on a document with version 2 returns `InvalidConfig`.

Add parsing tests for both adapter kinds, all three card types, escaped UTF-8 strings, standard/extended identifiers, decimal integers only where required, canonical/lowercase hex acceptance for IDs/addresses, overflow, signed text, trailing junk, nonfinite floating text, and invalid enum spellings.

- [ ] **Step 3: Run codec tests and verify linkage fails**

Run: `bazel test //src/backend/dashboard:dashboard_codec_test --test_output=errors`

Expected: FAIL because codec functions are undefined.

- [ ] **Step 4: Implement strict root/version dispatch and v1 decoder**

Use an explicit dispatch shape:

```cpp
Result<DashboardDocument> decode_dashboard_document(bytes::ByteView xml)
{
    pugi::xml_document tree;
    const auto parsed = tree.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed) return fail(ErrorKind::InvalidConfig, std::format("document: {} at offset {}", parsed.description(), parsed.offset));
    const pugi::xml_node root = tree.document_element();
    if (std::string_view(root.name()) != "omnihaste-dashboard")
        return fail(ErrorKind::InvalidConfig, "document.root: expected <omnihaste-dashboard>");
    const auto version = parse_required_u32(root, "format-version", "metadata.format-version", 10);
    if (!version) return std::unexpected(version.error());
    if (*version != 1)
        return fail(ErrorKind::Unsupported, std::format("metadata.format-version: unsupported version {}", *version));
    return decode_v1(root);
}
```

Implement reusable `reject_unknown_attributes(node, allowed, path)`, `require_single_child`, required/optional string parsing, `std::from_chars` unsigned parsing with full-consumption/overflow checks, finite `std::from_chars` double parsing, lowercase enum maps, and hex parsing requiring `0x` followed by hex digits. `decode_v1` must construct a candidate only after every field parses, then return `validate_dashboard_document(candidate)` or the complete candidate.

Use a documented v1 card XML shape: `<card id="..." channel-id="..." conversion-id="..." display-type="numeric|sparkline|horizontal-gauge" order="N" title="..." sparkline-history-seconds="N" gauge-min="..." gauge-max="..." gauge-step="..."/>`; require all three gauge attributes together and let complete validation enforce display applicability.

- [ ] **Step 5: Implement deterministic v1 encoding**

Validate first. Build nodes in this exact order: declaration, root, metadata, connection attributes, retry, optional preferred-adapter, channels and source-order conversions, cards in ascending `order`. Add attributes in the order shown in the spec; for cards use `id`, `channel-id`, `conversion-id`, `display-type`, optional `title`, `order`, optional gauge triplet, optional history.

Use `std::to_chars`/`std::format` helpers for locale-independent shortest round-trippable finite doubles and lowercase `0x{:x}` hex. Serialize with:

```cpp
std::ostringstream output;
tree.save(output, "    ", pugi::format_default, pugi::encoding_utf8);
std::string text = output.str();
if (!text.ends_with('\n')) text.push_back('\n');
return std::vector<std::uint8_t>(text.begin(), text.end());
```

Never emit optional attributes when their optionals are absent.

- [ ] **Step 6: Wire and run codec plus validation tests**

Add `dashboard_codec` and `dashboard_codec_test` targets; production deps are `:dashboard_document`, `:dashboard_validation`, `//src/algorithms/protocol`, `//src/backend/ports`, and `@pugixml`.

Run: `bazel test //src/backend/dashboard:dashboard_codec_test //src/backend/dashboard:dashboard_validation_test --test_output=errors`

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/backend/dashboard
git commit -m "feat: add strict ohd version one codec"
```

### Task 4: Import the legacy FastECU CDBG catalog atomically

**Files:**
- Create: `src/backend/dashboard/legacy_cdbg_catalog_importer.h`
- Create: `src/backend/dashboard/legacy_cdbg_catalog_importer.cpp`
- Create: `src/backend/dashboard/legacy_cdbg_catalog_importer_test.cpp`
- Modify: `src/backend/dashboard/BUILD.bazel`

**Interfaces:**
- Consumes: legacy logger-shaped XML bytes, Colt `kRequestCanId`/`kReplyCanId`, dashboard validation, and logging conversion semantics.
- Produces: `LegacyCdbgImportDefaults`; `Result<DashboardDocument> import_legacy_cdbg_catalog(bytes::ByteView, const LegacyCdbgImportDefaults&)`.

- [ ] **Step 1: Define explicit caller defaults and import contract tests**

Create the header:

```cpp
struct LegacyCdbgImportDefaults {
    std::string document_name;
    std::uint32_t bitrate;
    CanIdentifierWidth identifier_width;
    std::uint8_t stream_instance;
    std::uint32_t sampling_interval_ms;
    RetryPolicy retry;
};

Result<DashboardDocument> import_legacy_cdbg_catalog(
    bytes::ByteView xml, const LegacyCdbgImportDefaults& defaults);
```

In the test, load `resources/shared/config/logger_cdbg_example.xml` as Bazel test data and assert all four channels, source order, all text/address/length/conversion fields, `conversion-1` IDs, precision derived from legacy `format` (`"0" -> 0`, `"0.0" -> 1`), explicit Colt IDs, supplied defaults, no cards, and no adapter. Include a fixture containing another protocol and CDBG switches and assert they are ignored.

- [ ] **Step 2: Add all-or-nothing failure tests**

For each inline fixture assert `!result`, `InvalidConfig`, a stable path, and therefore no `DashboardDocument`: malformed/wrong root; zero or two CDBG protocol sections; no CDBG parameters; missing/`No id`/empty ID; duplicate IDs; missing/overflow address; length outside 1/2/4; missing conversions; invalid expression; unsupported precision format (anything except `0` followed by zero or more `.0` digits); missing/nonfinite/invalid gauge fields; unknown child in the selected CDBG protocol/parameter/conversion; and defaults rejected by complete document validation.

- [ ] **Step 3: Run importer tests and verify linkage fails**

Run: `bazel test //src/backend/dashboard:legacy_cdbg_catalog_importer_test --test_output=errors`

Expected: FAIL because the importer is undefined.

- [ ] **Step 4: Implement strict selected-section conversion**

Parse with pugixml, require `<logger><protocols>`, scan direct protocol children, and count `id="CDBG"` exactly. Ignore non-CDBG protocol subtrees and `<switches>` without interpreting them. Strictly reject unknown content within the selected section except `parameters` and `switches`; within imported parameters allow only the legacy attributes `id/name/desc/length/enabled`, plus one `address` and one `conversions`; within conversions allow only `units/expr/format/gauge_min/gauge_max/gauge_step`.

Construct locally, using:

```cpp
DashboardDocument candidate{
    .metadata = {.format_version = 1, .name = defaults.document_name},
    .connection = {
        .protocol = DashboardProtocol::Cdbg,
        .transport = DashboardTransport::RawCan,
        .bitrate = defaults.bitrate,
        .identifier_width = defaults.identifier_width,
        .request_id = MitsuColtCanCdbg::kRequestCanId,
        .reply_id = MitsuColtCanCdbg::kReplyCanId,
        .stream_instance = defaults.stream_instance,
        .sampling_interval_ms = defaults.sampling_interval_ms,
        .retry = defaults.retry,
        .preferred_adapter = std::nullopt,
    },
    .channels = std::move(channels),
    .cards = {},
};
```

Map each conversion in source order to `conversion-<one-based-index>`, map absent legacy unit to `#` and expression to `x` only if those defaults match existing parser semantics, but require every gauge field. Parse addresses as bounded hexadecimal, lengths as decimal, and derive precision only from the strict legacy format grammar. Return the candidate only after `validate_dashboard_document(candidate)` succeeds.

- [ ] **Step 5: Wire data and run importer regression tests**

Add `config/logger_cdbg_example.xml` to the existing `exports_files` in `resources/shared/BUILD.bazel`, granting `//src/backend/dashboard:__pkg__` visibility. Add importer library/test targets and `//resources/shared:config/logger_cdbg_example.xml` to the test's `data`; set `CDBG_CATALOG_PATH` with `$(location //resources/shared:config/logger_cdbg_example.xml)` and read that environment path in the integration test.

Run: `bazel test //src/backend/dashboard:legacy_cdbg_catalog_importer_test --test_output=errors`

Expected: PASS and the bundled catalog imports four channels.

- [ ] **Step 6: Commit**

```bash
git add src/backend/dashboard resources/shared/BUILD.bazel
git commit -m "feat: import legacy CDBG catalog into ohd model"
```

### Task 5: Add the stateless persistence service

**Files:**
- Create: `src/backend/dashboard/dashboard_document_service.h`
- Create: `src/backend/dashboard/dashboard_document_service.cpp`
- Create: `src/backend/dashboard/dashboard_document_service_test.cpp`
- Modify: `src/backend/dashboard/BUILD.bazel`

**Interfaces:**
- Consumes: `IFileRepository`, `IAtomicFileWriter`, codec, importer, and `LegacyCdbgImportDefaults`.
- Produces: `DashboardDocumentService::load`, `save`, and `import_legacy_cdbg_catalog`; no retained document/path state.

- [ ] **Step 1: Define the service and failing orchestration tests**

Create:

```cpp
class DashboardDocumentService {
  public:
    DashboardDocumentService(IFileRepository& repository, IAtomicFileWriter& writer)
        : repository_(repository), writer_(writer) {}
    Result<DashboardDocument> load(std::string_view handle) const;
    Status save(std::string_view handle, const DashboardDocument& document) const;
    Result<DashboardDocument> import_legacy_cdbg_catalog(
        std::string_view handle, const LegacyCdbgImportDefaults& defaults) const;
  private:
    IFileRepository& repository_;
    IAtomicFileWriter& writer_;
};
```

Using `InMemoryFileRepository` and `InMemoryAtomicFileWriter`, test: load reads once and returns the decoded model; malformed/invalid load returns no model; unsupported version passes through; repository errors compare exactly; save emits canonical bytes in one replace call; invalid save makes zero replace calls; replacement errors compare exactly and do not update the fake destination; import reads once and returns an unsaved model with zero replace calls; import parse/validation/read failures return no model and zero replace calls.

- [ ] **Step 2: Run service tests and verify linkage fails**

Run: `bazel test //src/backend/dashboard:dashboard_document_service_test --test_output=errors`

Expected: FAIL because service methods are undefined.

- [ ] **Step 3: Implement thin, stateless orchestration**

```cpp
Result<DashboardDocument> DashboardDocumentService::load(std::string_view handle) const
{
    auto bytes = repository_.read(handle);
    if (!bytes) return std::unexpected(bytes.error());
    return decode_dashboard_document(*bytes);
}

Status DashboardDocumentService::save(std::string_view handle, const DashboardDocument& document) const
{
    auto bytes = encode_dashboard_document(document);
    if (!bytes) return std::unexpected(bytes.error());
    return writer_.replace(handle, *bytes);
}

Result<DashboardDocument> DashboardDocumentService::import_legacy_cdbg_catalog(
    std::string_view handle, const LegacyCdbgImportDefaults& defaults) const
{
    auto bytes = repository_.read(handle);
    if (!bytes) return std::unexpected(bytes.error());
    return dashboard::import_legacy_cdbg_catalog(*bytes, defaults);
}
```

- [ ] **Step 4: Wire and run all dashboard tests**

Add production and test targets with only dashboard, ports, and ports-testing dependencies.

Run: `bazel test //src/backend/dashboard:all --test_output=errors`

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/backend/dashboard
git commit -m "feat: add atomic dashboard document service"
```

### Task 6: Enforce portable closure and complete acceptance verification

**Files:**
- Modify: `BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`
- Modify: `src/backend/dashboard/BUILD.bazel`

**Interfaces:**
- Consumes: all dashboard production/test Bazel targets.
- Produces: package inclusion in source scanning and resolved dependency closure, with no Qt/JNI/platform reachability.

- [ ] **Step 1: Add the dashboard package to both portable checks**

Add to `PORTABLE_ROOTS`:

```python
ROOT / "src/backend/dashboard": {
    "dashboard_document",
    "dashboard_validation",
    "dashboard_codec",
    "legacy_cdbg_catalog_importer",
    "dashboard_document_service",
},
```

Add `//src/backend/dashboard:BUILD.bazel` to `portable_closure.data`.
Add all five production labels plus all four dashboard test labels to both the `expression` label set and matching `scope` in root `BUILD.bazel`. This makes compilation and transitive platform reachability part of the gate:

```starlark
"//src/backend/dashboard:dashboard_document",
"//src/backend/dashboard:dashboard_validation",
"//src/backend/dashboard:dashboard_codec",
"//src/backend/dashboard:legacy_cdbg_catalog_importer",
"//src/backend/dashboard:dashboard_document_service",
"//src/backend/dashboard:dashboard_validation_test",
"//src/backend/dashboard:dashboard_codec_test",
"//src/backend/dashboard:legacy_cdbg_catalog_importer_test",
"//src/backend/dashboard:dashboard_document_service_test",
```

Ensure `exports_files(["BUILD.bazel"], visibility = ["//:__pkg__"])` exists in the dashboard BUILD file.

- [ ] **Step 2: Run the portable gate by itself**

Run: `bazel test //:portable_closure --test_output=errors`

Expected: PASS and report all five dashboard production targets as required; the resolved closure contains no platform labels.

- [ ] **Step 3: Run the full acceptance suite**

Run:

```bash
bazel test //src/backend/dashboard:all //src/backend/logging:logging_session_test //:portable_closure --test_output=errors
bazel build //:fastecu //:fastecu-desktop-quick
prek run --all-files
git diff --check
```

Expected: every test/build/hook passes; `portable_closure` reports dashboard targets among required portable targets and no Qt/JNI/platform dependency; both desktop applications remain buildable; diff check is silent.

- [ ] **Step 4: Manually confirm delivery boundaries**

Run:

```bash
rg -n "Qt|src/backend/definitions|src/platform|apps/desktop" src/backend/dashboard
rg -n "placeholder|version.?0|migration" src/backend/dashboard
```

Expected: the first command finds no production dependency/include (mentions inside negative test names are acceptable); the second finds no placeholders or fictional migration API.

- [ ] **Step 5: Commit**

```bash
git add BUILD.bazel scripts/check-portable-closure.py src/backend/dashboard/BUILD.bazel
git commit -m "build: cover dashboard documents in portable closure"
```
