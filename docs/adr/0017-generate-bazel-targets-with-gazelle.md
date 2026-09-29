# ADR 0017: Manage BUILD Files with gazelle_cc

## Status

Accepted

## Context

BUILD files were maintained by hand. Adding or renaming a source, or changing an
`#include`, meant editing `srcs`, `hdrs` and `deps` in step, and drift was only
found when a build broke.

## Decision

`gazelle` with the `gazelle_cc` extension generates `cc_library` and test targets
for the areas listed in the `args` of the `//:gazelle` target in the root
`BUILD.bazel`. Currently that is `src/algorithms`, without `qt_compat`.

- Grouping is `cc_group unit`; `cc_test` is mapped to `fastecu_portable_gtest`.
  `cc_group directory` was rejected: it merges the per-file tests and overlaps
  deliberately split libraries.
- Gazelle does not write `visibility`, `PORTABLE_PACKAGES` or the `qt_layer`
  group. A new package needs a hand-written `package(default_visibility = ...)`
  **before** the first run: without one, gazelle emits
  `visibility = ["//visibility:public"]` on the new targets.
- `scripts/gazelle_check.py` runs gazelle and fails if any `*.bazel` file changed,
  was created, or was deleted. It is a prek `pre-push` hook, and the `gazelle`
  job in `pr.yml` runs the same hook.
- To regenerate: `bazel run //:gazelle`, then review and `git add` the result.
- To preview without writing, run the built binary directly:
  `bazel-bin/gazelle_cc_binary_/gazelle_cc_binary -mode=diff -repo_root=. -build_file_name=BUILD.bazel src/algorithms`.
  Flags cannot be passed through `bazel run //:gazelle --`, because the runner
  places the rule's `args` (the scope path) before them.
- To widen: add a path to `args`, regenerate, and review the diff in its own pull
  request.

## Consequences

BUILD files under gazelle are reproducible from the sources, and CI rejects a
stale one. Gazelle adds a redundant `@googletest//:gtest` to tests
(`gtest_main`, supplied by the macro, already depends on it); this is accepted
because suppressing it would also give libraries `gtest_main`.

`fastecu_portable_gtest` deduplicates its `deps`, because gazelle lists
`byte_matchers` that the macro also adds and Bazel rejects a repeated label. A
configurable attribute reaches a symbolic macro as a `select()` and cannot be
deduplicated, so the macro's `deps` is not configurable: a test that needs
`select()` in `deps` must use another macro or `fastecu_gtest`.

Gazelle leaves an unused `cc_test` load behind when `map_kind` converts a raw
`cc_test`; buildifier removes it, and gazelle does not add it back.

This is an exception to [ADR 0002](0002-use-prek-for-fast-local-checks.md), which
reserves prek for fast checks: the hook needs Bazel, so it runs at `pre-push`
only and is filtered to C++ and BUILD file changes. `--no-verify` bypasses it
locally; CI is the gate.

Other macros (`qt_cc_library`, `fastecu_gtest`) need their own `map_kind` and moc
`hdrs` / `normal_hdrs` handling before their packages can be added.
