# Logging selection rewrite interview

Status: confirmed by the user; implementation in progress.
The user confirmed the consolidated boundary and authorized implementation.

## Settled correction

Preserve PR #577's firmware-backed little-endian MUT/DMA free-form request and
response interpretation. The previous big-endian implementation has no recorded
working-hardware evidence and is not a supported compatibility dialect. Remove
`legacy-be` and the selector introduced to retain it from the rewrite design.
The analyzed firmware permits at most 96 entries and 96 requested response
bytes. Retain this evidenced guard independently of the removed selector; do
not represent the 255-entry count-field limit as supported ECU capacity.

## Decision tree

- Preservation standard: distinguish evidenced contracts, user requirements,
  imported definition compatibility, and accidental implementation behavior.
- Selection lifecycle: permit pending edits during a run or lock selection edits.
- Acquisition and presentation: identities, repeated positions, switch/gauge scope.
- Definition interpretation: raw numeric assembly, conversions and XML forms.
- Output: schema, run/file lifetime, formatting and error policy.
- Delivery: rewrite boundaries, dependencies and acceptance evidence.

## Settled interview decisions

- Q1: preserve complexity only for an explicit user requirement, evidenced protocol
  contract, or identifiable definition/workflow compatibility. Existing code and
  tests alone do not establish intended behavior.
- Q2: selection editing during a logging run is a user requirement. Do not lock
  editing. Edits apply at the next run, as settled in Q3.

- Q3: edits save next-run choices; active acquisition, display bindings and CSV
  remain fixed. Show the apply-after-restart indicator only when choices differ.
- Q4: CSV exports each unique acquired measurement once, in stable selection
  order. Keep escaping and UTF-8; remove the extra trailing empty column.
- Q5: compile expressions during preparation and evaluate readings with checked
  arithmetic. Remove validation using arbitrary trial input values.

- Q6 (width syntax): support only `<address length="…">`. Update bundled XML
  examples and authoring documentation in a separate prerequisite PR. Remove
  parameter-level width fallback and duplicate-width reconciliation. No migration
  path, compatibility alias, or migration-specific diagnostic is required.
  Switch syntax is settled in Q9; ordered SSM sources are settled in Q18.

- Q7: correct SSM binary numeric decoding in a separate prerequisite PR, using
  evidenced definition storage and byte-order semantics. Remove decimal-byte
  concatenation without a compatibility mode.
- Q8: use the first conversion by default in #562, retain alternatives in the
  imported definition, and defer operator unit selection to a separate feature.
- Q10: retain unresolved/known-unsupported selections visibly and block startup
  until corrected. Unknown support permits explicit selections.
- Q11: create a distinct collision-safe CSV for every run, with a fixed schema
  and no overwrite or append across runs.
- Q9: retain RomRaider top-level switch `byte`/`bit` syntax and remove the
  nested-address alternative. Follow the evidenced external schema; represent
  sample extraction and capability evidence separately internally. RomRaider
  uses the same external switch bit for both roles when `ecubyteindex` is present.

- Q12: acquire the union of gauge, Digital and switch selections; implement
  switch indicators and export values. A new gauge renderer remains outside scope.
- Q13: repeated display positions share acquisition by measurement identity.
  Distinct definitions remain separate acquisitions. Physical-read deduplication
  is unnecessary; do not schedule it as follow-up optimization.
- Q14: a value-dependent conversion failure makes only that measurement
  unavailable for the current sample; continue other measurements. CSV uses an
  empty cell and displays must not present an old value as current. Invalid
  expressions fail preparation; malformed packets remain protocol errors.
  Bound diagnostics to avoid repetitive log flooding.

- Q15: file logging remains editable during acquisition. Disable closes output;
  re-enable creates a fresh file with the active run's captured schema.
- Q16: file-open/write failure closes output and reports once while acquisition
  and live displays continue. Retry on explicit re-enable or the next run.
- Q17: deliver width syntax cleanup, SSM numeric decoding and expression
  compilation as separate prerequisite PRs. Rewritten #562 owns selection/support
  policy, gauge/switch acquisition, frozen run snapshots, unique CSV columns,
  per-measurement unavailable state and removal of obsolete MUT dialect machinery.

- Q18: accept both contiguous SSM base-address-plus-length sources and explicit
  ordered byte-address lists. Preserve order and source meaning; prepare both
  through one read-plan representation before acquisition.

## Confirmation boundary

All interview questions Q1–Q18 are settled, and the user confirmed the
consolidated delivery boundary before implementation. The existing PR-branch implementation and owning
contracts still contain superseded behavior; this note records the intended
rewrite and does not claim those changes have been implemented.

## Delivery boundary

Prerequisite PRs:

1. Canonical parameter width syntax, bundled XML examples and authoring docs.
2. Firmware/definition-backed SSM numeric decoding without decimal concatenation.
3. Compiled expressions with checked runtime evaluation, replacing trial inputs.

Rewritten #562:

- Selection/support policy with editable next-run choices and a frozen active run.
- Gauge/Digital/switch acquisition with identity-only sharing, switch indicators,
  and distinct definitions kept as distinct acquisitions.
- RomRaider switch syntax and both evidenced SSM source layouts.
- Unique-measurement CSV, collision-safe per-run files, editable file output and
  isolation of file failures from acquisition.
- Explicit per-measurement unavailable samples for conversion failures.
- Removal of MUT dialect selection while retaining little-endian interpretation
  and the analyzed firmware's 96-entry/96-requested-byte guard.

Excluded: migration compatibility modes, physical-read deduplication, alternative
unit selection UI, and a new gauge renderer.

## Source findings

RomRaider converts binary bytes to numeric values before scaling. Its unsigned
big-endian two-byte default makes 01 02 equal 258. It defaults to the first
conversion and permits operator selection of alternatives. These observations
support the settled Q7 and Q8 decisions.

Four-space selection XML indentation is inexpensive and avoids formatting churn.
Width compatibility is deliberately removed under Q6 despite bundled examples
using the old location; those examples will be corrected.

## Sources

- [Current logging contracts](../../reference/logging-contracts.md)
- [Firmware byte-order evidence](../../reference/logging-mut-freeform-wire.md)
- Original design: `2676ccdf`,
  `docs/superpowers/specs/2026-10-08-logging-selection-policy-design.md`.
- Corrective implementation: PR #577, commit `31a359fa`.

The linked PR-branch contracts still describe the disputed compatibility dialect;
they are evidence of the current proposal, not approval for its preservation.

## Q9 upstream switch findings

At pinned RomRaider revision `dafe0c36`, switch definitions use top-level `byte`
and `bit`. The parser uses `byte` as the one-byte sample address and `bit` for
sample extraction. It also uses the same switch `bit` with `ecubyteindex` for
capability filtering; switch `ecubit` is not read. Parameters instead use
`ecubyteindex`/`ecubit`. Nested address syntax for switches must therefore not
be described as the standard RomRaider switch form. The earlier suggestion
to discard top-level switch fields rested on an incorrect compatibility premise.
The internal sample/support distinction remains valuable. Q9 settles the external
syntax in favor of the RomRaider form.

Source: [RomRaider definition parser](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/logger/ecu/definition/xml/LoggerDefinitionHandler.java#L261).
