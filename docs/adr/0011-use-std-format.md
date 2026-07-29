# ADR 0009: Use std::format

## Status

Proposed

## Context

There is a lot of string concatenation code, especially in error messages construction.
std::format allows for more explicit message structure.

## Decision

Use std::format as the preferred way of constructing complex strings

## Consequences

- Searchable error message templates
- Performance impact can be disregarded on error path

## Follow-up

- Find and convert non-compliant cases
