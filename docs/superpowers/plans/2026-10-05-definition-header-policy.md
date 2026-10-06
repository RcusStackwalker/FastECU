# Definition Header Policy Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task. Executed on PR #508; the implementation and review fixes are published at commit 3bff23f0.

**Goal:** Revise PR #508 to share definition-header machinery under a permissive-read, canonical-write policy grounded in RomRaider and EcuFlash.

**Architecture:** Portable helpers own XML text extraction, header normalization, numeric conversion, and metadata mappings. Draft imports permit absent fields; validated loading and writing add required-field checks. Qt adapters own widget access, field presentation, and error display.

**Tech Stack:** C++23, pugixml, Qt desktop adapters, Bazel, GoogleTest.

**Spec:** The review conversation for PR #508, including the user's instructions that RomRaider/EcuFlash are authoritative, reading should be permissive, writing decisive, and uint64 addresses are acceptable. Unsettled choices below are explicit recommendations.

## Global Constraints

- Start implementation from PR #508, reviewed at commit 629249c84d83c7b5cd914b3b6507e8f54968f898; verify its current head before editing.
- Existing FastECU behaviour is not a compatibility requirement.
- Retain portable backend ownership and removal of QtXml from the form dependency closure.
- Accept uint64 addresses, including UINT64_MAX; reject overflow, negatives, and trailing junk.
- No new dependencies, parser framework, XML traversal modes, or ErrorKind values.
- Preserve internal spaces in scalar strings and formatting in free-form notes.
- Restrict normalization changes to definition headers; audit shared helpers before changing unrelated table or protocol parsing.

## Proposed Behaviour

- Concatenate direct PCDATA and CDATA children in document order; ignore comments and processing instructions. Remove first-text and recursive-descendant modes.
- Recommendation: reject nested elements in scalar header fields with contextual InvalidConfig errors. Do not silently flatten nested markup or truncate its enclosing field.
- Trim surrounding Unicode White_Space in header identifiers, include references, and scalar metadata. Do not collapse internal whitespace or trim notes.
- Apply the same normalization to imported headers and submitted form values; writer entry points also enforce it for non-UI callers.
- Parse addresses as hexadecimal with optional + and 0x/0X, allowing surrounding whitespace, requiring complete consumption, and accepting uint64 bounds.
- Missing or whitespace-only address means absent on read/draft conversion; write absence by removing the element, never as an empty element or placeholder zero.
- Missing identity fields are allowed for draft imports. Retain current required-field checks for loaded identities and writer submission; do not expand required identities in this change.
- Malformed XML and unsupported roots return errors. Missing fields in a valid partial header remain empty. UI displays errors without replacing the existing form values.
- Import root policy: rom, or first direct rom within roms. No arbitrary wrapper search. Keep format-specific root requirements for strict loaders.
- Keep ROM include/notes separate from romid metadata notes; retain filesize as non-editable metadata where currently supported.
- Retain contextual strict-parser duplicate detection and RomRaider selection timing as explicit FastECU operational decisions for this bounded change. Neither is documented as authoritative RomRaider behaviour.
- Requested presentation order belongs to the form adapter. Duplicate widget names/order guarantees are not backend business contracts.

## Review Focus

- Direct text split by comments must concatenate completely; tests must retain comments in the DOM to exercise this.
- Notes must retain leading/trailing spaces and line breaks in extraction and serialization.
- Writer calls that bypass the UI must still normalize and validate headers.
- Failed imports must leave the visible form unchanged and report the error.
- Changing shared text/numeric helpers must not silently alter table parsing or flash-plan callers.

### Task 1: Share header text normalization and numeric conversion

**Files:** parser_utils.{h,cpp}, text_format.h, definition_header_fields.{h,cpp}, metadata_fields.h under src/backend/definition; associated parser/header tests and BUILD.bazel.

**Interfaces:** Replace XmlTextMode with one direct-text operation. Provide one reusable UTF-8 surrounding-whitespace helper and one header-address conversion operation returning Result<optional<uint64_t>>. Keep parser error context in wrappers. Reuse kEditableMetadataFields.

- [x] Add failing cases for comment-separated text, CDATA, proposed nested-element errors, Unicode-padded scalar fields, untouched notes, +0X10, UINT64_MAX, blank address, overflow, negatives, embedded whitespace, and trailing junk.
- [x] Run the targeted portable tests and confirm each new policy assertion fails for the intended reason.
- [x] Implement direct text collection and shared header normalization. Move the existing whitespace implementation to one shared location rather than adding a second table. Validate scalar structure before extracting values.
- [x] Consolidate address conversion; audit parse_hex_value callers before deciding whether to change that existing primitive or add a narrowly scoped header wrapper. Avoid duplicated from_chars implementations.
- [x] Replace form-only trimming and numeric special cases with those helpers; use the same scalar normalization for draft conversion and strict header loading.
- [x] Run portable tests; verify unrelated parser/flash-plan numeric contracts if the common primitive changed.
- [x] Commit the independently tested shared policy changes on the PR branch.

### Task 2: Integrate loaders and draft import without parallel XML semantics

**Files:** ecuflash_parser.cpp, romraider_parser.cpp, parser_utils.{h,cpp}, definition_header_fields.{h,cpp}; src/ui/desktop/definition/definition_header_form.{h,cpp}; dialog/definition_authoring_dialog.cpp; corresponding tests and BUILD.bazel files.

**Interfaces:** Change portable import to Result<DefinitionHeaderFields> and propagate failure through the Qt adapter. Keep named fields as a boundary representation; narrow supported names to the existing authoring fields. Preserve parse_rom_header for validated identity assembly.

- [x] Add failing integration cases: equivalent header values through import and strict loading, partial draft success, malformed/unsupported import failure, valid root selection, and no form mutation on import failure.
- [x] Run the affected portable/form/dialog tests and confirm the new failures.
- [x] Route EcuFlash, RomRaider, and draft extraction through the shared text/address rules. Share primitives and mappings without adding validation-mode flags or parsing a draft's tables.
- [x] Return parse/root errors instead of blank fields. Populate widgets only after successful extraction; display the existing Result error in the authoring dialog.
- [x] Keep field order/presentation in the adapter. Remove tests that elevate duplicate requested names or duplicate widgets into backend policy; do not add a new map abstraction merely to replace the pair vector.
- [x] Keep source-byte encoding autodetection and explicit UTF-8 for already decoded form text; these are input representations, not competing business rules.
- [x] Run parser, header, form, and authoring-dialog tests; commit.

### Task 3: Enforce canonical headers at writer entry points

**Files:** src/backend/definition/definition_writer.cpp, definition_writer_test.cpp, definition_service_test.cpp; metadata_fields.h and BUILD.bazel if needed.

**Interfaces:** Preserve create_ecuflash_xml and rewrite_ecuflash_xml signatures. Normalize a local header value before required-field validation and writing; do not mutate caller input.

- [x] Add failing create/rewrite tests for Unicode-padded identities and scalar metadata, unchanged notes/internal spaces, whitespace-only required fields, absent-address removal, canonical 0x lowercase hexadecimal, UINT64_MAX, duplicate text replacement, and parse/write/read stability.
- [x] Run writer tests and confirm the new assertions fail.
- [x] Reuse the shared normalization and metadata mapping. Canonicalize every caller, including direct service calls; validate the canonical values before mutating XML.
- [x] Write one text value per scalar, collapse duplicate writable field elements, retain the existing optional-address removal, and preserve unrelated tables/comments during rewriting.
- [x] Retain UTF-8 XML declaration, escaping, and generated-definition validation. Preserve separate root notes and metadata notes.
- [x] Run writer/service tests plus portable parser tests; commit.

### Task 4: Replace compatibility claims and verify the whole change

**Files:** docs/design-notes.md; affected tests, BUILD.bazel files, and PR description.

- [x] Replace Qt-compatibility claims with the agreed read/write contract and local RomRaider/EcuFlash evidence. Explicitly identify uint64 acceptance and other deliberate application policies.
- [x] Remove obsolete first-text/descendant-mode and raw-form-versus-normalized-parser tests; retain meaningful encoding, partial-input, contextual-error, selection, and duplicate regressions.
- [x] Run: bazel test --config=release //src/backend/definition/... //src/ui/desktop/definition/...
- [x] Run the repository's release application build and full release suite; distinguish actual results from the original PR's reported results.
- [x] Run applicable formatting, Gazelle, changed-file clang-tidy, and dependency checks using the repository's existing commands. Verify portable backend and form tests retain their intended dependency closures.
- [x] Inspect the final diff: there must be one header normalization/address policy, one direct-text rule, no XmlTextMode, no recursive scalar text traversal, and no silent malformed-import fallback.
- [x] Record production/test/build line changes separately; explain any remaining growth by necessary behaviour rather than preservation of old FastECU quirks.
- [x] Update the PR description around the final behaviour and verified checks; commit documentation with the final cleanup.

## Completion Criteria

Equivalent scalar header values normalize identically through import, loading, and writing; partial drafts remain editable; malformed imports report errors; writes are canonical and preserve notes and unrelated XML content; uint64 boundaries work; all relevant verification passes. EcuFlash nested-text and DTD behaviour remain unverified and must not be claimed as reverse-engineered facts.


## Execution Results

Published five commits into the existing PR #508 branch, ending at
`3bff23f0b8875dd95e51cf196d8b26eaa7e9ff0a`. The PR remains open; its title and
description describe the final read/write policy. The original checkout's
unrelated files and the existing PR worktree were preserved.

Local verification: full release build passed; full release suite passed all 251
applicable targets (seven Windows-only targets skipped); changed-file clang-tidy
reported 17 files clean, zero findings; formatting, Gazelle, Buildifier, and
portable/header/form dependency checks passed. Android compilation was not run
locally because no NDK is configured; the GitHub Android gate passed.

Independent review found three Important issues, all reproduced with failing
tests and then fixed: whitespace-only PCDATA hiding table values, rich-text
interpretation of literal notes, and premature UTF-8 decoding of source files.
No Critical/Minor findings or declined behaviours remain. All four reproducing
targets and the full suite pass after the fixes.

### Rulings Made During Execution

- Reuse the clean existing PR worktree and execute inline: protects unrelated
  master-checkout work. Risk if wrong: modifying another task's checkout; the
  initial clean status and exact PR head were verified.
- Keep generic numeric parsing unchanged and scope normalization to headers:
  avoids changing flash-plan/protocol contracts. Risk if wrong: inconsistent
  header paths; import/loader/writer integration tests cover those paths.
- Place shared input normalization with the writer: avoids a writer-to-form
  dependency cycle. Risk if wrong: normalization might be bypassed; both writer
  entry points enforce it and direct-caller tests cover them.
- Test malformed import as opening no editable dialog: the actual flow creates
  a fresh dialog, rather than modifying an existing form. Risk if wrong: an
  unseen mutation; tests also assert no write or registration side effect.
- Extend canonical writing to catalog registration: immediate lookup must use
  the ID emitted in XML. Risk if omitted: newly authored definitions become
  unreachable; direct create/import catalog regression tests cover both paths.
- Preserve table text selection with a dedicated helper while retaining all
  header whitespace: keeps notes complete without rewriting unrelated DOM
  content. Risk if wrong: blank descriptions/static data; both format parser
  regressions reproduce and guard that failure.

### Deferred Minors

None.

### Remaining CI State at Publication

Windows clang-tidy failed downloading Qt with a connection timeout, before code
analysis. GitHub refused a retry while the overall workflow was running.
Linux/macOS/Windows builds, Linux clang-tidy, Android, Gazelle, pre-commit, and
CodeQL passed. SonarCloud was still running when publication was verified.
