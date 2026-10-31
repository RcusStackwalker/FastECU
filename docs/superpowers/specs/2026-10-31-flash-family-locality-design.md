# Flash family locality: design

Date: 2026-10-31. Terms (**flash family**, **plan type**, **flash route**) are
defined in the [flash glossary](../../../src/backend/flash/GLOSSARY.md).
Governing decisions: [ADR 0030](../../adr/0030-erase-the-family-plan-type.md)
(the family plan is type-erased), [ADR 0017](../../adr/0017-generate-bazel-targets-with-gazelle.md)
(Gazelle owns BUILD targets) and [ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md)
(transcripts and goldens are the wire contract).

This design continues the
[flash route registry design](2026-10-31-flash-route-registry-design.md). It
keeps that design's Decisions 1–5 (exact routes, protocol lists owned by each
family, plan-type facts next to each plan type) and the routing test. It
supersedes Decision 6 (two headers per family), Decision 7 (four central
entries per family), and the non-goals on per-family preparation and the
`flash_types.h` include fan-out.

## Intent

Adding a flash family should stay inside the family's own folder. After the
route registry work, a family still needs, outside its folder: its include and
its `FamilyPlan` alternative in `flash_types.h`, its enum value and name row in
`flash_family.h`, a registry row with its workflow alias, and, for three
families, preparation code in `workflow/flash_workflow.cpp`. It also needs two
headers, because the variant forces `flash_types.h` to include every
`<f>_types.h`.

## Starting point (master `9ba93873`)

- `FamilyPlan` is a `std::variant` of 30 plan types. It is never visited for
  behaviour: each family reads its own plan types with `std::get` or
  `std::get_if`, about 30 call sites.
- `ValidateAndBuild` (`flash_validation.cpp`) visits it twice: to check that
  `FlashPlanFields::family` and `::transport` match `FamilyTraits<T>`, and to
  read `kFamilyRequiresKernel<T>`.
- `flash_types_test.cpp` asserts the variant has 30 alternatives and checks
  each against a hand-written `KernelPlanTypes` list, so a new plan type must
  decide whether it uploads a kernel.
- Each builder writes `family` and `transport` into `FlashPlanFields` by hand.
- Family and plan type are not one to one: Hitachi SH7058 has a K-Line read
  plan type and a CAN write plan type.
- `flash_workflow.cpp` holds `PrepareMc68` (with its placeholder kernel),
  `PrepareBdm` and the boot-mode `BuildPlans`.

## Decisions

1. **The family plan is type-erased** (ADR 0030). `FamilyPlan` becomes a class
   holding a `std::any`. Its one constructor, `template <class P>
   FamilyPlan(P)`, records `FamilyTraits<P>::kFamily`, `::kTransport` and
   `::kRequiresKernel`. Existing assignments such as
   `fields.family_plan = DensoSh705xEepromCanPlan{…}` compile unchanged.
2. **A typed accessor replaces `std::get_if`.** `FlashPlan::FamilyPlanAs<P>()`
   returns `const P*`, null on a type mismatch. Executors that used
   `std::get` keep their behaviour: plan admission has already run the
   family validator, which rejects a null.
3. **The kernel requirement is a required trait.** `kFamilyRequiresKernel<T>`
   is replaced by `static constexpr bool kRequiresKernel` in each
   `FamilyTraits` specialization, with no default. All 30 plan types state it:
   9 `true`, 21 `false`. A plan type that has not decided fails to compile at
   the `FamilyPlan` constructor. `KernelPlanTypes` and the alternative-count
   assertion go.
4. **Family and transport come from the plan type.** `FlashPlanFields::family`
   and `::transport` are removed; `FlashPlan::Family()` and `Transport()` read
   the facts `FamilyPlan` recorded. A plan whose transport disagrees with its
   plan type can no longer be built, so the validation tests for that case go.
5. **One header per family.** `flash_types.h` includes no family header, so
   the cycle that rejected one header per family is gone. `<f>_plan.h` keeps
   the name that pairs with `<f>_plan.cpp` under Gazelle's `cc_group unit`,
   and takes the plan struct and its traits from `<f>_types.h`. Every
   `<f>_types.h` and its target is deleted.
6. **Per-family preparation moves into the family.** `PrepareMc68`,
   `PrepareBdm` and the boot-mode `BuildPlans` move next to their family's
   builder. The order of identity validation, file reads and prompts stays
   exactly as it is; unchanged transcripts and goldens prove it. A move that
   would need any order change stops for review instead.
7. **What stays central.** The `FlashFamily` enum value and its name row, side
   by side in `flash_family.h` with the `consteval` order check. Several
   call sites branch on the enum, it names families in logs, and it is the one
   list of every family. In `workflow/`, one registry row and its workflow
   alias: workflow templates are staging policy, and `workflow/` depends on
   `ecu/`, not the other way round.

## Tests

- The routing test over the built-in catalog, and
  `BuiltinCatalogCapability.EveryOfferedOperationIsAcceptedAndEveryOtherIsUnsupported`,
  stay as they are. Together they reach every builder, so every plan type is
  constructed in a test.
- `flash_types_test.cpp` keeps the per-plan-type kernel expectations it already
  has, read from `FamilyTraits<T>::kRequiresKernel`.
- New tests for `FamilyPlan`: it records the traits of the type it wraps, and
  `FamilyPlanAs` returns null for a different plan type, including SH7058's
  sibling plan type.
- Wire transcripts and plan goldens do not change in any step.

## Behaviour changes

None. No wire behaviour, prompt order, plan content or routing changes.

## Delivery

Each step is a feature branch and PR, green on the full CI:

0. **Spec and ADR.** This document, ADR 0030, and pointers from the route
   registry design.
1. **Erasure.** `FamilyPlan` class, the accessor, `ValidateAndBuild`, the
   required kernel trait and the call sites. `flash_types.h` stops including
   family headers. Decisions 1–3.
2. **Derived family and transport.** Decision 4. Touches every builder.
3. **One header per family.** Decision 5. Mechanical.
4. **Preparation moves.** Decision 6, one PR each for MC68, BDM and boot-mode.

## Non-goals

- Removing the `FlashFamily` enum, or generating it.
- Moving workflow aliases or registry rows out of `workflow/`.
- Stating each single-window family's wire identity once.
- Changing the catalog's shape, or letting catalog entries name a family.

## Done when

Adding a family touches its own folder (one `<f>_plan.h`, its plan source with
any preparation, its executor), one registry row with its workflow alias, and
two lines in `flash_family.h`. `flash_types.h` includes no family header. A
plan type without `FamilyTraits` or a kernel decision fails to compile, and the
routing test still fails on a protocol with no route or a route with no
protocol.
