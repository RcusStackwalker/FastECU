# ADR 0016: Qt Reachability Is Enforced by Visibility

## Status

Accepted and implemented in the Bazel target graph. Supersedes the
`//:backend_no_widgets` source scan and the `QT_FREE_PACKAGES` half of
`//:portable_closure`, both deleted.

## Context

`src/backend` must never construct or show a user interface.
`//:backend_no_widgets` enforced that by scanning every source file under
`src/backend` for widget types and `Q_OBJECT`.

A text scan is the wrong instrument for a dependency rule: `glob()` does not
cross the nineteen package boundaries under `src/backend`, so the scan walked
the real checkout through a runfiles anchor, which works only where runfiles
are symlinks — Windows was exempt. `//:portable_closure` carried a second,
package-level Qt ban, also by text scan.

`rules_qt` gives each Qt module its own `cc_library` and headers, so a target
without a Qt dependency cannot compile `#include <QWidget>`. Every Qt-using
backend target already used `QT_DEPS_NO_WIDGETS`; nothing enforced that but the
scans.

## Decision

Every Qt module is reached through a `//bazel/qt:*` alias, and `QT_DEPS` /
`QT_DEPS_NO_WIDGETS` point at those aliases, so a target outside an alias's
visibility fails at analysis with an error naming both ends.
`//bazel/qt:widget_layer` gates Widgets and Charts (which pulls Widgets in
transitively): `//src/ui/...`, `//src/platform/...`, `//apps/...`,
`//tests/...`. `//bazel/qt:qt_layer` gates the rest — those four, plus
`//resources/...` and the six packages that still hold a Qt-typed legacy
adapter or a `qt_compat` shim. A new `src/backend` or `src/algorithms` package
is denied by default.

`bazel/qt_targets.bzl` carries a `visibility()` call for the widget layer and
holds `QT_DEPS`, `qt_cc_library`, and `qt_cc_binary`; the rest moved to
`bazel/qt_common.bzl`, loadable anywhere and re-exported by `qt_targets.bzl`.
Losing `qt_cc_library` replaces the `Q_OBJECT` half of the scan: its `hdrs`
attribute is the only route to moc in a library target, so no `src/backend`
header is moc'd and a `Q_OBJECT` there fails at link. QtTest fixtures, the
scan's one carve-out, still get moc through `fastecu_qttest`.

## Consequences

Positive consequences:

- The guard runs on Windows and covers a new package the moment it exists — no
  enumeration, no registry.
- The "no Qt at all" promise for `//src/backend/flash` and
  `//src/backend/checksum` is a visibility property now, as it is for the other
  fourteen packages that hold no Qt.

Costs and risks:

- `@rules_qt//:qt_widgets` is public in its own repo and our visibility cannot
  narrow it, so a BUILD file writing that label directly still builds. Every
  in-repo reference goes through the alias, making that a deliberate act;
  closing it needs a validation aspect on every build. A backend package could
  likewise moc a header by hand via `qt_cpp_moc_headers`.
- A backend `Q_OBJECT` fails at link with an undefined symbol rather than at a
  scan with an explanatory message.
- Two Qt `.bzl` files instead of one.
- `//:portable_closure` skipped `qt_cc_library` when identifying portable
  targets by rule kind; the backend `Legacy*Adapter` targets are plain
  `cc_library` now, so it scans only what `PORTABLE_ROOTS` names.
- Its per-target Qt scan survives for the six mixed packages: visibility is
  package-granular, so nothing can let `legacy_config_adapter` see Qt while
  denying its neighbour `app_config`. Splitting each adapter and shim into its
  own package would retire the scan; each of the four adapters has one consumer
  today.

## Notes

`src/platform/desktop/common/ports` granted its whole package to
`//src/backend/definitions`, for three QtTest suites that exercise
`FileActions` against the real Qt ports. That grant moved to the `:ports`
target.
