# Flash plan-type facts: PR 2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each flash plan type's family id, transport, validator binding and
kernel requirement sit in its family's `<f>_types.h`, and the kernel
requirement defaults to `false`.

**Architecture:** The pieces those facts need are the `FlashFamily` enum,
`TransportKind`, the three primary templates and a forward-declared
`FlashPlan`. These move out of `flash_types.h` into a new leaf header,
`flash/flash_family.h`. Each `<f>_types.h` includes that header and specializes
the templates next to its plan struct. A validator specialization in
`_types.h` only declares `Validate`, because `FlashPlan` is incomplete there;
its body moves verbatim into the family's `_plan.cpp`. `flash_types.h` keeps
the includes and the `FamilyPlan` variant.

**Tech Stack:** C++23, Bazel with Gazelle, GoogleTest, clang-tidy, prek.

**Spec:** `docs/superpowers/specs/2026-10-31-flash-route-registry-design.md`
(Decisions 4–5; Delivery item 2).

## Global Constraints

- Admission does not change: every plan type maps to the same family and
  transport, has the same validator, and has the same kernel requirement it
  has on master.
- No wire, prompt, plan-content or error-text changes. Validator bodies move
  byte for byte.
- Warnings are errors. No suppressions (`NOLINT`, `#pragma`).
- Gazelle owns BUILD targets: run `python3 scripts/gazelle_check.py --fix`
  after include changes.
- Every header stays self-contained and includes what it uses.
- The 9 plan types that require a kernel are:
  - `DensoSh705xEepromKlinePlan`
  - `DensoSh705xEepromCanPlan`
  - `SubaruDensoMc68hc16y5_02Plan`
  - `SubaruDensoSh7055_02Plan`
  - `SubaruDensoSh7058CanPlan`
  - `SubaruDensoSh7058CanDieselPlan`
  - `SubaruDensoSh705xDensoCanPlan`
  - `SubaruDensoSh705xKlinePlan`
  - `SubaruTcuDensoSh705xCanPlan`

## Review Focus

1. A plan type whose `FamilyTraits` specialization is missing or duplicated
   after the move must still fail to compile. The primary stays undefined.
2. A plan type that silently lost its kernel requirement must fail a test
   (Task 1's guard).
3. A `PlanValidator` specialization declared in `_types.h` but defined in no
   `.cpp` must fail to link in the executor tests, not pass by accident. The
   executor tests and `flash_workflow_test` link every family.
4. `_types.h` headers must not include `flash_types.h` or `flash_plan.h`
   (include cycle). Checked in Task 2 by grep.
5. The domain comments on the 21 `= false` specializations (why a family
   uploads no kernel) must survive, next to the family's traits.

---

### Task 1: `flash_family.h` and the kernel-requirement guard

**Files:**
- Create: `src/backend/flash/flash_family.h`
- Modify: `src/backend/flash/flash_types.h` (lines 52–143: `FlashFamily`, `kFlashFamilyNames`, `FlashFamilyNamesFollowEnumOrder`, `FlashFamilyName`, `TransportKind`; line 237 primary `FamilyTraits` and its comment; line 423 primary `kFamilyRequiresKernel` and its comment)
- Modify: `src/backend/flash/flash_plan.h:127-136` (primary `PlanValidator` and its comment)
- Test: `src/backend/flash/flash_types_test.cpp`

**Interfaces:**
- Produces: `flash_family.h`, which declares in `fastecu::flash`:
  - `enum class FlashFamily`, `kFlashFamilyNames`, `FlashFamilyName`;
  - `enum class TransportKind`;
  - `template <typename PlanT> struct FamilyTraits;` (no definition);
  - `template <typename PlanT> inline constexpr bool kFamilyRequiresKernel = true;` (it still defaults to true in this task);
  - `template <typename PlanT> struct PlanValidator;` (no definition);
  - `class FlashPlan;`.

  It includes only std headers. `flash_types.h` and `flash_plan.h` include it.

- [ ] **Step 1: Write the guard test.** Add `TEST(FlashTypesTest, exactlyTheKernelUploadingPlanTypesRequireAKernel)` to `flash_types_test.cpp`.
  - Over every `FamilyPlan` alternative (`std::index_sequence` over `std::variant_size_v<FamilyPlan>`), collect `FlashFamilyName(FamilyTraits<T>::kFamily)` for each `T` with `kFamilyRequiresKernel<T>`.
  - Assert, with `UnorderedElementsAre`, that the collected names are exactly:
    `"DensoSh705xEepromKline"`, `"DensoSh705xEepromCan"`, `"SubaruDensoMc68hc16y5_02"`, `"SubaruDensoSh7055_02"`, `"SubaruDensoSh7058Can"`, `"SubaruDensoSh7058CanDiesel"`, `"SubaruDensoSh705xDensoCan"`, `"SubaruDensoSh705xKline"`, `"SubaruTcuDensoSh705xCan"`.
  - Delete `FamilyRequiresKernelDefaultsTrueForExistingFamilies`, which this test supersedes. Keep the per-type `EXPECT_TRUE`/`EXPECT_FALSE` lines in the other tests.
- [ ] **Step 2: Run** `bazel test //src/backend/flash:flash_types_test`. Expected: PASS. It is a guard for Tasks 2 and 3, so it passes on master.
- [ ] **Step 3: Move** the declarations listed above into `flash_family.h`, keeping their comments.
  - Reword the two "right here, next to the variant" comments to say: the specialization lives in the plan type's `<f>_types.h`.
  - Leave the `FamilyPlan` variant and everything else in `flash_types.h`.
- [ ] **Step 4: Run** Gazelle, then `bazel test //src/backend/flash/...`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(flash): declare plan-type facts in a leaf header`.

### Task 2: Plan-type facts move into each `<f>_types.h`

**Files:**
- Modify: the 27 `<f>_types.h` headers included by `flash_types.h` (lines 14–40).
- Modify: `flash_types.h:226-416`: delete every `FamilyTraits` specialization.
- Modify: each family's `_plan.h` and the EEPROM `denso_sh705x_eeprom_common.h`: delete the `PlanValidator` specializations.
- Modify: each family's plan `.cpp`, which gains the `Validate` definitions.

**Interfaces:**
- Consumes: `flash_family.h` (Task 1).
- Produces: in each `<f>_types.h`, after each plan struct:
  ```cpp
  template <> struct FamilyTraits<XPlan>
  {
      static constexpr FlashFamily kFamily = FlashFamily::kX;
      static constexpr TransportKind kTransport = TransportKind::kY;
  };

  template <> struct PlanValidator<XPlan>
  {
      static Status Validate(const FlashPlan& plan);
  };
  ```
  - The `kFamily`/`kTransport` values are copied from `flash_types.h`.
  - Each `_types.h` also includes `src/backend/ports/result.h` for `Status`.
  - The `Status PlanValidator<XPlan>::Validate(const FlashPlan& plan)` definition goes in the family's plan `.cpp`, with the old inline body verbatim. That includes the SH7058 operation guards and the two EEPROM forwards.

- [ ] **Step 1: Move** the traits and validator specializations family by family, keeping their comments.
  - The SH7058 comment ("The two transports share one plan validator…") goes above the two SH7058 validator declarations.
  - The `FamilyTraits` rationale comment ("Deliberately has no primary definition…") stays with the primary in `flash_family.h`.
- [ ] **Step 2: Check** for an include cycle: `grep -l 'flash_types.h\|flash_plan.h' src/backend/flash/ecu/*_types.h src/backend/flash/eeprom/*_types.h` prints nothing.
- [ ] **Step 3: Run** Gazelle, then `bazel test //src/backend/flash/...` and `bazel run //:clang_tidy -- //src/backend/flash/...`. Expected: PASS, with 0 findings.
- [ ] **Step 4: Commit** `refactor(flash): plan-type facts live with each plan type`.

### Task 3: The kernel requirement defaults to false

**Files:**
- Modify: `flash_family.h`: set the primary to `false`, and reword its comment.
- Modify: `flash_types.h:418-499`: delete all 21 `= false` specializations.
- Modify: the `_types.h` headers of the 9 plan types in Global Constraints. Each gains `template <> inline constexpr bool kFamilyRequiresKernel<XPlan> = true;` after its `FamilyTraits`.
- Modify: the `_types.h` headers of the 21 kernel-free plan types. Each comment that sat on a deleted `= false` line moves above that type's `FamilyTraits` specialization, verbatim.
- Test: `flash_types_test.cpp`, which needs no change; Task 1's guard proves the flip.

- [ ] **Step 1: Flip** the primary to `false`. Comment: "Whether validate_and_build requires FlashPlanFields::kernel for this plan type. Defaults false; a plan type that uploads a kernel opts in, in its `<f>_types.h`."
- [ ] **Step 2: Add** the 9 opt-ins and delete the 21 opt-outs, moving their comments as listed under Files.
- [ ] **Step 3: Run** `bazel test //src/backend/flash/...`. Expected: PASS, including `exactlyTheKernelUploadingPlanTypesRequireAKernel` and `flash_validation_test`.
- [ ] **Step 4: Commit** `refactor(flash): plan types that upload a kernel opt in`.

### Task 4: Docs, gates and PR

- [ ] **Step 1:** In the spec, amend Decision 7. The enum value and its name row live in `flash_family.h`, beside each other; the include and the `FamilyPlan` alternative live in `flash_types.h`. There are still four entries, all checked at compile time.
  - Grep `docs/` and `src/backend/flash` for "flash_types.h" comments that now point at the wrong file, and fix them. `docs/reference/flash-contracts.md` lines 49 and 56 name `PlanValidator` only by behaviour; leave them.
- [ ] **Step 2:** `bazel test --config=release //...`, `prek run --all-files` and `bazel run //:clang_tidy -- //src/backend/flash/...`. Expected: PASS, with 0 findings.
- [ ] **Step 3:** Push `refactor/flash-plan-type-facts` and open the PR. The summary is a `diff` of one family's `_types.h` gaining its facts, plus `flash_types.h` losing about 280 lines. The evidence is the guard test and the release run. Merge danger: two-way door; blast radius "plan admission".
