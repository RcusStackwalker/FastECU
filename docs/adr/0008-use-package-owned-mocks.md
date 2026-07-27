# ADR 0008: Use Package-owned mocks

## Status

Proposed

## Context

For testability purposes we often define interfaces and then need simple
testing mocks for them. Mocks themselves need simple tests to reduce
blast radius

## Decision

When a package defines an interface, it shall create a subpackage testing/
and define each mock as its own target (cc_library, testonly = True).

Each mock shall be accompanied by its own test target (cc_test), see
src/backend/ports/testing/ for reference

## Consequences

- Single source of truth, no behavioural drift
- Clear ownership

## Follow-up

- Find and convert non-compliant cases
