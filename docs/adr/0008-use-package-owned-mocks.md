# ADR 0008: Use Package-owned mocks

## Status

Accepted

## Context

For testability purposes we often define interfaces and then need simple
testing mocks for them. Mocks themselves need simple tests to reduce
blast radius.

## Decision

When a package defines an interface, it shall create a `testing/` subpackage
and define each mock as its own target (`cc_library`, `testonly = True`).

Each mock shall be accompanied by its own test target (`cc_test`); see
`src/backend/ports/testing/` for reference.

## Consequences

- Single source of truth, no behavioural drift.
- Clear ownership.

## Notes

The rule as it applies when writing a test is stated in
[the coding style guide](../coding-style.md).
