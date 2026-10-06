# Calibration Typed Data Flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace calibration's internal text transport with typed snapshots and
double arithmetic while implementing the approved edit and error policies.

**Architecture:** Add checked numeric evaluation alongside the logging-facing API.
Introduce typed decoding and numeric patch construction, then migrate desktop
consumers together and remove calibration's legacy text interfaces. Retain current
ownership boundaries until the separately scoped extraction follow-up.

**Tech Stack:** Qt 6, C++23, Bazel, GoogleTest, native byte types, `std::expected`.

**Spec:** [Approved calibration typed-flow design](../specs/2026-10-06-calibration-typed-data-flow-design.md).

## Global Constraints

- Numeric calculations use `double`; decimal formatting belongs to presentation.
- Legacy text-rounding stages and byte-for-byte edit outcomes are not compatibility requirements for this redesign.
- Every edit is atomic across the selected cells.
- ROM bytes remain authoritative; there is no parallel mutable value cache.
- Keep the existing logging-facing evaluator APIs and behavior in this change.
- Do not widen Bazel layer visibility or introduce Qt into algorithms/backend.
- Do not change the evidence-gated `wrx02` read/write predicate, existing endian rules, flash/checksum workflows, logger behavior, definition-authoring formats, or switch/MultiSelectable semantics as incidental cleanup.
- Follow [repository instructions](../../../AGENTS.md) and [coding conventions](../../coding-style.md); regenerate managed BUILD files with Gazelle and retain warnings as errors.
- Use an isolated feature worktree at execution time and preserve unrelated files.
- Never commit directly to `master`; push only when authorized.

## Review Focus

- Static labels containing commas remain one label; typed data has no delimiter ambiguity (Task 2).
- A tiny valid numeric value rendered as zero still participates in arithmetic at its full double value (Tasks 3 and 4).
- Clamping can produce unchanged bytes; the no-change notice must not falsely attribute every no-op to storage resolution (Task 4).
- A modal dialog can outlive a selected ROM; editing must use the original stable identity and current bytes or become inert if closed (Task 4).
- An invalid decoding expression can coexist with valid encoding; assignment succeeds but the fresh display can remain `NaN` (Tasks 3 and 4).

## File and interface map

New files have one responsibility:

- `src/algorithms/expression/checked_expression.{h,cpp}` and its package-owned
  test: checked numeric evaluation, finite literal parsing, and grammar errors.
- `src/backend/calibration/decoded_map.h`: owned typed map values, axes, and cells.
- `src/backend/calibration/numeric_map_edit.{h,cpp}` and its package-owned test:
  double edit calculations and validated encoded writes.

Modify `calibration_service.{h,cpp}` for decoding, `CalibrationSession` for its
decode API, the calibration adapters for typed boundaries, and calibration/menu
widgets for rendering and dispatch. Additive APIs in Tasks 1–3 keep intermediate
commits buildable; Task 4 migrates production consumers and Task 5 removes obsolete
calibration APIs. Transitional implementations must not remain after Task 5.

## Task 1: Checked numeric expression evaluation

**Files:** Create the checked-expression files and test above; regenerate
`src/algorithms/expression/BUILD.bazel`. Existing evaluator sources/tests stay.

**Interfaces:** In namespace `fastecu::expression`, define
`struct EvaluationError { std::string detail; };`,
`std::expected<double, EvaluationError> parse_finite_number(std::string_view text);`
and `std::expected<double, EvaluationError> evaluate_checked(std::string_view expression, double x);`.
Errors remain algorithm-owned; callers translate them to backend errors.

- [ ] Write `CheckedExpression` tests asserting `"x*2+1"` at `x=3` gives `7`,
  `"-(x+2)"` at `3` gives `-5`, `"+1.25e-2"` gives `0.0125`, and blank input
  gives the supplied `x`. Prove `"1000000000000000+1-1000000000000000"` gives `1`
  without decimal rounding between operators. Reject `"2junk"`, unmatched
  parentheses, missing operands, unknown variables/functions, `"1/0"`,
  `"0/0"`, non-finite input, and overflowing literals/results. Numeric literal
  parsing consumes the entire trimmed input; blank literals are errors.
- [ ] Run `bazel test --config=release //src/algorithms/expression:checked_expression_test`;
  expect failure before the new target/API exists.
- [ ] Implement recursive-descent arithmetic with ordinary precedence, unary signs,
  numeric literals, and parentheses. Use numeric values throughout; validate
  finiteness before each successful result and reject division by zero. Return
  diagnostics without throwing across the algorithm API. Register and regenerate
  the package-owned library/test through `python3 scripts/gazelle_check.py --fix`.
- [ ] Run the new test plus
  `//src/algorithms/expression:expression_evaluator_test`,
  `//src/backend/logging:logging_conversion_test`, and
  `//src/backend/logging:logging_session_test`; expect all passing.
- [ ] Review generated BUILD changes and commit only this task's named paths as
  `feat: add checked numeric expression evaluation`.

## Task 2: Typed calibration decoding

**Files:** Create `decoded_map.h`; modify `calibration_service.{h,cpp}` and
`calibration_service_test.cpp`; regenerate `src/backend/calibration/BUILD.bazel`.

**Interfaces:** In `fastecu::calibration`, define
`using NumericCell = Result<double>;`, `struct NumericRun { std::vector<NumericCell> cells; };`,
`struct StaticAxis { std::vector<std::string> labels; };`,
`struct BlobValue { bytes::Bytes data; };`,
`using AxisValue = std::variant<std::monostate, NumericRun, StaticAxis>;`, and
`struct DecodedMap { std::variant<NumericRun, BlobValue> body; AxisValue x_axis; AxisValue y_axis; };`.
Produce `Result<NumericRun> decode_numeric_run(bytes::ByteView rom, const ElementRun& run);`
and `Result<DecodedMap> decode_calibration_map(const definition::RomDefinition& definition, const definition::CalibrationMap& map, bytes::ByteView rom);`.
The outer error is structural; numeric cell errors are contained within a run.

- [ ] Add typed decode tests: four values give exactly four cells; signed/unsigned
  8/16/24/32-bit and float storage decode correctly with existing byte order and
  striding; absent scaling and blank expressions use identity; static labels
  `"Low, load"`/`"High"` survive as two labels; absent axes are `monostate`; blob
  bytes `{0xcc, 0xdd}` remain those bytes. A scaling such as `"1/x"` over values
  `{0,2}` produces one error cell and one valid `0.5` cell. Missing addresses,
  zero/overflowing dimensions, and out-of-ROM ranges fail structurally.
- [ ] Run `bazel test --config=release //src/backend/calibration:calibration_service_test`;
  expect the new tests to fail before implementing their APIs.
- [ ] Implement typed decoding using Task 1, retaining the established raw storage
  interpretation and the evidence-gated addressing behavior. Convert algorithm
  failures to existing `ErrorKind::InvalidConfig` with map/run/cell context.
  Validate layout before reading; retain static labels as labels and explicit
  absent-axis state. Leave existing text APIs available only for consumers not
  yet migrated. Regenerate the package BUILD.
- [ ] Run `bazel test --config=release //src/backend/calibration:calibration_service_test //src/backend/calibration/session:calibration_session_test`;
  expect passing typed tests and existing consumers.
- [ ] Commit named task paths as `feat: decode calibration maps into typed values`.

## Task 3: Double edit calculation and storage encoding

**Files:** Create `numeric_map_edit.{h,cpp}` and `numeric_map_edit_test.cpp`;
modify shared layout helpers in `map_edit.{h,cpp}` only where needed; regenerate
the backend calibration BUILD.

**Interfaces:** Consume Task 2's `NumericCell`, existing `MapElementSpec`,
`SelectionRange`, and `IncrementStep`. Define
`struct CellWrite { std::uint32_t index; std::uint64_t byte_address; bytes::Bytes bytes; };`
and `using NumericEditPatch = std::vector<CellWrite>;`.
Produce the following functions in `fastecu::calibration`:

```cpp
Result<NumericEditPatch> calculate_increment(bytes::ByteView rom, const MapElementSpec& spec,
    std::uint32_t run_width, std::span<const NumericCell> cells,
    const SelectionRange& range, IncrementStep step);
Result<NumericEditPatch> calculate_assignment(bytes::ByteView rom, const MapElementSpec& spec,
    std::uint32_t run_width, std::span<const NumericCell> cells,
    const SelectionRange& range, std::string_view expression);
Result<NumericEditPatch> calculate_interpolation(bytes::ByteView rom, const MapElementSpec& spec,
    std::uint32_t run_width, std::span<const NumericCell> cells,
    const SelectionRange& range);
Result<NumericEditPatch> calculate_paste(bytes::ByteView rom, const MapElementSpec& spec,
    std::uint32_t run_width, std::uint32_t run_height,
    const SelectionRange& range, std::span<const std::vector<double>> values);
```

Definition limit text is external metadata, parsed once into finite optional
double bounds per operation. These APIs do not accept formatted cell values,
precision arguments, or computed display strings. `run_width` is the resolved
body/axis width, preserving the existing Y-axis width of one.

**Approved Set Value input rule:** Signed literals are absolute
assignments (`-20` assigns negative twenty); relative expressions explicitly use
`x` (`x-20` subtracts twenty). Task 4 updates dialog examples accordingly.

- [ ] Add `NumericMapEdit` tests for ties (`10.5 → 11`, `-10.5 → -11`), each
  integer width's exact limits and overflow, float overflow/non-finite failures,
  and definition clamping (`120` to `100` for bounds `0–100`). Malformed or
  reversed bounds reject the operation. Test assignment/paste into invalid
  cells, relative rejection, interior-invalid interpolation success and
  endpoint-invalid rejection. One invalid required cell returns no patch.
  A `0.01` step on storage with scaling `x/10` remains unchanged after exactly
  one attempt. A cell at `0.0001`, displayed as `0.00`, with `to_byte="x*10000"`
  produces raw integer `101` for `x+0.01`. Reject out-of-extent and negative
  selection coordinates.
- [ ] Run `bazel test --config=release //src/backend/calibration:numeric_map_edit_test`;
  expect failure before the target/API exists.
- [ ] Implement a shared checked encoding path: validate input/metadata, clamp,
  evaluate `to_byte`, round integer storage ties away, check exact representable
  range before any narrowing, then pack through existing layout helpers. For
  float storage reject values outside finite float range before conversion.
  Do not pre-read old scaled values for absolute assignments or paste; determine
  whether an assignment expression uses the `x` variable from its tokens. Constant
  expressions can evaluate with a finite placeholder input on invalid cells;
  expressions using `x` require a valid current cell. Compute interpolation
  solely from required endpoints/corners. Remove retry/sign-wrap policy from
  these new operations.
  Filter byte-identical writes, retaining no fractional accumulator. Regenerate
  BUILD and keep legacy edit APIs only until Task 4's caller migration.
- [ ] Run new numeric edit tests, existing `map_edit_test`, and
  `calibration_service_test`; expect passing tests without changes to the pinned
  `wrx02` predicate evidence.
- [ ] Commit named task paths as `feat: calculate calibration edits with double values`.

## Task 4: Migrate session, edit adapters, and presentation together

**Files:** Modify `session/calibration_session.{h,cpp}`, its test, and
`session/rom_open_test.cpp` under `src/backend/calibration`;
`map_edit_adapter.{h,cpp}` and test; `map_presentation.{h,cpp}` and test in
`src/ui/desktop/calibration`; `calibration_maps.{h,cpp}`,
`calibration_maps_test.cpp`, `menu_actions.cpp`, `mainwindow.cpp`, and
`mainwindow_test.cpp` in `src/ui/desktop/widgets`; regenerate affected BUILDs.

**Interfaces:** Change `CalibrationSession::decode_map(std::size_t) const` to
`Result<DecodedMap>`. `ResolvedEdit` owns its selected `NumericRun`; expose
`std::span<const calibration::NumericCell> cells() const&` rather than `cell_text()`.
Change UI-owned `apply_patch` to accept `const calibration::NumericEditPatch&`.
It validates all target indices, expected addresses, and byte ranges before writes.
Preserve existing read/write address rules; derive expected writes with the write
predicate and preserve its separate `wrx02` tests.

In presentation define `struct PresentedCell { QString text; QString diagnostic; std::optional<double> numeric_value; };`.
Store body cells as `std::vector<PresentedCell>`, axes as
`std::optional<std::vector<PresentedCell>>`, and blob identity separately as
`std::optional<bytes::Bytes>`. Selection labels remain UI text; selection byte
values are decoded for byte-identity comparison. No joined map-value strings or
fake trailing selections remain. Static axis cells have no numeric payload.
Make color bounds optional when no numeric cells are valid.

- [ ] Add session/adapter tests proving four cells reject index four, wrong-map
  byte addresses reject, a valid first write plus invalid second write changes
  neither ROM nor dirty state, and complete no-ops keep clean sessions clean.
  Add widget tests for `NaN` with tooltip, successful invalid-cell assignment
  whose fresh decode remains invalid, valid relative rejection, interpolation
  recovery, labels containing commas, absent-axis fallback, blob byte identity,
  neutral colors for all-invalid maps, and exclusion of invalid cells from bounds.
  Test a structural failure at first opening and after refresh: an error is
  visible, grid edits are disabled, and stale values disappear.
- [ ] Add modal identity tests: changing selected ROM during input never edits
  that other ROM; closing the original session makes the action inert; changing
  its bytes during input uses current values when calculating after the dialog.
  Add no-change notices for both sub-resolution increments and clamping, with
  distinct causes. Add Set Value tests for absolute `-20` and relative `x-20`.
- [ ] Run the affected calibration session, adapter, presentation, and widget
  suites before implementation; expect failing new assertions/API compilation.
- [ ] Switch session decoding to Task 2 and edit calls to Task 3. Parse pasted
  numeric text completely using Task 1 before calculating writes. Preserve field
  resolution and patch-application ownership in the UI. Replace Qt increment
  parsing that silently yields zero with checked numeric metadata parsing and
  a reported error. Re-resolve original stable identity and current bytes after
  modal input. Never use requested display text as the post-write value.
- [ ] Migrate rendering to typed presentation cells and direct double formatting
  using definition precision. Render errors as `NaN`; expose diagnostics on hover.
  Add `CalibrationMaps::show_map_error(const fastecu::Error&)` to clear/hide stale grid
  content, display the diagnostic, and disable editing; successful refresh
  restores the ordinary view. Permit map windows to open despite structural
  decode errors by removing MainWindow's pre-creation decode-failure return.
  Preserve window identity/name without needing successful decode. Keep widget
  layout, static labels, switch/MultiSelectable behavior, and emitting-session
  dispatch. Compare blob selections by bytes; leave selectable write extraction
  in MainWindow for the follow-up.
- [ ] Regenerate BUILD and run `bazel test --config=release`
  with `//src/backend/calibration/session:calibration_session_test`,
  `//src/backend/calibration/session:rom_open_test`,
  `//src/ui/desktop/calibration:map_edit_adapter_test`,
  `//src/ui/desktop/calibration:map_presentation_test`,
  `//src/ui/desktop/widgets:test_calibration_maps`, and
  `//src/ui/desktop/widgets:test_mainwindow`; expect all passing.
- [ ] Commit named task paths as `refactor: use typed calibration values throughout desktop flows`.

## Task 5: Remove obsolete calibration transport and reconcile contracts

**Files:** Modify `calibration_service.{h,cpp}`/test, `map_edit.{h,cpp}`/test,
affected session and UI tests, generated BUILDs, `docs/design-notes.md`,
`docs/reference/calibration-compatibility.md`, and `docs/tech-debt.md`.

**Interfaces:** Retain the typed decode/numeric edit APIs from Tasks 2–4 and
existing raw storage/addressing helpers. Remove `MapCellValues`, calibration's
comma-text decode APIs, precision-driven legacy edit functions, unused display
text fields/helpers, and calibration uses of the logging-facing evaluator.

- [ ] Migrate remaining legacy fixtures/assertions to the typed API, retaining
  meaningful storage/striding/endian/selection coverage. Replace tests expecting
  phantom cell acceptance, zero substitution, intermediate decimal precision,
  or retry accumulation. Retain the explicitly pinned `wrx02` mismatch.
- [ ] Run focused backend and UI calibration suites; expect failures from any
  fixture/API dependency not yet migrated. Remove obsolete implementations and
  targets, then regenerate BUILD. Review `rg` results for `MapCellValues`,
  `cell_text`, legacy precision arguments, and comma joins/splits in calibration
  consumers; remaining text processing must have a named external-input or
  unrelated widget-identity purpose rather than transporting numeric values.
- [ ] Update the owning calibration reference and design notes with implemented
  typed ownership, numeric rules, atomic edits, and error states. Shorten debt to
  the remaining field-resolution/patch/selectable ownership extraction. Preserve
  the approved spec and plan in delivered history before their later removal;
  do not combine first introduction and deletion into one squash delivery.
- [ ] Run focused expression, logging regression, calibration/session, and UI
  suites. Then run `bazel build --config=release //:fastecu`,
  `bazel test --config=release //...`, `prek run --all-files`,
  `python3 scripts/gazelle_check.py --fix`, and
  `bazel run //:clang_tidy_report_changed`. Expect all checks passing and no
  unreviewed generated changes; apply fixes and rerun their affected checks.
  Confirm supported platform CI/packaging before delivery without claiming
  hardware qualification from automated tests.
- [ ] Review the scoped diff against every spec section and commit named paths as
  `refactor: retire calibration text transport and document typed contracts`.

## Execution handoff

The design and Set Value input rule are approved; this plan is ready for review.
Native execution is recommended because the tasks share successive
decoder/edit/presentation interfaces and must converge in one consumer migration.
The alternative is subagent-driven execution with a fresh task implementer and
reviewer. Choose the execution method after plan review; this document itself
does not start product implementation.
