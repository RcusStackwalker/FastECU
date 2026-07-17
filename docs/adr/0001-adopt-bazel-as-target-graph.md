# ADR 0001: Use Bazel as the Target Build Graph

## Status

Accepted

Accepted 2026-07-17: qmake removed; Bazel is the sole build graph for the
application, tests, release packaging (via scripts/package-*.{sh,ps1}),
SonarCloud (compile_commands.json), and coverage.

## Context

FastECU currently has more than one build path. qmake remains the primary path
for packaging and several tests, while Bazel already models the application and
test targets.

This split creates avoidable maintenance cost:

- Source lists, dependency wiring, and platform behavior can drift.
- Local developer workflows can differ from release jobs.
- Dependency setup is harder when qmake, Bazel, and CI scripts each own part of
  the build graph.
- Regression coverage is weaker when one path is only exercised by releases.

## Decision

The desired final state is to abandon qmake for project builds in favor of a
single Bazel-based build graph.

Bazel should become the source of truth for application targets, tests,
third-party dependencies, platform selects, generated Qt artifacts, clang-tidy
analysis, and release packaging inputs once packaging is migrated.

## Consequences

Positive consequences:

- One build graph reduces drift between local development, pull request CI, and
  release verification.
- Bazel gives a clearer model for libraries, generated sources, platform
  selects, and third-party dependencies than hand-maintained qmake lists.
- Bazel can run clang-tidy from the same target graph used for compilation, so
  static analysis sees the same sources, includes, and generated headers.
- Dependencies can be represented once in `MODULE.bazel` instead of split
  across qmake and CI scripts.

Negative consequences and risks:

- qmake cannot be removed immediately because release packaging still depends on
  it.
- Bazel rules for Qt packaging and deployment must be mature enough first.
- Developers who currently rely on qmake will need documented Bazel commands
  and setup instructions.
- CI may initially do duplicate work while Bazel and qmake paths overlap during
  migration.

## Migration Notes

The migration should be incremental:

1. Keep qmake and Bazel source lists synchronized while both paths exist.
2. Add new tests, reusable build structure, and clang-tidy checks in Bazel first.
3. Move third-party dependencies into Bazel module declarations where practical.
4. Port release packaging verification to Bazel-backed artifacts.
5. Remove qmake CI jobs and project files after Bazel covers equivalent build,
   test, and packaging behavior.
