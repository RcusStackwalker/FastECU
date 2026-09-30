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
`BUILD.bazel`. The scope includes every C++ test-owning package and the
previously migrated production packages. `scripts/gazelle_check.py` carries
the identical `MANAGED_ROOTS` list; its unit test checks agreement. Production
library and binary ownership outside the earlier migration remains deferred.

- Grouping is `cc_group unit`; `cc_test` is mapped to `fastecu_portable_gtest`.
  The `qt_compat` package overrides the mapping to `fastecu_gtest` and explicitly
  uses the shared Qt header mappings in the root `BUILD.bazel`. These inherited
  `resolve` directives map encountered headers to their Qt modules, including
  Core, Gui, Widgets, Xml, SerialPort and RemoteObjects. Managed UI
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

### Complete C++ test ownership

All C++ test executables use GoogleTest and regenerate with `gazelle_cc`.
Gazelle covers every test-owning package, including a test-only desktop
support package. Python build guards retain their existing ownership.
The pinned Gazelle and gazelle_cc versions are unchanged.

Each package has one recognized test mapping. Mixed packages use
`fastecu_gtest`, with `qt = False` for portable tests and
`use_gtest_main = False` for exceptional entry points. Ordinary Qt tests
register explicit Core or Widgets GoogleTest environments; signal recording
and event waits live in the test-only desktop support package.

Newly covered mixed packages exclude production sources and headers from
discovery and preserve production rules with explained keeps. Header
resolutions point to existing public owners. Runtime resources, platform
selection, constraints and non-inferable link dependencies remain explicit.
These exclusions and resolutions are the maintenance cost of deferring
production ownership; adding a test include still regenerates its dependency.
No C++ test may have a whole-rule keep.

Exceptional main sources are excluded from discovery and retained with narrow
source-entry keeps, so Gazelle cannot treat them as shared package runners.
DLL fixtures and other helper binaries remain excluded and hand-owned.
Generated test labels end in `_test`: the pinned unit-group generator appends
`_test` to nonstandard names in mixed packages. The former
`//tests:mut_dma_integration_tests`, `//tests:serial_crash_tests` and
`//tests/force_asserts:tst_force_asserts` labels were not retained as aliases.
Platform constraints and runtime settings remain on the generated tests.

The checker rejects uncovered C++ test packages and whole-rule test keeps.
Generation followed by a second checker run must be unchanged.

### Production deferrals

Production ownership is migrating package by package. The checker carries a
shrinking `KEPT_CPP_PRODUCTION_RULES` allowlist of C++ library and binary labels
that still have a whole-rule keep. A new whole-rule keep outside the list is
rejected, and so is a listed label that no longer has one: delete the entry in
the change that migrates the rule. Narrow attribute and dependency keeps are
unrestricted. The desktop `bytes`, `definition`, `transport` and `serial/testing`
packages are fully generated; their libraries are plain `cc_library` targets with
`COMMON_COPTS`, and the `transport` and `flash_transports` aggregates keep their
labels because generated rules merge into existing rules of the same name.

Platform packages that mixed ordinary helpers with moc workers are split by
package, not by macro: ordinary code stays in the parent as plain `cc_library`
targets, and moc code moves to a child that maps `cc_library` to
`qt_cc_library` locally (`diagnostics/workers`, `logging/runtime`,
`flash/worker`, `service_functions/worker`). Tests move with their
implementations and keep their target names. The connection package and the UI
flash dialog and controller packages are homogeneous moc packages and use the
same mapping in place. A child that reaches the ports package's private event
sink resolves `qt_event_sink.h` to the public `ports` label.

The serial package follows the same rule. The facade (`serial/facade`), the
remote backend (`serial/remote`), the websocket device (`serial/websocket`) and
the recording log sink (`serial/testing/log_sink`) are moc children; the backend
interface, facade codes, backend host, factory, idle-state adapter and
`qtrohelper.hpp` are plain generated libraries in the parent. Replica
generation stays hand-owned, and the remote package resolves the generated
`rep_serial_port_actions_replica.h` to it. The facade-threading test keeps its
custom main as an excluded source with a narrow source-entry keep. The direct
backend and the J2534 selector remain hand-owned until they migrate.

The direct serial backend is split by responsibility under `serial/direct`: the
header-only moc library for `serial_port_actions_direct.h`, the shared
implementation in `direct/common`, and one hook library each in `direct/unix`
and `direct/windows`. The moc library depends on neither implementation; the
implementation and hooks depend on it. Because no include expresses it, each
hook library keeps an explained dependency on the shared implementation, and
the shared and hook libraries retain `alwayslink` so the selected backend
survives archive ordering. The `j2534_api` selection alias, the OS-selecting
`srcs` of `j2534_driver_selection`, and the `direct_serial_backend_for_tests`
alias remain hand-owned. Production libraries never resolve through that
test-only alias; tests that need the runtime implementation keep it narrowly.
Non-moc children undo the inherited `map_kind` with
`map_kind cc_library cc_library`.

The desktop UI separates widgets from ordinary helpers the same way. The
`desktop` aggregate, the DTC dialog and their tests live in
`src/ui/desktop/widgets`, a moc package; `main_window_services`, `config_fields`
and `diagnostic_link_io` are plain generated libraries in the parent, which also
keeps the Designer forms and their hand-owned generation. The widgets package
resolves every generated `ui_*.h` header to its form's library and keeps the
resource-registration dependencies, Windows link options, offscreen
environments and static-link settings. The service-function dialog moved to a
`dialog` child of the UI service-functions package, leaving the preflight in the
ordinary parent. The `ui_desktop` and `ui_service_functions` visibility groups
name exactly the relocated consumer packages.
