# ADR 0002: Use prek for Fast Local Checks

## Status

Accepted

## Context

FastECU needs fast formatting and small repository checks before Bazel or
platform CI work starts. These checks do not need the full build graph.

## Decision

Use `prek` for formatters and small pre-commit checks where practical. GitHub
Actions should run `prek run --all-files` before expensive CI jobs.

## Consequences

Formatters and small checks stay in one fast local path, and CI can fail before
running Bazel. Checks that need generated files, target configuration, or
platform toolchains still belong in Bazel or dedicated GitHub Actions jobs.
