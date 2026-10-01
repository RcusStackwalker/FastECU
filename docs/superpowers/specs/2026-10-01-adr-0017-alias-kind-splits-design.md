# ADR 0017: retire the `alias_kind` whole-rule keeps

## Intent

Remove the last three `alias_kind` whole-rule keeps from the checker's
`KEPT_CPP_PRODUCTION_RULES` allowlist, so the pinned Gazelle limitation recorded
in [ADR 0017](../../adr/0017-generate-bazel-targets-with-gazelle.md) no longer
applies to any package. After this change the allowlist holds only the two DLL
fixtures, which have no discoverable local sources and stay as documented
permanent exceptions.

Constraints:

- Gazelle 0.54.0 and gazelle_cc 0.6.0 stay unmodified.
- No behavior change in any target; no label changes meaning silently.
- Hand-owned items that are not C++ rules Gazelle generates (resources,
  Designer forms, replicas, OS-selection aliases) are out of scope.

Success criteria:

- `python3 scripts/gazelle_check.py --fix` run twice leaves the second run
  unchanged.
- `bazel test --config=release //...` and `prek run --all-files` pass.
- No `alias_kind` directive remains in the repository.
- The checker allowlist and the ADR text agree.

## Problem

Three moc libraries sit in packages that also hold plain libraries, so those
packages use `alias_kind qt_cc_library cc_library`. With the pinned versions the
merger ignores existing attributes on aliased kinds, so the three moc rules are
hand-owned with whole-rule keeps:

- `//src/platform/desktop/common/ports:qt_event_sink`
- `//src/ui/desktop/definition:definition_authoring_dialog`
- `//src/platform/desktop/unix/j2534:j2534`

## Approach

Split each mixed package into an ordinary parent and a moc child that maps
`cc_library` to `qt_cc_library` locally. This is the rule the ADR already states
("code that needs both is split into a parent and a child package") and the
pattern used for diagnostics, logging, flash, service functions, serial and the
desktop widgets. Alternatives rejected: patching or vendoring Gazelle (contradicts
the unmodified-tooling stance) and leaving the keeps (defeats the goal).

## Design

### 1. Event sink: `common/ports/event_sink`

- Move `qt_event_sink.h` and `qt_event_sink.cpp` to the new child, a moc package:
  `# gazelle:map_kind cc_library qt_cc_library //bazel:qt_targets.bzl`. The child
  is visible only to the parent `ports` package.
- The parent `ports` library keeps re-exporting the sink through the stable
  public label, now as a narrow dependency keep on the child (no include in the
  parent expresses it). The sink is consumed by apps, platform workers, the UI
  widgets and the UI service-function dialog; resolving to the existing `ports`
  label means no new `ui -> platform` edge and no visibility change for any
  consumer.
- The seven `gazelle:resolve cc .../qt_event_sink.h` directives (apps/desktop,
  diagnostics/workers, flash/worker, logging/runtime, service_functions/worker,
  ui service_functions/dialog, ui widgets) keep their targets and only change
  the header path to `src/platform/desktop/common/ports/event_sink/qt_event_sink.h`.
- Includes change in `apps/desktop/desktop_composition.h`, `dtc_worker.cpp`,
  `flash_worker.cpp`, `logging_engine.h`, `service_function_worker.cpp`,
  `qt_port_adapters_test.cpp`, `widgets/mainwindow.h`, `widgets/mainwindow_test.cpp`
  and `widgets/settings_test.cpp`.
- `test_qt_port_adapters` stays in the parent because it exercises the adapters
  and the sink together; Gazelle resolves its sink include to the child.
- The parent drops its `alias_kind` directive and the `qt_cc_library` load.

### 2. Authoring dialog: `ui/desktop/definition/dialog`

- Move `definition_authoring_dialog.{h,cpp}` and its test to the child, a moc
  package, following the `service_functions/dialog` precedent. `definition_header_form`
  and its test stay in the ordinary parent.
- The `config_fields.h` resolve directive moves to the child.
- The only consumer outside the package is `ui/desktop/widgets` (`mainwindow.h`
  and its dependency list). The child uses the UI layer as its default
  visibility. The grant in `common/definition` that names
  `//src/ui/desktop/definition:__pkg__` is repointed to the new child, which is
  the only package that uses the catalog session.

### 3. Unix J2534 driver: `unix/j2534/driver`

- Move `J2534_unix.{h,cpp}` to the child, a moc package. The parent keeps
  `j2534_types`, `serial_byte_buffer` and the `j2534_api` alias with its
  explained include-prefix keeps.
- `j2534_api.h` and `J2534_unix.cpp` get updated include paths.
- The `//src/platform/desktop/unix/j2534` default label ceases to exist. Its
  consumers are repointed: `tests/BUILD.bazel` (resolve directive and dependency),
  `unix/j2534/testing:mock_openport`, and `j2534_api`. The child depends on the
  parent's `j2534_types` and `serial_byte_buffer`.
- OS `target_compatible_with` selects are kept on the child. The Windows package
  is untouched.

### 4. Bookkeeping

- Remove the three entries from `KEPT_CPP_PRODUCTION_RULES` in
  `scripts/gazelle_check.py`.
- The managed-roots lists (`MANAGED_ROOTS` and the root `GAZELLE_ARGS`) match by
  path prefix, so the three new children are already covered; leave them alone.
- Delete the three `alias_kind` directives.
- Rewrite the ADR's alias-merging paragraph and its "remaining exceptions" list
  so only `pe_bitness_x64_fixture` and `fake_j2534_dll_native` remain, and note
  that moc libraries in mixed packages are always split.
- Add or adjust a checker test if the existing tests do not cover the shrunken
  allowlist (a stale entry must still be rejected).

## Error handling and risk

- The checker already rejects both a stale allowlist entry and a new whole-rule
  keep, so a missed entry fails `prek`/CI rather than silently passing.
- A missed include path or consumer dependency fails at compile time.
- Visibility mistakes fail at analysis time; the layer groups name exact packages.
- Windows-only paths cannot be built on the macOS development machine. The unix
  driver compiles on macOS (non-Windows) and the Windows packages are untouched.

## Testing and verification

1. `python3 scripts/gazelle_check.py --fix`, review the diff, run it again and
   confirm no change.
2. `bazel test --config=release //...`.
3. `prek run --all-files`.
4. `scripts/android-cross-compile.sh` is not expected to be affected (platform
   and UI packages only); run it if `ANDROID_NDK_HOME` is available.

## Delivery

One branch and one pull request off `master`, with one commit per package plus a
bookkeeping commit: event sink, authoring dialog, unix driver, then checker, ADR
and managed-root lists. Nothing is pushed or opened without explicit instruction.

## Out of scope

- The two DLL fixtures stay as permanent allowlist entries.
- Resource, Designer-form, replica and OS-selection alias rules stay hand-owned.
- Deferrals outside ADR 0017, including items in the tech-debt roadmap.
