# Definition Parser Deduplication Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task inline under the user's execution authorization. Steps use checkbox syntax.

**Goal:** Reuse existing EcuFlash/RomRaider parsing infrastructure for authoring headers and remove duplicated identity and metadata assembly without changing caller contracts.

**Architecture:** Extend the existing `parser_utils` layer beneath the three callers. Share XML loading, text reading, validated ROM identity extraction and metadata field names. Keep format selection, authoring normalization and error presentation in their current callers; do not route authoring through full table parsing.

**Tech Stack:** C++23, Bazel, GoogleTest, existing pugixml. No QtXml.

**Spec:** User request to deduplicate authoring header parsing with existing EcuFlash/RomRaider parsers; preserve the accepted item 2 contract in the [migration roadmap](2026-10-05-backend-migration.md) and [design notes](../../design-notes.md#portable-authoring-header-fields).

## Current overlap and differences

| Concern | Existing parsers | Authoring helper | Decision |
| --- | --- | --- | --- |
| XML loading | `parse_root` in `parser_utils`; auto-detect byte encoding | Direct `load_buffer`; input already UTF-8 | One loader; explicit encoding argument |
| Root selection | EcuFlash requires `rom`; RomRaider requires `roms` | Accepts `rom` or first direct `rom` in `roms` | Preserve separate selection policies |
| Text | `child_text`: first text child, then trim | Concatenate descendant text/CDATA, preserve whitespace | Share reader with two explicit text modes |
| Identity | `definition_id_for_rom` validates `romid` and singleton children; address/identity assembly repeated | Partial raw fields, first XML occurrence; last named form field wins | Share strict identity assembly between parsers; keep form policy separate |
| Metadata | `parse_metadata` shared by both full parsers | Same nine editable field names mapped again | One descriptor list for those nine names |
| Address conversion | `optional_hex_element` wraps shared `parse_hex_value` with parser context | Same hex core, plus leading plus and Unicode trimming | Keep the existing shared core; preserve wrappers |
| Parent/notes | EcuFlash `include` elements; RomRaider `base` attribute; metadata notes under `romid` | One `include` and root-level `notes` | Preserve locations and cardinality; do not conflate |

Existing index parsers are closer to header-only parsing than full parsers, but still require a valid identity, normalize text and reject duplicate identity fields. Reusing their returned entries directly would lose raw authoring metadata and make partial imports fail. Calling full parsers would also make unrelated invalid tables block header import.

## Global Constraints

- Execution authorized by the user. No work on migration items 3–10.
- Backend stays Qt-free, thread-free and free of direct filesystem I/O.
- No new runtime dependencies or ErrorKind values. No DTD expansion or Qt parser oracle.
- Keep public parser, writer, authoring helper and UI APIs stable.
- Preserve error messages/context, catalog ordering, matching and writer registration semantics.
- Preserve XML byte encoding auto-detection in file parsers and explicit UTF-8 in the form helper.
- DOM nodes are borrowed from a caller-owned `pugi::xml_document`; never retain them after that document dies.
- Do not widen root acceptance in strict parsers or change their existing multiple-root behavior as a side effect. Any XML validation tightening requires a separately reviewed behavioral change.

## Review Focus

- An invalid address in an unselected RomRaider record must not prevent loading a different valid record; identity validation of candidates must still occur.
- Raw form values, mixed text/CDATA, empty fields and duplicate handling must not inherit strict-parser normalization.
- Root notes and `romid` notes, EcuFlash repeated includes and RomRaider `base` must retain their current meanings.
- UTF-8 form input and encoded file bytes must not accidentally use the same encoding policy.
- Parser errors must retain source/definition context; form import failures still yield blanks.

## Files and ownership

- Modify `src/backend/definition/parser_utils.h/.cpp`: shared DOM loading/text primitives and validated ROM header extraction; existing error helpers remain here.
- Create `src/backend/definition/metadata_fields.h`: fixed descriptors for the nine editable metadata fields, not a general schema registry.
- Modify `src/backend/definition/ecuflash_parser.cpp` and `romraider_parser.cpp`: replace repeated strict identity assembly only; keep format-specific table, parent and selection logic.
- Modify `src/backend/definition/definition_header_fields.cpp`: use shared DOM primitives and metadata descriptors; keep authoring conversion policy.
- Tests stay co-located in `parser_utils_test.cpp`, `ecuflash_parser_test.cpp`, `romraider_parser_test.cpp` and `definition_header_fields_test.cpp`.
- Update `src/backend/definition/BUILD.bazel` through Gazelle and update ownership documentation. UI adapter source/API and writer serialization stay unchanged.

## Task 1: Share XML loading and text reading

**Interfaces in `parser_utils.h`:**

```cpp
enum class XmlTextMode { FirstText, DescendantText };
Result<pugi::xml_node> parse_document_root(
    pugi::xml_document& document, std::span<const std::uint8_t> xml,
    std::string_view source, pugi::xml_encoding encoding);
std::string read_element_text(pugi::xml_node element, XmlTextMode mode);
```

`parse_document_root` performs the existing pugixml parse with `parse_default`, returns the document element and uses existing parser-context errors for parse failures. It does not choose a definition format or add schema validation. `read_element_text` returns untrimmed text: `FirstText` retains existing `child_value()` semantics; `DescendantText` concatenates text/CDATA recursively in document order. Missing nodes produce an empty string.

- [x] Add tests for missing nodes, first-text versus descendant-text behavior, mixed CDATA, preserved whitespace, standard references, parse-error source context and explicit UTF-8 versus auto-detected encoded bytes. Run `bazel test --config=release //src/backend/definition:parser_utils_test` and observe the missing helper failures.
- [x] Implement both helpers. Make existing `parse_root` delegate loading with `encoding_auto` while preserving its expected-root checks and existing errors. Make existing `child_text` use `FirstText` followed by `trim_copy`.
- [x] Make `collect_ecuflash_base_header_fields` delegate loading with `encoding_utf8`, translating parse failures to blank fields. Retain its single-document-root check and explicit `rom`/`roms` selection. Use `DescendantText` and remove the local recursive reader. Convert the UTF-8 string view to a byte span locally without introducing an owning buffer or leaking pugixml through the public form API.
- [x] Regenerate BUILD files; add `parser_utils` as an implementation dependency of `definition_header_fields`. Run parser-utils, both parser, header-helper and form tests. Add characterization assertions for strict roots and authoring multiple-root rejection if coverage is absent.
- [x] Commit the shared DOM primitives and caller migration as one reviewable change.

## Task 2: Consolidate strict ROM identity assembly

**Interfaces in `parser_utils.h`:**

```cpp
struct ParsedRomHeader
{
    pugi::xml_node rom_id; // Borrowed from the caller's document.
    RomIdentity identity; // Owns its strings.
};
Result<ParsedRomHeader> parse_rom_header(pugi::xml_node rom,
                                        std::string_view source);
```

This helper calls existing `definition_id_for_rom` and `optional_hex_element`, then assembles `RomIdentity` using existing normalized text semantics. It does not read metadata, parents or tables. Missing optional addresses stay absent; present empty/invalid addresses retain their current errors.

- [x] Add parser-utils tests for valid identity, missing/duplicate `romid`, duplicate singleton children, trimmed identity values, absent versus empty/overflowing address, and unchanged error context. Run the target and observe the new helper failures before implementation.
- [x] Implement `parse_rom_header` using existing validators and hex parsing. Replace EcuFlash's private repeated header validation/identity assembly with it; retain the parsed root for table and parent processing.
- [x] Use the helper for every RomRaider index record and only for the selected ROM in full definition loading. Keep the candidate selection loop on `definition_id_for_rom`: eagerly parsing every candidate address would change existing behavior. Preserve duplicate requested IDs, unknown IDs and invalid candidate identity errors.
- [x] Add or strengthen a RomRaider regression: an unselected ROM with valid identity but invalid address does not block loading the requested valid ROM; indexing that same collection still fails. Keep duplicate requested identity and missing identity coverage.
- [x] Run parser-utils, EcuFlash, RomRaider, definition-service, catalog and writer tests; compare existing error assertions and index entries. Commit the strict identity deduplication.

## Task 3: Share the editable metadata field mapping

**Interface in new `metadata_fields.h`:**

```cpp
struct MetadataField
{
    std::string_view xml_name;
    std::string RomMetadata::*member;
};
// Inline constexpr std::array<MetadataField, 9> kEditableMetadataFields.
```

The exact descriptors are `make→make`, `market→market`, `model→model`, `submodel→submodel`, `transmission→transmission`, `year→year`, `flashmethod→flash_method`, `memmodel→memory_model`, `checksummodule→checksum_module`.

- [x] Pin all nine mappings with distinct fixture values through both strict parsing and form conversion. Also pin that strict `filesize` and `romid/notes` remain supported, while form metadata does not acquire those fields. Keep root-level form notes separate. Use existing golden tests where they already cover these contracts; extend only missing assertions.
- [x] Add the descriptors and use them in `parse_metadata` with `child_text`, then assign strict `file_size` and `notes` explicitly. Use the same descriptors in `definition_header_input` with its raw last-value lookup. Leave XML ID/address normalization and `include`/`notes` form mapping unchanged.
- [x] Regenerate BUILD files for the header-only target/dependencies. Run parser-utils, both parsers, header-helper and writer tests. Verify whitespace differs only where the existing contracts require it; compare full metadata objects and serialized writer results.
- [x] Commit the shared mapping. Do not extend this change into a generic field registry, writer rewrite or schema framework.

## Task 4: Verify and deliver the deduplicated implementation

- [x] Update [design notes](../../design-notes.md#portable-authoring-header-fields) to explain shared parser primitives and the retained caller policies. Record results in this plan.
- [x] Run `python3 scripts/gazelle_check.py --fix`, then `python3 scripts/gazelle_check.py` to prove stability. Run changed-file `prek` checks and `bazel run --config=release //:clang_tidy_report_changed` with the repository's LLVM toolchain.
- [x] Run `bazel build --config=release //:fastecu` and `bazel test --config=release //...`. Existing catalog, authoring dialog, MainWindow and composition targets must pass alongside portable parser tests; report platform skips.
- [x] Query `filter('^//src/platform/', deps(//src/backend/... + //src/algorithms/...))`; expect empty. Query QtXml reachability from the header-helper/form tests; expect empty. Run the Android gate if an NDK becomes available, otherwise retain the explicit limitation.
- [x] Review the scoped diff for changed root selection, trimming, notes/include mapping, error context and DOM lifetimes. Deliver one separate PR after #508, or update #508 if it remains open and the user chooses that integration. Preserve unrelated untracked files.

## Acceptance

The authoring helper contains no XML loading or recursive text parsing implementation of its own. Strict ROM identity extraction is defined once and reused by both parser entry-point families. The nine editable metadata names are declared once. Remaining differences are explicit caller policies with portable regression tests, not duplicated parser internals.


## Execution Results — 2026-10-05

- Executed inline using Superpowers, in the retained isolated worktree on
  `refactor/definition-parser-deduplication`, starting at `7e69ce3d` from #508.
- Baseline: five parser/header/form targets passed.
- Task 1: loading/text helper tests failed at link time before implementation;
  all five parser/header/form targets passed afterward. Strict-root and encoded
  byte input tests pin the retained caller policies.
- Task 2: strict ROM-header helper tests failed at link time before implementation;
  parser-utils, EcuFlash, RomRaider, definition service, catalog and writer
  targets passed afterward. Regressions cover unselected invalid addresses,
  duplicate requested IDs and unknown-ID error precedence.
- Task 3: metadata characterizations passed before and after deduplication.
  All five parser/header/writer targets passed; raw form metadata and strict
  notes/file-size handling remain distinct.
- Final release application build passed. Full release suite: 251 targets
  passed, seven Windows-only targets skipped; catalog integration, authoring
  dialog, MainWindow and desktop composition coverage passed.
- Gazelle regeneration/check and changed-file formatting passed. LLVM 23
  clang-tidy: eight translation units clean, zero findings.
- Portable backend/algorithm closure contains no platform targets. Header-helper
  and form-test closure contains no QtXml target.
- Fresh whole-branch review found no functional Critical/Important issues and
  no declined inputs. Two Result conventions were corrected: explicit
  `.has_value()` checks and diagnostic test matchers. Final release/static
  checks passed after those corrections.
- Ruling: applied the repository's mandatory Result conventions despite their
  minor review grade rather than deferring them as optional polish. Behavior
  is unchanged; the cost of this choice is additional stylistic review churn.
- Android gate was unavailable: `ANDROID_NDK_HOME` is unset and no NDK was found
  in the checked local SDK locations. Items 3–10 remain untouched; original
  untracked Sonar files remain untouched.

- Delivered as [PR #509](https://github.com/RcusStackwalker/FastECU/pull/509),
  stacked on the still-open #508. The worktree and branch are retained for review.
