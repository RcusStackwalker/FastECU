# ADR 0012: Use std::ranges

## Status

Proposed

## Context

There are multiple ways of applying algorithms to collections: index loops, iterator algorithms, range algorithms. We'd like to converge on a less error-prone way and concise way.

## Decision

Use std::ranges, views, algorithms instead of iterators or raw index loops.

## Consequences

- Built-in bounds checking
- Impossible to mismatch iterators

## Follow-up

- Find and convert non-compliant cases
