# Numeric Map Target Resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make decoding, copying, and editing share definition interpretation and safely prepare fresh numeric targets without changing their observable behavior.

**Architecture:** Add a session-independent owning definition snapshot, consumed by decoding, placement, and operation preparation. An internal session module owns the snapshot and decoded cells for one operation and performs copy/calculation/patch application without exposing borrowed views. Keep public operation signatures and whole-map validation; derive temporary views only after owners reach their final location.

**Tech Stack:** C++23, portable backend, GoogleTest/Google Mock, Bazel, Gazelle, Qt 6 at existing desktop boundaries only.

**Spec:** [Numeric map target resolution](../specs/2026-10-13-numeric-map-target-resolution.md).

## Global Constraints

- “Body fields retain their precedence over scaling defaults; axes retain their resolved-axis semantics.”
- “placement checks declared axes even where decoding omits an axis.”
- “There is no persistent cache of metadata or decoded values.”
- “Borrowed views remain inside the implementation; callers do not coordinate their lifetimes.”
- “Existing decode, copy, and edit operation interfaces remain the behavioral test surface.”
- “A structural failure in an axis continues to prevent an operation on the map body.”
- “Copying does not acquire an encoding, limit, or increment requirement.”
- “Absolute assignment does not acquire an increment requirement.”
- “These external corpora are not repository test dependencies.”
- “No hardware qualification is claimed.”

Follow [repository instructions](../../../AGENTS.md), [testing conventions](../../coding-style.md#tests), [layering](../../design-notes.md#layering), [ADR 0017](../../adr/0017-generate-bazel-targets-with-gazelle.md), and [ADR 0020](../../adr/0020-model-rom-memory-as-a-memory-map.md). Use the pinned Bazel and host setup in [PR CI](../../../.github/workflows/pr.yml). Add no runtime dependency, polymorphic adapter, ErrorKind, warning suppression, thread, Qt dependency, or filesystem access. Preserve unrelated checkout changes and work on a feature branch.

## Review Focus

- Short and heap-backed expressions after owner relocation/destruction: decoding and encoding must use owned metadata (Task 2 lifetime test; Task 3 moved preparation test).
- Declared axes omitted by decoder eligibility: placement still rejects fill/outside ranges and operations reject the whole map (Tasks 2 and 3).
- Valid decoded values with unusable edit-only metadata: copy succeeds; only an operation consuming the bad field fails (Task 3).
- Invalid current cells alongside structural validity: absolute replacements can repair bytes; copy and relative edits preserve cell diagnostics (Task 3).
- A retained session ID after intervening edits, other sessions closing, or its own closure: operations use fresh bytes and never retarget another session (Task 3).

---

## File structure and interfaces

This is one cooperating calibration refactor, not independent subsystem work. Execute the three tasks in order.

| File | Responsibility |
| --- | --- |
| `src/backend/calibration/session/testing/numeric_session_fixture.h` | Shared grid/ROM and workspace/session installation; no operation assertions |
| `src/backend/calibration/definition_interpretation_internal.{h,cpp}` | Own storage, byte order, expressions, bounds, increments, address and run/map geometry; no session knowledge |
| `src/backend/calibration/definition_interpretation_internal_test.cpp` | Ownership regression using decoding/encoding, rather than field-by-field tests |
| `src/backend/calibration/session/numeric_operation_types.h` | Existing copy/edit request, operation, and outcome types, moved without renaming or changing defaults |
| `src/backend/calibration/session/numeric_target_internal.{h,cpp}` | Session lookup, whole-map decode, owned preparation, copy formatting, edit calculation and guarded patch application |
| `src/backend/calibration/session/numeric_target_internal_test.cpp` | Move-safety test of prepared operation using shared fixture |
| Existing decoder, placement and numeric use-case sources/tests | Consume the new modules; retain interface-level compatibility evidence |
| Existing calibration/session/testing BUILD files | Generated targets/dependencies with explicit narrow visibility and testonly settings |
| Calibration compatibility, design notes and docs index | Enduring ownership contract and active-work navigation |

`internal` below means `fastecu::calibration::internal`; test helpers live in `fastecu::calibration::testing`. Keep production metadata and preparation targets visible to `//bazel/layers:backend` only; preparation may use package-only visibility because every consumer is in its package. Do not expand layer groups. Existing public headers continue providing their current transitive types/includes.

### Task 1: Share numeric session setup without changing test behavior

**Files:** Create `src/backend/calibration/session/testing/numeric_session_fixture.h`; modify `src/backend/calibration/session/testing/BUILD.bazel`, `src/backend/calibration/session/BUILD.bazel`, `src/backend/calibration/session/numeric_copy_use_case_test.cpp`, and `src/backend/calibration/session/numeric_edit_use_case_test.cpp`.

**Interfaces:** Produces `inline constexpr std::size_t kBody = 0x10`, `definition::RomDefinition GridDefinition()`, `bytes::Bytes GridRom()`, and a `NumericSessionFixture : public ::testing::Test` with protected `SetUp()`, `Install(std::optional<definition::RomDefinition>, bytes::Bytes = GridRom(), RomProtocolInfo = {}, std::optional<memory::MemoryMap> = std::nullopt) -> void`, `Session() -> CalibrationSession&`, `RomBytes() -> bytes::Bytes`, plus existing protected `workspace_` and `id_`. This is the edit suite's existing memory-map-capable setup, shared with the copy suite; keep `Cell`, `Copy`, `Edit`, `BodyBytes`, and matchers local to their suites.

- [ ] **Step 1: Record the behavioral baseline.**

Run: `bazel test --config=release //src/backend/calibration/session:numeric_copy_use_case_test //src/backend/calibration/session:numeric_edit_use_case_test`.
Expected: both suites pass. This fixture-only task uses existing tests as its test cycle; do not invent a failing behavior for a relocation.

- [ ] **Step 2: Extract the fixture and migrate both suites.**

Preserve the 3×2 body `{10,20,30,40,50,200}`, X `{1,2,3}`, Y `{4,5}`, bounds `[0,200]`, increments `10`/`1`, and `proto_b` adoption. Put definitions inline in the header, following existing header-only test support. Both suite classes inherit the shared fixture. Keep explicit memory layouts in the edit tests and the copy-to-paste integration test in the copy suite. Create one `numeric_session_fixture` cc_library with `testonly = True`.

- [ ] **Step 3: Regenerate the graph.**

Run `python3 scripts/gazelle_check.py --fix`, review the two affected BUILD files, then run `python3 scripts/gazelle_check.py`.
Expected: check succeeds without further changes; unrelated generated changes must be understood before staging.

- [ ] **Step 4: Re-run the Step 1 command.**

Expected: same tests and assertions pass, including explicit read-only layouts and copy-to-paste unchanged behavior.

- [ ] **Step 5: Commit the fixture deliverable.**

Stage only the five paths in this task. Commit: `test: share numeric calibration session setup`.

### Task 2: Centralize owning definition interpretation and wire decoding/placement

**Files:** Create `src/backend/calibration/definition_interpretation_internal.h`, `src/backend/calibration/definition_interpretation_internal.cpp`, and `src/backend/calibration/definition_interpretation_internal_test.cpp`; modify `src/backend/calibration/calibration_service.cpp`, `src/backend/calibration/map_placement.cpp`, `src/backend/calibration/calibration_service_test.cpp`, `src/backend/calibration/map_placement_test.cpp`, and `src/backend/calibration/BUILD.bazel`.

**Interfaces:** Produces `ResolvedElement ResolveMapElement(const definition::RomDefinition&, const definition::CalibrationMap&, NumericTarget)` and `std::uint32_t ElementByteWidth(std::optional<definition::StorageType>, const definition::Scaling*)` in `internal`. `ResolvedElement` owns `std::optional<memory::DefinitionAddress> address`, `std::optional<definition::StorageType> storage_type`, std::string fields `endian/from_byte/to_byte/min_value/max_value`, double fields `coarse_increment/fine_increment`, `MapDimensions dimensions`, std::uint32_t fields `width/height/start_position/interval/byte_width`, `std::uint64_t count`, and `bool is_selectable`. It exposes `MapElementSpec EditSpec() const&` and deletes `EditSpec() const&&`. No stored string_view, span, pointer to scaling, or cached ElementRun/MapElementSpec. NumericTarget comes from existing `map_edit.h`; do not move it.

- [ ] **Step 1: Add the ownership regression and placement characterization.**

In the new internal test, construct a two-cell uint16 body at address 0 with little endian, body fallback, `from_byte="x*2"`, `to_byte="x/2"`, limits `0`/`9000`, and increments `60`/`1`. Parameterize expression strings as short values and as the same expressions padded with 128 spaces. Resolve in a scope, move the owner out, destroy the original definition/owner, then build views from the moved owner. `ElementRun` remains the existing decoder input; fill it from the resolved address/count/stride/storage/endian/from_byte/selectable fields.

```cpp
// ResolvedElementOwnsMetadataAcrossMoves (after the setup above)
const bytes::Bytes rom{0xE8, 0x03, 0xD0, 0x07};
const auto decoded = DecodeNumericRun(DefinitionBytes(rom), run);
ASSERT_THAT(decoded, IsOk());
EXPECT_THAT(decoded->cells, ElementsAre(IsOkAnd(2000), IsOkAnd(4000)));
const auto encoded = CalculateAssignment(DefinitionBytes(rom),
                                        moved.EditSpec(), 2, decoded->cells,
                                        {.first_row=0, .first_col=0, .last_row=0, .last_col=0}, "7500");
ASSERT_THAT(encoded, IsOk());
ASSERT_EQ(encoded->writes.size(), 1U);
EXPECT_THAT(encoded->writes[0].bytes, ElementsAre(0xA6, 0x0E)); // 3750 little endian
```

Add a dependent requires-expression concept `HasRvalueEditSpec<T>` testing `std::move(value).EditSpec()`, and `static_assert(!HasRvalueEditSpec<internal::ResolvedElement>)`. This tests the correctly spelled overload; never take a borrowed view before a move.

In placement's existing fixture add `ChecksDeclaredAxesEvenWhenDecoderOmitsThem` for X axis types `""`, `"Static X Axis"`, and `"unexpected"`, including dimension 1. Each case declares uint8 storage at `0x20` (the fill block), with body at `0x10`:

```cpp
EXPECT_THAT(Check(definition, Layout()), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("X axis")));
```

Keep `UsesMapThenScalingStoragePrecedence`, `DecodesNumericAxesWithResolvedExpressions`, `AppliesStrideInStorageBytes`, static labels, blob/selectable semantics, overflow, and diagnostics tests. Extend precedence coverage: scaling little endian versus explicit map big endian on `{0x12,0x34}` yields `4660`, with an explicit uint16 body.

- [ ] **Step 2: Run the new test and existing calibration tests.**

After generating the new test target, run `bazel test --config=release //src/backend/calibration:definition_interpretation_internal_test //src/backend/calibration:calibration_service_test //src/backend/calibration:map_placement_test`.
Expected: the new ownership test fails to compile until the resolver exists; placement characterization may already pass and must continue passing.

- [ ] **Step 3: Implement the resolver and wire its consumers.**

Body storage/endian prefer map fields then scaling; body conversions use scaling or `"x"`. Axes use their resolved fields without scaling fallback for storage/endian/expressions; scaling supplies bounds/increments. Empty/whitespace increments map to `0`, malformed nonempty increments to NaN without returning an error. Body width/height are x/y; X width/height are x/1; Y width/height are 1/y; calculate count in uint64 before narrowing. Preserve optional addresses. Resolve declared axis facts even when its type would be omitted by decoding.

Move ElementByteSize's interpretation into `ElementByteWidth`; leave public `ElementByteSize` as a forwarding wrapper. Blob width remains the first selection's byte-vector size, falling back to StorageByteSize. Do not make the new target depend on calibration_service (that would create a cycle).

Decoder builds a local ElementRun from the owning snapshot. Retain its existing address/dimension/scaling checks, blob early return, DecodeTypedAxis eligibility/static handling, selectable zero semantics, and diagnostic prefixes. Placement consumes snapshot address/count/stride/byte_width but keeps its own skip/overflow, fill, writability and definition-base checks. Blob placement still checks exactly one element at start/interval 1. No decoder eligibility predicate is shared with placement.

- [ ] **Step 4: Regenerate and verify.**

Run `python3 scripts/gazelle_check.py --fix`, inspect the graph for cycles and visibility, then `python3 scripts/gazelle_check.py` and `bazel test --config=release //src/backend/calibration/...`.
Expected: generation is stable and all calibration tests pass, including existing session/edit field tests still present until Task 3.

- [ ] **Step 5: Commit shared interpretation.**

Stage the eight named paths in this task. Commit: `refactor: share owning calibration definition interpretation`.

### Task 3: Hide target preparation lifetimes behind unchanged copy/edit operations

**Files:** Create `src/backend/calibration/session/numeric_operation_types.h`, `src/backend/calibration/session/numeric_target_internal.h`, `src/backend/calibration/session/numeric_target_internal.cpp`, and `src/backend/calibration/session/numeric_target_internal_test.cpp`; modify `src/backend/calibration/session/numeric_copy_use_case.{h,cpp}`, `src/backend/calibration/session/numeric_edit_use_case.{h,cpp}`, both use-case tests, `src/backend/calibration/session/BUILD.bazel`, `docs/reference/calibration-compatibility.md`, `docs/design-notes.md`, and `docs/README.md`; delete `src/backend/calibration/session/map_element_fields.{h,cpp}` and `src/backend/calibration/session/map_element_fields_test.cpp` after preserving substantive cases below.

**Interfaces:** Consumes Tasks 1–2. Move existing NumericSelection, IncrementEdit, AssignmentEdit, InterpolationEdit, PasteEdit, NumericEditOperation, both requests and all outcomes/reasons into `numeric_operation_types.h` unchanged. Public use-case headers include it and retain `CopyNumericValues(CalibrationWorkspace&, const NumericCopyRequest&) -> Result<NumericCopyOutcome>` and `ApplyNumericEdit(CalibrationWorkspace&, const NumericEditRequest&) -> Result<NumericEditOutcome>`.

Produces internal `PreparedNumericTarget`, owning a ResolvedElement and NumericRun plus an operation-scoped CalibrationSession pointer, with `Copy(const SelectionRange&) const -> Result<NumericCopyOutcome>` and `Apply(const SelectionRange&, const NumericEditOperation&) -> Result<NumericEditOutcome>`. `using NumericTargetPreparation = std::variant<PreparedNumericTarget, NumericEditNotApplicable>`; `PrepareNumericTarget(CalibrationWorkspace&, SessionId, std::size_t, NumericTarget) -> NumericTargetPreparation`. No borrowed accessor is exposed. Do not persist prepared targets across operations or session closure.

- [ ] **Step 1: Add operation characterization tests using the shared grid.**

Keep existing local `Copy`, `Edit`, `Cell`, and outcome matchers. Add named tests with these setups and exact assertions; set up malformed metadata separately so one bad field cannot mask another.

```cpp
// CopyReadsValuesAfterAnEdit (copy suite, default grid)
ASSERT_THAT(ApplyNumericEdit(workspace_, {.session=id_, .map_index=0,
    .selection={.target=NumericTarget::kMapBody, .elements=Cell(0,0)},
    .operation=AssignmentEdit{"33"}}), IsOkAnd(VariantWith<NumericEditChanged>(_)));
EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0,0)), CopiedText("33"));

// AnInvalidAxisRejectsBodyOperations (each suite): X address 0x40 outside GridRom
EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0,0)), NotApplicable(NotApplicableReason::kUnavailableTarget));
EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0,0), AssignmentEdit{"7"}),
            NotApplicable(NotApplicableReason::kUnavailableTarget));
EXPECT_EQ(RomBytes(), GridRom());
EXPECT_FALSE(Session().Dirty());

// ReciprocalEncodingIsValidatedForTheRequestedValue (edit suite):
// one-cell uint16 body at kBody, raw big-endian 1000, both expressions
// "7500000/x", bounds [0,9000], coarse increment 60, no axes
EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0,0), AssignmentEdit{"7500"}),
            Unchanged(NoChangeReason::kUnchanged));
EXPECT_THAT(Edit(NumericTarget::kMapBody, Cell(0,0), AssignmentEdit{"0"}), IsErr(ErrorKind::kInvalidConfig));
EXPECT_EQ(RomBytes(), before);
EXPECT_FALSE(Session().Dirty());
// In copy suite with the same explicitly local reciprocal setup:
EXPECT_THAT(Copy(NumericTarget::kMapBody, Cell(0,0)), CopiedText("7500"));
```

Add `CopyIgnoresEditOnlyMetadata` in copy suite, parameterized by to_byte `"1/0"`, minimum `"bad"`, reversed bounds `"200"`/`"0"`, and fine_increment `"bad"`: copying first body cell remains `CopiedText("10")`, bytes unchanged and clean. Add `AssignmentIgnoresMalformedIncrement` in edit suite with fine_increment `"bad"`: assignment `"7"` changes first body byte to 7; subsequent fine increment is InvalidConfig and preserves bytes/dirty state. A separate `ConsumedMalformedLimitRejectsAssignment` with minimum `"bad"` rejects `"7"` before mutation.

Extend invalid-cell test with fine increment failure before successful absolute repair; retain nonempty copy diagnostic and its first failing coordinates. Add `ResolvedAxisExpressionsAndStrideDriveEdits`: X start_position 2, interval 2, expressions `"x*10"`/`"x/10"`, scaling increments 10/1; set raw X bytes at offsets 1,3,5 to 1,2,3. Assignment `"40"` to Cell(0,1) writes raw 4 at offset 3 and preserves offsets 1,5 and the body. Retain the existing body fallback integration test and all limit/increment behavior tests.

Extend unavailable-target tests in both callers to absent/static axes and blob body; retain decoder/static/selectable tests without merging their fixtures. Add `ClosingAnotherSessionKeepsTheTargetIdStable`: adopt another session, close it, copy returns `"10"`, assignment `"7"` affects the original session; close the original and assert both operations return kClosedSession. Keep copy precision and copy-to-paste unchanged tests.

- [ ] **Step 2: Run characterization tests before moving code.**

Run `bazel test --config=release //src/backend/calibration/session:numeric_copy_use_case_test //src/backend/calibration/session:numeric_edit_use_case_test`.
Expected: compatibility characterizations pass on the old implementation. If any fails, report the implementation/spec disagreement before expanding this refactor.

- [ ] **Step 3: Add a failing moved-preparation regression.**

In `numeric_target_internal_test.cpp`, derive from NumericSessionFixture. Prepare the default body, move the PreparedNumericTarget alternative out of its variant into a new owner, destroy the moved-from preparation, then operate on the destination. Repeat with `from_byte="x/3"`, `to_byte="x*3"` padded with 128 spaces to cover heap-backed strings as well as SSO.

```cpp
// MovedPreparationKeepsMetadataAndCellsUsable (default-grid case)
EXPECT_THAT(moved.Copy(Cell(0,0)), IsOkAnd(VariantWith<NumericCopyText>(Field(&NumericCopyText::text, "10"))));
EXPECT_THAT(moved.Apply(Cell(0,0), AssignmentEdit{"7"}), IsOkAnd(VariantWith<NumericEditChanged>(_)));
EXPECT_EQ(Session().File()[testing::kBody], 7);
// Padded x/3 + x*3 case: AssignmentEdit{"7"} writes raw 21 instead.
```

Define Cell locally in this test. Generate its target and run `bazel test --config=release //src/backend/calibration/session:numeric_target_internal_test`.
Expected: compilation fails because preparation is not implemented yet.

- [ ] **Step 4: Implement owned preparation and delegate both public operations.**

Move types into the header-only target first to prevent a dependency cycle: the internal implementation depends on type definitions, never on either public use-case implementation target. Prepare performs the existing lookup ordering, calls session.DecodeMap for whole-map placement and structural validation, selects only a NumericRun, resolves the Task 2 snapshot, and owns the selected cells. Return existing not-applicable reasons exactly. No target-local decode shortcut or encoding preflight.

Move FormatValue and selection validation into Copy, preserving fixed-notation shortest roundtrip, tabs/LF, row-major order and first-cell diagnostics. Move ParsePaste, Calculate dispatch and WritePatch into Apply's implementation. Derive EditSpec only locally inside Apply after all owner moves; render the byte window there (copy does not need an edit window), pass DefinitionBytes only while that window lives, and validate every write before applying any. Use stored width/height instead of duplicated target geometry switches. Keep unchanged outcomes, dirty-state semantics and session write guards intact.

Delete old field collection only after its substantive fallback, resolved-axis/stride, bounds and increment cases are covered above. Retire field-plucking assertions and inaccurate lifetime comments; do not duplicate them against the new structure. Both public wrappers prepare and immediately perform one operation, returning their existing Result types.

- [ ] **Step 5: Update owning documentation alongside the code.**

Correct the reported stale final paragraph of “Decoded values and map edits”: backend shared interpretation owns definition facts; backend operation preparation owns metadata/cells and validates/applies patches; desktop translates selections and presents outcomes. Link the new owners from that contract, and link the contract from design notes. State fresh operation-local ownership, move safety, whole-map rejection and consumed-field validation there. Update active-work navigation to link this plan while unfinished. Do not claim hardware qualification or copy corpus inventories into current contracts.

- [ ] **Step 6: Regenerate and run focused tests.**

Run `python3 scripts/gazelle_check.py --fix`, review affected targets (including testonly/visibility and deleted targets), then `python3 scripts/gazelle_check.py` and `bazel test --config=release //src/backend/calibration/...`.
Expected: stable BUILD output; all existing and new suites pass. Review test changes to ensure operation assertions stayed local and no universal fixture was introduced.

- [ ] **Step 7: Run full delivery gates.**

Run `prek run --all-files`, `python3 scripts/gazelle_check.py`, `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, and `bazel run //:clang_tidy_report_changed`.
Expected: successful exits and no first-party warnings or analysis findings. Review scoped diff; record unavailable host gates honestly. PR CI must also pass its platform builds/tests, Android portable-core check, analysis and packaging jobs before delivery; local macOS success is not evidence for other platforms.

- [ ] **Step 8: Commit the integrated operation deliverable.**

Stage only the paths named in this task, including deletions and generated BUILD file. Commit: `refactor: prepare numeric targets with operation-local ownership`.

## Delivery and document lifecycle

Preserve original spec/plan contents in a separate delivered documentation change before deletion: these documents are currently untracked and a squash that introduces and removes them loses their contents. After implementation review and delivery, extract any remaining enduring knowledge into the owning contract, then remove completed spec/plan and their active-work links in a later change following [documentation lifecycle](../../README.md#recover-completed-work-and-deleted-source-citations). Do not delete other active artifacts.

Self-review: all spec sections map to Tasks 1–3 or this delivery rule; five review-focus conditions have named tests; preparation exposes no borrowed views; decoder/placement eligibility stays distinct; public signatures remain unchanged. Characterization passes are explicitly distinguished from the new interfaces' failing-test cycles. No implementation is authorized by this plan alone.
