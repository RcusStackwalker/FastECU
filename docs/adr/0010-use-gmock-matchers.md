# ADR 0009: Use gmock-matchers

## Status

Proposed

## Context

It's a common need to test a container elements for matching specific properties.
One way is to use standard algorithms and loops to describe expecations.
GoogleTest framwork offers wide variety of matcher functions for property testing.

## Decision

Use GoogleTest matchers (#include <gmock/gmock-matchers.h>) for property testing.

## Consequences

- Uniform style of describing expectations
- Expectation failure is self-describing without additional comments
- Whenever an expectation fails on the container, its contents will be printed

## Follow-up

- Find and convert non-compliant cases
