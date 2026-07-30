# ADR 0011: Use std::format

## Status

Accepted

## Context

There is a lot of string concatenation code, especially in error messages construction.
std::format allows for more explicit message structure.

## Decision

Use std::format as the preferred way of constructing complex strings

## Consequences

- Searchable error message templates
- Performance impact can be disregarded on error path

## Follow-up

- Adopted 2026-07-30: converted the remaining non-compliant message-construction
  call sites in `src/algorithms` and `src/backend` (the portable `std::string`
  layer) to `std::format`.
- Deliberately out of scope: `QString`-based code in `src/ui`, `src/platform`,
  and the legacy `src/backend/definitions/file_actions.cpp` monolith.
  `file_actions.cpp` is already slated for extraction into portable use cases
  by `docs/modularization-plan.md` step 5 and `docs/tech-debt.md`'s "Split
  `FileActions`" item; converted pieces adopt `std::format` as new portable
  code lands, without a separate action here.
- Also deliberately out of scope: pure path/filename-joining concatenation
  (e.g. `src/backend/config/config_paths.cpp`), since it isn't "constructing
  a complex string" in the sense this ADR targets — no message text, no
  readability/searchability gain from a format string.
- No dedicated CI enforcement was added; this ADR is enforced by PR review,
  same as every other already-accepted ADR in this repo.
