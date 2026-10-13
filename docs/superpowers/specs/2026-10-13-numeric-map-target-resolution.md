# Numeric map target resolution

Status: agreed design, 2026-10-13. Implementation has not started.
This specification records the architecture review and subsequent design
decisions; it is not authorization to implement them.

## Purpose

Concentrate definition interpretation and numeric target preparation so decoding,
copying, and editing use consistent metadata without repeating ownership and
target-selection rules. Improve test locality by sharing repeated fixture setup
and preserving behavioral coverage at the interfaces callers use.

The existing copy/edit interfaces already have depth and portable tests. The
value of this work is in absorbing metadata and lifetime knowledge, not merely
extracting repeated switches.

## Current evidence

- [Numeric copy](../../../src/backend/calibration/session/numeric_copy_use_case.cpp)
  and [numeric edit](../../../src/backend/calibration/session/numeric_edit_use_case.cpp)
  repeat session lookup, definition/map checks, fresh map decoding, numeric-run
  selection, and body/X/Y geometry.
- [Decoding](../../../src/backend/calibration/calibration_service.cpp),
  [placement](../../../src/backend/calibration/map_placement.cpp), and
  [field collection](https://github.com/RcusStackwalker/FastECU/blob/e444fd75cdc48743fd0ed877c9e79bde9bc71831/src/backend/calibration/session/map_element_fields.cpp)
  repeat parts of storage and definition interpretation. The review found no
  demonstrated disagreement between decode and edit metadata resolution.
- [MapElementFields](https://github.com/RcusStackwalker/FastECU/blob/e444fd75cdc48743fd0ed877c9e79bde9bc71831/src/backend/calibration/session/map_element_fields.h)
  exposes borrowed strings through `Spec() const&`, but deletes a differently
  named `spec() const&&`. The intended protection against temporaries therefore
  does not apply. Current production calls retain the owner; no existing
  dangling call was found. Relocation of an owner after obtaining a view is a
  separate lifetime concern.
- Copy/edit tests duplicate their numeric grid, ROM bytes, configuration,
  workspace construction, and session installation.

## Agreed structure

### Shared definition interpretation

A portable internal module owns interpretation of storage, byte order,
conversion expressions, and geometry used by decoding and editing. It is
independent of workspace/session state. Body fields retain their precedence
over scaling defaults; axes retain their resolved-axis semantics.

Placement can consume shared structural facts where appropriate, while retaining
its existing validation contract. Sharing facts must not equate decoder axis
eligibility with placement eligibility: placement checks declared axes even
where decoding omits an axis.

### Numeric target preparation

A cooperating internal module resolves the session and selected map body or
axis, performs the existing whole-map validation, and owns freshly resolved
metadata and decoded cells for one copy/edit operation. Borrowed views remain
inside the implementation; callers do not coordinate their lifetimes. There is
no persistent cache of metadata or decoded values.

The implementation must remain valid when owning results move. Storing pointers
or string views into movable owned data requires an explicit safe construction
strategy; merely fixing the accessor spelling is insufficient for the design.

Existing decode, copy, and edit operation interfaces remain the behavioral test
surface. Exact internal C++ types and functions are implementation choices,
subject to these ownership and dependency constraints. This in-process seam
does not require a new polymorphic adapter.

## Behavioral constraints

- Preserve whole-map structural validation. A structural failure in an axis
  continues to prevent an operation on the map body.
- Resolve current ROM bytes on every operation. Preserve stable session IDs,
  closed-session outcomes, and unavailable-target outcomes.
- Preserve the distinction between an invalid numeric cell and a structural
  map failure. Absolute replacement may repair an invalid cell when encodable;
  operations requiring its current value still fail.
- Validate operation-specific fields and requested values when consumed.
  Copying does not acquire an encoding, limit, or increment requirement.
  Absolute assignment does not acquire an increment requirement.
- Preserve atomic edits, pre-mutation validation, memory-map write guards,
  unchanged-edit outcomes, dirty state, copy precision, and paste clipping.
- Preserve static axes, absent axes, selectable maps, and diagnostic behavior
  while reorganizing shared interpretation.

The owning contracts are [calibration compatibility](../../reference/calibration-compatibility.md),
[ADR 0020](../../adr/0020-model-rom-memory-as-a-memory-map.md), and the
[layering decision](../../design-notes.md#layering).

## Corpus evidence for operation-specific validation

A read-only local scan inspected 969 XML files across three sources:

| Local source | XML files | Numeric scaling declarations in parsed definitions |
| --- | ---: | ---: |
| Extracted ECUFlash `rommetadata` | 745 | 1,640 |
| `mmc-definitions` | 59 | 2,198 |
| Historical `evom-jcsbanks/ecuflash-xml` checkout | 165 | 1,916 |

Nineteen files failed XML parsing; three archive files were not definition
documents. Counts include repeated declarations and are not distinct resolved
scalings. The scan found no malformed nonempty numeric increments or limits, or
reversed limits, among the inspected numeric scaling declarations. It was not
a full FastECU load/inheritance audit and does not establish general corpus
compatibility. These external corpora are not repository test dependencies.

A concrete example in the local `mmc-definitions` corpus is
`roms/m32r/c7280010/c7280010.xml`: line 65 defines `RPMLimit`, and line 375
uses it for `Rev Limit`. Both conversion directions are `7500000/x`, with
declared limits `[0, 9000]` and increment `60`.

- Raw `1000` decodes to `7500`, which can be copied.
- Assigning `7500` encodes to `1000`.
- Assigning `0` divides by zero despite satisfying the declared limits.

These are deductions from the definition, not results from running a ROM.
They support input-dependent encoding validation. They do not demonstrate
malformed increment metadata in use. Existing tolerance for unrelated malformed
edit-only metadata is preserved as a compatibility constraint.

## Test restructuring

1. Share the numeric grid/ROM construction and workspace/session setup used by
   copy/edit suites in package-owned, `testonly` support. Keep operation helpers,
   expected values, and operation-specific assertions close to each test.
2. Retain explicit placement layouts. Decoder, placement, and session fixtures
   express different preconditions; avoid a universal configurable fixture.
3. Retire tests tied solely to removed field-collection machinery. Preserve
   substantive cases for body fallback, axis expressions/stride, limits, and
   increments through meaningful module or operation interfaces.
4. Consolidate repeated tests of the same metadata rule while retaining evidence
   that each caller uses the rule correctly. Preserve copy/edit integration
   coverage, including copy-to-paste unchanged behavior.
5. Add focused lifetime regression coverage for temporary/relocation hazards
   appropriate to the resulting interface, and cross-operation coverage of
   whole-map rejection and fresh values after edits. Do not reproduce every
   internal field assignment in tests.

Test helpers follow the [testing conventions](../../coding-style.md#tests).
Regenerate managed BUILD files under [ADR 0017](../../adr/0017-generate-bazel-targets-with-gazelle.md);
use established visibility groups without widening them to bypass guards.

## Documentation and delivery

The final paragraph of the current
[decoded-values and map-edits contract](../../reference/calibration-compatibility.md#decoded-values-and-map-edits)
says field resolution and patch application remain in the desktop edit adapter.
The backend already owns both. This disagreement was reported during review;
correct the stale paragraph alongside implementation and document the new owners.

Implementation requires focused calibration tests and the applicable repository
formatting, Gazelle, static-analysis, and full verification gates. Hardware wire
behavior is outside this refactor. No hardware qualification is claimed.

After delivery, extract enduring contracts into their owning documents and
remove this specification only after its original contents are preserved in
delivered Git history, following the [documentation lifecycle](../../README.md#recover-completed-work-and-deleted-source-citations).
