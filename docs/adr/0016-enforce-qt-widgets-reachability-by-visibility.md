# ADR 0016: Qt Widgets Reachability Is Enforced by Visibility

## Status

Accepted and implemented in the Bazel target graph. Supersedes the
`//:backend_no_widgets` source scan, which is deleted.

## Context

`src/backend` must never construct or show a user interface.
`//:backend_no_widgets` enforced that by scanning every source file under
`src/backend` for widget types and `Q_OBJECT`.

A text scan is the wrong instrument for a dependency rule. `glob()` does not
cross the nineteen package boundaries under `src/backend`, so the scan could
not be handed the tree as test data; it walked the real checkout through a
runfiles anchor instead, which works only where runfiles are symlinks — so
Windows was exempt.

Yet `rules_qt` gives each Qt module its own `cc_library` and headers, so a
target without a widgets dependency cannot compile `#include <QWidget>`. Every Qt-using backend target already used
`QT_DEPS_NO_WIDGETS`; nothing enforced that but the scan.

## Decision

Widgets and Charts (which pulls Widgets in transitively) are reached through
the `//bazel/qt:widgets` and `//bazel/qt:charts` aliases, visible to the
`//bazel/qt:widget_layer` package group: `//src/ui/...`, `//src/platform/...`,
`//apps/...`, `//tests/...`. `QT_DEPS` points at the aliases, so a target
outside that group fails at analysis with a visibility error naming both ends.

`bazel/qt_targets.bzl` carries a `visibility()` call for the same four layers
and holds `QT_DEPS`, `qt_cc_library`, and `qt_cc_binary`; the rest moved to
`bazel/qt_common.bzl`, loadable anywhere and re-exported by `qt_targets.bzl`.
Losing `qt_cc_library` replaces the `Q_OBJECT` half of the scan: its `hdrs`
attribute is the only route to moc in a library target, so no `src/backend`
header is moc'd and a `Q_OBJECT` there fails at link. QtTest fixtures, the
scan's one carve-out, still get moc through `fastecu_qttest`.

## Consequences

Positive consequences:

- The guard runs on Windows, and covers a new backend package the moment it
  exists — no enumeration, no registry.
- `src/algorithms` gained the same protection; its `ssm` `qt_compat` shim had
  been pulling widgets in.

Costs and risks:

- `@rules_qt//:qt_widgets` is public in its own repo and our visibility cannot
  narrow it, so a backend BUILD file writing that label directly still builds.
  Every in-repo reference goes through the alias, so that is a deliberate act,
  not an accident; closing it needs a validation aspect on every build. A
  backend package could likewise moc a header by hand via
  `qt_cpp_moc_headers`.
- A backend `Q_OBJECT` fails at link with an undefined symbol rather than at a
  scan with an explanatory message.
- Two Qt `.bzl` files instead of one.
- `//:portable_closure` identified portable targets by rule kind, skipping
  `qt_cc_library`; the backend `Legacy*Adapter` targets are plain `cc_library`
  now, so it scans only what `PORTABLE_ROOTS` names.

## Notes

`QT_FREE_PACKAGES` in `scripts/check-portable-closure.py` is not subsumed:
`//src/backend/flash` and `//src/backend/checksum` promise no Qt *at all*,
including `@rules_qt//:qt_core`, which these aliases do not gate.
