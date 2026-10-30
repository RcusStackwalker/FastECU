# Flash route registry, PR 1 (exact routes) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each workflow-owning family module exports the exact protocol ids it accepts; `workflow/` routes by exact match from one registry of flash routes; one table-driven test over the built-in catalog guards routing.

**Architecture:** A `FlashRoute` row in `flash_workflow.cpp` pairs a span of protocol ids, obtained from the family module, with the existing `CreateWorkflow` function. Prefix matching and `kRoutes` go away. The factory exposes the routed ids so a test can check them against the catalog. Nothing changes on the wire.

**Tech Stack:** C++23, Bazel, GoogleTest/gmock, Gazelle (ADR 0017).

**Spec:** [docs/superpowers/specs/2026-10-31-flash-route-registry-design.md](../specs/2026-10-31-flash-route-registry-design.md)
(decisions 1–3, Tests, Behaviour changes).

## Global Constraints

- No wire behaviour, prompt order or plan content changes. Wire transcripts and plan goldens are not edited (ADR 0024).
- Builder error kinds and texts stay byte-identical; family tests that assert them must pass unchanged.
- A protocol id is never matched by prefix.
- Warnings are errors; no suppressions (ADR 0018). Check `Result`/`Status` with `.has_value()`.
- Regenerate managed BUILD files with `python3 scripts/gazelle_check.py --fix` and review the output (ADR 0017).
- Feature branch `refactor/flash-exact-routes` from `master`; PR, never a direct commit.

## Decision the spec left open

- **EEPROM keeps its builder rules.** `BuildEepromReadPlan` picks transport and security by substring and suffix, and its tests use names outside the catalog (`…_kline_cobb`, `…_sh7058_can_ecutek`), on purpose. So the EEPROM route's id list lives in `eeprom_read_plan.h`, but the builder does **not** check membership. Every other family's builder already checks an exact table, or gets one in Task 2.
- **Unisia JECS M32R K-Line routes 4 of its 6 variants.** The builder also accepts the two `_bootmode` names, because the boot-mode workflow builds its Read plan through it. The route list is the four non-bootmode variants; the boot-mode route owns the other two.

## Review Focus

1. A name outside the catalog that the old prefix rows matched (`mitsu_ecu_m32r_can_typo`, `sub_ecu_denso_mc68hc16y5_02_bdm_x`, `sub_ecu_unisia_jecs_20_bootmode_x`) must now get no route. It must not reach some other family's route. Pinned in Task 3: `lookalikesOutsideTheCatalogHaveNoRoute`.
2. `sub_ecu_denso_mc68hc16y5_02_bdm` must reach the BDM workflow, never the K-Line MC68 one. The old table relied on row order for this. Pinned in Task 3 through the one-route-per-protocol check, and by `everyFlashProtocolRoutesToAWorkflowThatAcceptsIt`.
3. SH7058 Hitachi must still pick K-Line for Read and CAN for Write from one route. `routesSh7058ReadAndWriteWithPreTransportPrompts` stays, unchanged.
4. Converting the three `if` chains in Task 2 must keep each rejection's error kind and text. Each family's existing tests prove this; Task 2 runs them.
5. The logging-only `mitsu_ecu_m32r_kline_mut_dma` must stay unrouted. Pinned in Task 3: it is not a flash protocol, so `everyRoutedIdIsACatalogFlashProtocol` fails if any route claims it.

---

### Task 1: Project protocol ids from a family table

**Files:**
- Modify: `src/backend/flash/ecu/plan_primitives.h` (next to `FindCatalogEntry`)
- Test: `src/backend/flash/ecu/plan_primitives_test.cpp`

**Interfaces:**
- Produces: `template <auto Member, typename Entry, std::size_t N> consteval std::array<std::string_view, N> detail::ProtocolIdsOf(const std::array<Entry, N>& table)`. It returns `table[i].*Member` in table order. `Member` is a pointer to a `std::string_view` data member.

- [ ] **Step 1: Write the failing test** `TEST(PlanPrimitivesTest, protocolIdsOfProjectsTheProtocolColumnInOrder)`. Use a local `struct Row { std::string_view id; int x; }` and `constexpr std::array<Row, 2> kRows{{{"a", 1}, {"b", 2}}}`, then:
  - `static_assert(detail::ProtocolIdsOf<&Row::id>(kRows) == std::array<std::string_view, 2>{"a", "b"});`
  - `EXPECT_THAT(detail::ProtocolIdsOf<&Row::id>(kRows), ElementsAre("a", "b"));`
- [ ] **Step 2: Run** `bazel test //src/backend/flash/ecu:plan_primitives_test`. Expected: compile FAIL, `ProtocolIdsOf` not declared.
- [ ] **Step 3: Implement** `detail::ProtocolIdsOf` in `plan_primitives.h` with `std::ranges::transform` or an index loop over `N`.
- [ ] **Step 4: Run** the same test. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(flash): project protocol ids from a family table`.

### Task 2: Every workflow-owning family exports its protocol ids

**Files (each a `*_plan.h` declaration and its `*_plan.cpp` definition; EEPROM is `eeprom/eeprom_read_plan.{h,cpp}`):**

| Function (namespace `fastecu::flash`) | Source of the ids in the `.cpp` | Ids |
| --- | --- | --- |
| `MitsuColtM32rCanProtocols()` | `ProtocolIdsOf<&ColtVariant::protocol_id>(kColtVariants)` | 4 |
| `SubaruMitsuM32rKlineProtocols()` | existing `kProtocols` | 1 |
| `SubaruHitachiM32rKlineProtocols()` | existing `kProtocols` | 2 |
| `SubaruHitachiM32rCanProtocols()` | existing `kProtocols` | 1 |
| `SubaruTcuHitachiM32rKlineProtocols()` | existing `kProtocols` | 1 |
| `SubaruTcuCvtHitachiM32rCanProtocols()`, `SubaruTcuCvtMitsuMh8111CanProtocols()`, `SubaruTcuCvtMitsuMh8104CanProtocols()` | existing `kProtocols` | 1 each |
| `SubaruDenso1n83m15mCanProtocols()`, `SubaruDenso1n83m4mCanProtocols()`, `SubaruDensoSh72531CanProtocols()`, `SubaruDensoSh72543CanDieselProtocols()` | existing `kProtocols` | 1 each |
| `SubaruDensoSh705xDensoCanProtocols()` | `ProtocolIdsOf<&CatalogEntry::protocol>(kCatalog)` | 5 |
| `SubaruTcuDensoSh705xCanProtocols()` | projected `kCatalog` | 2 |
| `SubaruDensoSh7058CanProtocols()` | projected `kCatalog` | 5 |
| `SubaruDensoSh7058CanDieselProtocols()` | projected `kCatalog` | 2 |
| `SubaruDensoSh705xKlineProtocols()` | projected `kVariants` | 6 |
| `SubaruUnisiaJecsProtocols()` | projected `kIdentities` | 2 |
| `SubaruUnisiaJecsM32rKlineProtocols()` | split `kVariants` into `kVariants` (the four `_20/_30/_40/_70`) and `kBootModeReadVariants` (the two `_bootmode`); `FindVariant` searches both, in that order; export the projection of `kVariants` | 4 |
| `SubaruUnisiaJecsM32rBootModeProtocols()` | projected bootmode `kVariants` | 2 |
| `SubaruDensoMc68hc16y502BdmProtocols()` | `std::array{kProtocol}` | 1 |
| `SubaruTcuHitachiM32rCanProtocols()` | `std::array{kProtocol}` | 1 |
| `SubaruDensoMc68hc16y502Protocols()` | new `kProtocols{"sub_ecu_denso_mc68hc16y5_02", "…_ecutek", "…_tpu"}` replaces the `!=` chain at `:45` | 3 |
| `SubaruDensoSh705502Protocols()` | new `kProtocols{"sub_ecu_denso_sh7055_02", "…_ecutek"}` replaces the chain at `:20` | 2 |
| `SubaruHitachiSh72543rCanProtocols()` | new `kProtocols{kProtocol, "sub_ecu_hitachi_sh72543r_can_recovery"}` replaces the chain at `:31` | 2 |
| `SubaruHitachiSh7058Protocols()` | `std::array{kProtocol}` (one list for both SH7058 builders) | 1 |
| `DensoSh705xEepromProtocols()` | new explicit array of the six `sub_ecu_eeprom_denso_*` catalog names (`sh7055_kline`, `sh7058_kline`, `sh7055_densocan`, `sh7058_densocan`, `sh7058_can`, `sh7058_can_diesel`); builder unchanged | 6 |

**Interfaces:**
- Consumes: `detail::ProtocolIdsOf` (Task 1).
- Produces: each function above, declared `std::span<const std::string_view> <Name>Protocols();`, returning a span over a namespace-scope `constexpr` array in its `.cpp`. Total: 27 functions, 60 ids.

- [ ] **Step 1: Export the 23 lists that only expose or project an existing table** (every row except the three marked "replaces the chain" and EEPROM). Builders are untouched apart from the Unisia K-Line table split.
- [ ] **Step 2: Replace the three `if` chains** (MC68 02, SH7055 02, SH72543R) with `std::ranges::contains(kProtocols, protocol)`. Keep each `Fail(...)` call (kind and text) exactly as it is.
- [ ] **Step 3: Add `DensoSh705xEepromProtocols()`.**
- [ ] **Step 4: Run** `bazel test //src/backend/flash/...`. Expected: PASS, and no test file changed.
- [ ] **Step 5: Commit** `refactor(flash): families export the protocol ids they accept`.

### Task 3: Exact flash routes and the routing test

**Files:**
- Modify: `src/backend/flash/workflow/flash_workflow.h:127-133`
- Modify: `src/backend/flash/workflow/flash_workflow.cpp:1004-1146`: replace `RouteMatch`, `Route` and `kRoutes`, and keep the `CreateWorkflow` constants and `MakeSubaruHitachiSh7058`.
- Test: `src/backend/flash/workflow/flash_workflow_test.cpp`

**Interfaces:**
- Consumes: the 27 `…Protocols()` functions (Task 2).
- Produces:
  - `static std::vector<std::string_view> FlashWorkflowFactory::RoutedProtocolIds();`: every id of every flash route, duplicates kept, in registry order.
  - The private `struct FlashRoute { std::span<const std::string_view> protocols; CreateWorkflow create; };` and `std::span<const FlashRoute> FlashRoutes()`, a function-local static array with 27 rows.
  - `TryCreate` picks the route whose `protocols` contains `request.protocol.name` exactly. Replace its header comment with: "A null result means no flash route names this protocol id. A routed protocol returns a workflow even when preflight will fail."

- [ ] **Step 1: Write the failing tests** in `flash_workflow_test.cpp`. Define a helper `std::vector<const config::ProtocolSpec*> CatalogFlashProtocols()` that returns the built-in catalog entries with `read || test_write || write`.
  - `TEST(FlashRouteTest, everyCatalogFlashProtocolHasExactlyOneRoute)`: for each spec, `std::ranges::count(FlashWorkflowFactory::RoutedProtocolIds(), spec->name) == 1`, with `SCOPED_TRACE(spec->name)`.
  - `TEST(FlashRouteTest, everyRoutedIdIsACatalogFlashProtocol)`: for each id, the catalog's `FindProtocol(id)` is non-null and flags at least one operation.
  - `TEST(FlashRouteTest, everyFlashProtocolRoutesToAWorkflowThatAcceptsIt)`: for each spec, build the request from the **real** `ProtocolSpec` with `FlashOperation::kRead` when the catalog flags Read (every flash protocol except the boot-mode ones is expected to), otherwise the first flagged operation with `image = bytes::Bytes(1, 0xff)`; `paths = CatalogPaths()`. Populate an `InMemoryFileRepository` with a 4-byte file for `spec->kernel` when it is non-empty. Then call `DriveToAttemptOrFailure(workflow)`, which calls `Next()`, answers every `FlashPromptStep` with `kAccept`, and stops at the first `FlashAttempt`, `FlashFailureStep` or `FlashCompletedStep`, with at most 16 steps. Assert the workflow is non-null, and that the result is not a `FlashFailureStep` with `kUnsupported`, nor, for Read, with `kInvalidConfig`. (A placeholder image can fail image checks with `kInvalidConfig`, so Write and TestWrite exclude only `kUnsupported`.) A `kInvalidConfig` on Read from a real catalog spec is a finding: stop and report it rather than skip it.
  - `TEST(FlashRouteTest, lookalikesOutsideTheCatalogHaveNoRoute)`: `CreateWorkflow(Request(name)) == nullptr` for `"mitsu_ecu_m32r_can_typo"`, `"sub_ecu_denso_mc68hc16y5_02_bdm_x"`, `"sub_ecu_unisia_jecs_20_bootmode_x"`, `"sub_ecu_denso_sh7055_04_x"`, `"sub_ecu_eeprom_denso_sh7058_can_x"` and `"mitsu_ecu_m32r_kline_mut_dma"`.
- [ ] **Step 2: Run** `bazel test //src/backend/flash/workflow:flash_workflow_test`. Expected: compile FAIL, `RoutedProtocolIds` not declared.
- [ ] **Step 3: Implement** `FlashRoutes()`, `RoutedProtocolIds()` and the exact `TryCreate`, with 27 rows. The EEPROM row uses `DensoSh705xEepromProtocols()`; the SH7058 row uses `SubaruHitachiSh7058Protocols()` with `kSubaruHitachiSh7058`. Delete `RouteMatch`, `Route`, `kRoutes` and their ordering comments.
- [ ] **Step 4: Run** the test target. Expected: the four new tests PASS. `invalidColtSuffixIsRecognizedButFailsPreflight` FAILS, as the spec predicts.
- [ ] **Step 5: Remove the route tests the new tests own.** In `flash_workflow_test.cpp`, delete:
  - `recognizesEveryPortableFamilyPrefixAndLeavesLegacyAlone`
  - `invalidColtSuffixIsRecognizedButFailsPreflight`
  - `mc68TpuProtocolIsClaimedByPortableRoute`
  - `sh7055ProtocolIsClaimedByPortableRoute`
  - `mc68Revision04HasNoRoute`
  - `unisiaJecsM32rLookalikesStayUnrouted`

  For `densoCanRoutesOnlyTheFiveExactProtocols`, `petrolRoutesOnlyTheFiveExactProtocols`, `dieselRoutesOnlyTheTwoExactProtocols`, `tcuRoutesOnlyTheTwoExactProtocols`, `unisiaJecsM32rRoutesTheFourExactProtocols` and `unisiaJecsRoutesOnlyExactProtocolMcuPairs`, delete only the assertions about which names route (null or non-null from `CreateWorkflow`). Keep any assertion about prompts, attempts, plan content or a builder's MCU or lookalike rejection: it stays, renamed if its name no longer describes it. If a lookalike that a family test rejected is no longer reachable through the factory, move that rejection to the family's `*_plan_test.cpp` against the builder.
- [ ] **Step 6: Run** `bazel test //src/backend/flash/...`. Expected: PASS.
- [ ] **Step 7: Commit** `refactor(flash): route flash workflows by exact protocol id`.

### Task 4: Operations agree with the catalog flags

**Files:**
- Test: `src/backend/flash/workflow/flash_workflow_test.cpp`

**Interfaces:**
- Consumes: `CatalogFlashProtocols()` and `DriveToAttemptOrFailure` (Task 3).

- [ ] **Step 1: Write the test** `TEST(FlashRouteTest, routedWorkflowsAcceptExactlyTheOperationsTheCatalogFlags)`. For each catalog flash protocol and each of `kRead`, `kTestWrite` and `kWrite`, build the request as in Task 3 and drive it. "Rejected" means a `FlashFailureStep` with `kUnsupported`.
  - If the catalog flags the operation, assert it is not rejected.
  - If the catalog does not flag it, assert it is rejected.

  Before the loop, declare `constexpr std::array kKnownFlagMismatches = std::to_array<std::pair<std::string_view, FlashOperation>>({...});`, with a comment on each entry naming the mismatch. Skip those pairs with `SCOPED_TRACE`, and assert at the end that every skip-list entry still mismatches, so a fixed entry fails until it is removed.
- [ ] **Step 2: Run** the test. If it fails, add each reported pair to `kKnownFlagMismatches` with a one-line reason. Do not change any builder or catalog flag in this PR. Re-run. Expected: PASS.
- [ ] **Step 3: Record findings.** For each skip-list entry, add an item to `docs/tech-debt.md` under flash, saying which side is wrong if the code makes that clear, and otherwise "undetermined".
- [ ] **Step 4: Commit** `test(flash): routed workflows accept exactly the catalog's operations`.

### Task 5: Gates and PR

- [ ] **Step 1:** `python3 scripts/gazelle_check.py --fix`, then review the BUILD diff. The new deps on `flash_workflow_test` (the family plan targets, if any) are expected.
- [ ] **Step 2:** `bazel test --config=release //...` and `prek run --all-files`. Expected: PASS.
- [ ] **Step 3:** `bazel run //:clang_tidy -- //src/backend/flash/...`. Expected: no findings; this repo's macOS-vs-Linux GCC warnings are caught here.
- [ ] **Step 4:** Update the spec's "Starting point" only if a number in it turned out wrong. Leave the spec otherwise as is; it stays until PR 3 closes it.
- [ ] **Step 5:** Push `refactor/flash-exact-routes` and open the PR: summary as a `diff` of `kRoutes` → `FlashRoute`, with evidence from the before/after test runs and the skip list. Merge danger: two-way door; blast radius "flash routing".
