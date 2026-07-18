# ADR 0001: Use Bazel as the Target Build Graph

## Status

Accepted and implemented.

The migration completed on 2026-07-17. Bazel is the sole build graph for the
application, tests, release packaging, SonarCloud compile commands, coverage,
and clang-tidy. The qmake project files and source-list synchronization checks
were removed after the Bazel paths covered their responsibilities.

## Context

Before this decision, FastECU maintained overlapping qmake and Bazel graphs.
Packaging and several tests used qmake while Bazel modeled an increasing share
of the application and test targets. The split allowed source lists, dependency
wiring, platform behavior, and local/CI workflows to drift.

## Decision

Use Bazel as the only project target graph. Bazel owns application and test
targets, third-party dependencies, platform selects, generated Qt artifacts,
compile commands, static-analysis inputs, and release-package build inputs.

Platform packaging remains in `scripts/package-macos.sh` and
`scripts/package-windows.ps1`; both scripts build their binaries from Bazel
targets before invoking the platform Qt deployment tools.

## Consequences

Positive consequences:

- One graph defines what builds in local development, pull requests, and
  releases.
- Qt generation, platform sources, dependencies, coverage, and analysis share
  the same target configuration.
- `MODULE.bazel` and the Bazel extensions own project dependencies instead of
  duplicating them across project formats.

Costs and remaining risks:

- Developers need Bazel/Bazelisk and the platform host tools documented in the
  README and CI workflows.
- Qt deployment and platform runtime collection still happen in packaging
  scripts outside Bazel actions.
- Much of the application remains in the broad `fastecu_core_common` target;
  target decomposition is tracked in `docs/tech-debt.md`.
