# ADR 0007: Use Bazel as the CI Source of Truth

## Status

Accepted and implemented.

## Context

During the Bazel migration, surrounding CI still selected some toolchain and
dependency inputs independently for builds, packaging, coverage, and platform
checks. Those jobs could drift even as Bazel became the target graph.

## Decision

Treat Bazel configuration as the source of truth for CI build targets,
dependency choices, platform compatibility, tests, coverage inputs, compile
commands, and packaging binaries.

Some host-tool values must also be present in GitHub Actions. In particular,
workflow `QT_VERSION` values must match the Qt version in `MODULE.bazel`.
Update Bazel first, then update surrounding CI configuration to follow it.

## Consequences

- Local, pull request, and release builds use the same Bazel targets.
- SonarCloud and clang-tidy consume Bazel-derived compile commands.
- Coverage enumerates Bazel test targets, and release packages are assembled
  from Bazel outputs.
- Host-tool installation and packaging scripts remain outside Bazel actions;
  duplicated values such as `QT_VERSION` require explicit consistency checks
  and review.
