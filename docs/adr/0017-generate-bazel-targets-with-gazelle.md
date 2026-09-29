# ADR 0017: Manage BUILD Files with gazelle_cc

## Status

Accepted

## Context

BUILD files were maintained by hand. Adding or renaming a source, or changing an
`#include`, meant editing `srcs`, `hdrs` and `deps` in step, and drift was only
found when a build broke.

## Decision

`gazelle` with the `gazelle_cc` extension generates `cc_library` and test targets
for the areas listed in `GAZELLE_ARGS`, shared by `//:gazelle` and `//:gazelle_diff` in the root
`BUILD.bazel`. Currently that is `src/algorithms`, plus
`src/backend/ports`, `src/backend/protocol`, `src/backend/checksum`,
`src/backend/diagnostics`, `src/backend/config`, `src/backend/definition`,
`src/backend/calibration`, `src/backend/logging`,
`src/backend/service_functions`, `src/backend/flash`,
`src/ui/desktop/calibration`, `src/ui/desktop/checksum` and
`src/ui/desktop/menu`, including their subpackages. The legacy Qt-backed
`src/backend/definitions` remains unmanaged.

- Grouping is `cc_group unit`; `cc_test` is mapped to `fastecu_portable_gtest`.
  The `qt_compat` package overrides the mapping to `fastecu_gtest` and explicitly
  uses the shared Qt header mappings in the root `BUILD.bazel`. These inherited
  `resolve` directives map Qt headers to Core, Gui and Widgets for all managed
  packages. The three managed UI packages likewise map tests to `fastecu_gtest`. Their libraries declare no `Q_OBJECT`, so they use plain `cc_library`
  with `COMMON_COPTS` instead of the moc-bearing `qt_cc_library`; the header-only
  calibration view state retains its existing compiler settings. Compiler
  options, visibility and offscreen test environments remain hand-owned;
  generated dependencies replace the broad Qt module set.
  `cc_group directory` was rejected: it merges the per-file tests and overlaps
  deliberately split libraries.
- Gazelle does not write `visibility`, `PORTABLE_PACKAGES` or the `qt_layer`
  group. A new package needs a hand-written `package(default_visibility = ...)`
  **before** the first run: without one, gazelle emits
  `visibility = ["//visibility:public"]` on the new targets.
- Gazelle owns source/header lists, generated dependencies and dependency
  classification (`deps` versus `implementation_deps`). People own target names,
  intentional library boundaries, visibility, comments, runtime metadata and
  configuration directives. Hand-edit those choices, then run the pipeline to
  reconcile generated attributes.
- `scripts/gazelle_check.py` runs Gazelle, discovers the managed BUILD files
  (including new ones), then runs the pinned `buildifier` and `buildifier-lint`
  prek hooks. Its broader `*.bazel` snapshot
  still detects changed, created or deleted files without treating preexisting
  uncommitted edits as drift. The prek `pre-push` hook and the `gazelle` job in
  `pr.yml` use this same checker.
- To update completely: `python3 scripts/gazelle_check.py --fix`, then review and
  commit the output. It returns **0** on success, including corrections. Without
  `--fix`, exits are **0** unchanged, **1** corrections made, **2** tool or lint
  failure. Formatter corrections are confirmed with another formatter pass.
- To preview generation without writing:
  `bazel run --config=release //:gazelle_diff`. This previews Gazelle output;
  the complete update command also applies Buildifier formatting and lint fixes.
- To widen: add a path to `GAZELLE_ARGS` and the checker's `MANAGED_ROOTS`,
  regenerate, and review the diff in its own pull request.

## Consequences

BUILD files under gazelle are reproducible from the sources, and CI rejects a
stale one. Gazelle adds a redundant `@googletest//:gtest` to tests
(`gtest_main`, supplied by the macro, already depends on it); this is accepted
because suppressing it would also give libraries `gtest_main`. Keep explicit
framework dependencies even when the test macro also supplies them. Use standard
Buildifier formatting, conventional `srcs = [` assignments, and put associated
tests after their libraries while retaining architectural comments. For a
runtime dependency Gazelle cannot infer, use a narrowly scoped `# keep` comment
with an explanation; do not freeze entire generated lists. See the
[Gazelle preservation rules](https://github.com/bazel-contrib/bazel-gazelle/blob/master/gazelle-reference.md).

`fastecu_portable_gtest` deduplicates its `deps`, because gazelle lists
`byte_matchers` that the macro also adds and Bazel rejects a repeated label. A
configurable attribute reaches a symbolic macro as a `select()` and cannot be
deduplicated, so the macro's `deps` is not configurable: a test that needs
`select()` in `deps` must use another macro or `fastecu_gtest`.

`fastecu_gtest` also deduplicates implicit Qt and framework dependencies when
`deps` and `qt_deps` are plain lists. Configurable expressions retain their
existing behavior and must avoid repeating implicit dependencies themselves.

Gazelle leaves an unused `cc_test` load behind when `map_kind` converts a raw
`cc_test`; buildifier removes it, and gazelle does not add it back.

This is an exception to [ADR 0002](0002-use-prek-for-fast-local-checks.md), which
reserves prek for fast checks: the hook needs Bazel, so it runs at `pre-push`
only and is filtered to C++, BUILD, `.bzl`, module, checker and hook-configuration
changes. `--no-verify` bypasses it
locally; CI is the gate.

Other macros (`qt_cc_library`, `fastecu_qttest`) need their own `map_kind` and moc
`hdrs` / `normal_hdrs` handling before their packages can be added.
