# ADR 0030: The Family Plan Is Type-Erased

## Status

Accepted. Design: the
[flash family locality spec](../superpowers/specs/2026-10-31-flash-family-locality-design.md).
Supersedes Decision 6 of the
[flash route registry design](../superpowers/specs/2026-10-31-flash-route-registry-design.md).

## Context

`FlashPlan` carried its family-specific part as `FamilyPlan`, a closed
`std::variant` of all 30 plan types. The variant forced `flash_types.h` to
include every family's `<f>_types.h`. Every family's plan source depends on
`flash_types`, so a family could not have one header: folding `<f>_plan.h` into
`<f>_types.h` either makes a Bazel dependency cycle or leaves the builders
unlinked under Gazelle's include-driven resolution
([ADR 0017](0017-generate-bazel-targets-with-gazelle.md)). Adding a family also
meant editing the variant and its include list, and an edit to any plan type
rebuilt everything downstream of `flash_types.h`.

The variant was never visited for behaviour. Each family reads only its own
plan types, with `std::get` or `std::get_if`. The two visits in
`ValidateAndBuild` read `FamilyTraits` and the kernel requirement, and a test
counted the alternatives to force a kernel decision on each new plan type.

## Decision

- `FamilyPlan` is a class holding a `std::any`. Its one templated constructor
  records the plan type's family, transport and kernel requirement from
  `FamilyTraits<P>` at the point of construction; `ValidateAndBuild` checks the
  recorded facts.
- The kernel requirement is a required `FamilyTraits` member with no default.
  A plan type without `FamilyTraits`, or without a kernel decision, fails to
  compile where it is wrapped.
- Families read their plan through a typed accessor on `FlashPlan` that
  returns a pointer, null on a type mismatch.
- `flash_types.h` includes no family header.

## Alternatives

- **Keep the variant (two headers per family).** It works, but leaves four
  central edits per family and the include fan-out, for exhaustiveness nothing
  uses.
- **Bend Gazelle ownership** (`alwayslink` plus `# keep` dependencies, or
  self-registration). It defeats include-driven dependency resolution, and
  self-registration brings static-initialisation order back.
- **A hand-rolled erased holder with a per-type key.** It avoids RTTI, which
  the build does not disable. The accessor is the one place to change if that
  ever matters.
- **A virtual base class for plan types.** It puts a hierarchy on what are
  plain aggregates and adds `dynamic_cast` for no gain over `std::any`.

## Consequences

- No list of all plan types remains. Coverage comes from the routing test over
  the catalog, which reaches every builder, and from the compile errors above.
- A family wrapping the wrong plan type is caught at run time by the accessor
  returning null, in that family's own validator, as before.
- One heap allocation per plan.
