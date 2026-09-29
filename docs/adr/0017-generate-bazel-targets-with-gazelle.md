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
`src/ui/desktop/calibration`, `src/ui/desktop/checksum`,
`src/ui/desktop/menu`, `src/ui/desktop/channels`, `src/ui/desktop/definition`,
`src/ui/desktop/biu`, `src/ui/desktop/hexedit`, `apps/bench`,
`src/platform/desktop/common/ports`, `src/platform/desktop/common/remote_utility`,
`src/platform/desktop/unix/j2534`, `src/platform/desktop/windows/j2534` and
`src/platform/desktop/common/connection/testing`, including their
subpackages. The legacy Qt-backed
`src/backend/definitions` remains unmanaged.

- Grouping is `cc_group unit`; `cc_test` is mapped to `fastecu_portable_gtest`.
  The `qt_compat` package overrides the mapping to `fastecu_gtest` and explicitly
  uses the shared Qt header mappings in the root `BUILD.bazel`. These inherited
  `resolve` directives map encountered headers to their Qt modules, including
  Core, Gui, Widgets, Xml, SerialPort, Test and RemoteObjects. Managed UI
  packages with GoogleTest suites map tests to `fastecu_gtest`. Moc-free
  libraries use plain `cc_library`
  with `COMMON_COPTS` instead of the moc-bearing `qt_cc_library`; the header-only
  calibration view state retains its existing compiler settings. The connection
  test harness also uses plain `cc_library` with `COMMON_COPTS` and no moc;
  package-local header mappings resolve its unmanaged Qt-macro providers.
  Bench fixtures retain the portable test mapping. Compiler
  options, visibility and offscreen test environments remain hand-owned;
  generated dependencies replace the broad Qt module set.
  `cc_group directory` was rejected: it merges the per-file tests and overlaps
  deliberately split libraries.
- Gazelle does not write `visibility` or the `qt_layer`
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

The channels package maps `cc_library` to `qt_cc_library` locally. Both of its
header-only libraries declare `Q_OBJECT`, so every generated `hdrs` entry must
run through moc; target names and visibility remain hand-owned. Empty `srcs`
attributes are kept because the Qt macro requires them even for header-only
libraries. This mapping is suitable only when all library headers need moc.
Mixed packages split ordinary headers into plain `cc_library` targets and keep
only `Q_OBJECT` headers in `qt_cc_library.hdrs`. The ports package re-exports
its private `qt_event_sink` moc library through the existing `ports` label;
Unix J2534 publishes ordinary type declarations through `j2534_types`.
Windows J2534 uses a plain library and obtains bridge headers from their
existing owners. Shared J2534 API include prefixes remain explicitly kept,
along with OS constraints, x86 transitions and the externally sourced
`pe_bitness_x64_fixture` rule.

Gazelle 0.54.0 and gazelle_cc 0.6.0 remain unmodified. In mixed packages,
`alias_kind qt_cc_library cc_library` recognizes the existing macro without
converting plain libraries. However, this pinned combination has a merging
limitation: gazelle_cc emits the alias kind on generated rules, while Gazelle's
merger looks up mergeable attributes using that kind without its underlying
`cc_library` metadata. Existing source/header/dependency attributes therefore
remain unchanged rather than regenerating. The three affected moc rules are
explicitly hand-owned with explained rule keeps:

- `//src/platform/desktop/common/ports:qt_event_sink`
- `//src/ui/desktop/definition:definition_authoring_dialog`
- `//src/platform/desktop/unix/j2534:j2534`

Their dependency lists were generated through the standard library shape and
verified with their moc-bearing shapes. Plain libraries and tests in these
packages are fully generated. Remove these temporary keeps when an approved
upstream version fixes alias merging; until then, changes to their sources or
includes require updating these rules by hand.

Packages containing only moc libraries use `map_kind cc_library qt_cc_library`.
Bench uses `map_kind cc_binary qt_cc_binary` for its existing binary and keeps
the portable GoogleTest mapping. The binary's selected direct backend is an
explained dependency keep. Designer-form and remote-replica generation remain
hand-owned; package-local resolutions connect `ui_*.h` and
`rep_remote_utility_replica.h` to their generated targets. Resource registration
is retained with dependency keeps wherever no include expresses the link.
Unmanaged platform and UI header providers have package-local resolutions.

The checker now covers **51 of 67 C++ packages**, including the eleven packages
added in this phase. The remaining sixteen packages await the separate
QtTest-to-GoogleTest migration and mixed-header cleanup. That prerequisite must
preserve coverage, application initialization, event-loop waits, offscreen
settings, platform constraints and process-test behavior; `QSignalSpy` may stay
in GoogleTest suites. Generated Qt assets, shared moc owners, platform selection
and exceptional process/ABI fixtures remain explicitly hand-owned where needed.
Completion of the broader migration requires every surviving C++ package to be
covered and ordinary attributes to regenerate, with exceptions documented.

## Update (2026-09-29)

The legacy Qt-backed `src/backend/definitions` package named above was retired
in step 6n, and the `qt_compat` package override no longer applies to any
surviving package. The scope list and the header-mapping rules otherwise stand.
