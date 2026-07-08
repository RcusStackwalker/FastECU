# ADR 0003: Use GitHub Actions for CI

## Status

Accepted

## Context

FastECU needs pull request and release checks on Windows, macOS, and Linux.
Those checks should run the same commands that are documented for local use.

## Decision

Use GitHub Actions as the canonical CI runner. CI should run `prek` first, then
the relevant Bazel build, test, analysis, and packaging verification jobs.

## Consequences

CI stays close to the repository and can validate the supported platform matrix.
Checks that need secrets, artifacts, or platform runners belong in GitHub
Actions rather than local hooks.
