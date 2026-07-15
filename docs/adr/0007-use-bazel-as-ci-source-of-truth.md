# ADR 0007: Use Bazel as the CI Source of Truth

## Status

Accepted

## Context

ADR 0001 makes Bazel the target build graph. Surrounding CI still runs qmake,
release packaging, coverage, and platform checks that install some tools
directly. Those jobs can drift if they choose toolchain inputs independently.

## Decision

Treat Bazel configuration as the source of truth for surrounding CI toolchain
and dependency decisions. For Qt, workflow `QT_VERSION` values must match
`MODULE.bazel`. Update Bazel first, then update surrounding CI to follow it.

## Consequences

- qmake, Bazel, and release workflows validate the same dependency assumptions.
- Workflow-only divergence must be temporary and documented.
- Some values remain duplicated until qmake and release packaging move behind
  shared scripts or Bazel-backed artifacts.
