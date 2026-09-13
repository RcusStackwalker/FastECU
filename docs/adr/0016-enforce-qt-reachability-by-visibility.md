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
package-level Qt ban, also by text scan. Yet `rules_qt` gives each Qt module
its own `cc_library` and headers, so a target without a Qt dependency cannot
compile `#include <QWidget>`; every Qt-using backend target already used
`QT_DEPS_NO_WIDGETS`, with nothing but the scans enforcing it.

## Decision

Every Qt module is reached through a `//bazel/qt:*` alias, and `QT_DEPS` /
`QT_DEPS_NO_WIDGETS` point at those aliases, so a target outside an alias's
visibility fails at analysis with an error naming both ends.
`//bazel/qt:widget_layer` gates Widgets and Charts (which pulls Widgets in
transitively): `//src/ui/...`, `//src/platform/...`, `//apps/...`,
`//tests/...`. `//bazel/qt:qt_layer` gates the rest — those four, plus
`//resources/...` and the leaf packages that exist to hold Qt-typed
transitional code. A new `src/backend` or `src/algorithms` package is denied by
default.

Six packages used to mix portable targets with a Qt-typed neighbour, which
package-granular visibility cannot separate. Each neighbour moved into its own
package — `src/backend/<pkg>/legacy` for the four `Legacy*Adapter` targets,
`src/algorithms/protocol{,/ssm}/qt_compat` for the two shims — carrying
`default_visibility = ["//bazel/qt:qt_layer"]`. That makes one group the single
answer to "may this package touch Qt", for Qt itself and for our Qt-typed code
alike.

Qt is unreachable even by name. `bazel_dep(name = "rules_qt")` lives in
`third_party/qt`, a nested module reached by `local_path_override`, not in the
root module. A module's repo mapping holds only its own direct dependencies, so
`@rules_qt` and the three `@qt_*` platform repos do not resolve anywhere in
FastECU: `deps = ["@rules_qt//:qt_widgets"]` is a load error, not a visibility
violation. That module re-exports the Qt libraries as aliases visible only to
`//bazel/qt`, which is therefore the sole route to Qt in the repo.

`bazel/qt_targets.bzl` carries a `visibility()` call for the widget layer and
holds `QT_DEPS`, `qt_cc_library`, and `qt_cc_binary`; the rest moved to
`bazel/qt_common.bzl`, loadable anywhere and re-exported by `qt_targets.bzl`.
Losing `qt_cc_library` replaces the `Q_OBJECT` half of the scan: its `hdrs`
attribute is the only route to moc in a library target, so no `src/backend`
header is moc'd and a `Q_OBJECT` there fails at link. QtTest fixtures, the
scan's carve-out, still get moc through `fastecu_qttest`.

## Consequences

Positive consequences:

- The guard runs on Windows and covers a new package the moment it exists — no
  enumeration, no registry.
- Every portable package is Qt-free by construction, not by scan, and there is
  no bypass left. `//:portable_closure` keeps none of its Qt checking, nor the
  JNI pattern that outlived it unused: the file is down to one assertion, that
  no `//src/platform` label is in the portable closure.

Costs and risks:

- FastECU owns `qt_cc_library`, `qt_cc_binary` and `qt_cc_test` now, in
  `third_party/qt/qt.bzl`. A raw `"@repo//..."` string inside a macro resolves
  against the *calling* package's repo mapping, and rules_qt's versions carry
  51 of them across their select keys, `data` and `env`, so a package that
  cannot name `@rules_qt` cannot call them. The reimplementations use
  `str(Label(...))`, which resolves in that file and yields a canonical label.
  They track upstream by hand, so keep them diffable against it. Intel macOS
  goes with them, and a patch drops the matching branch from `@rules_qt`'s own
  targets: it names a repo this project never fetches, which a genquery scope
  -- unlike ordinary analysis -- resolves, breaking `//:portable_closure`
  whenever it had something to say.
- `third_party/qt` is listed in `.bazelignore`. Without it the nested module is
  also a package of the main repo, where its own `@rules_qt` labels do not
  resolve.
- A backend package could still moc a header by hand via `qt_cpp_moc_headers`,
  which `fastecu_qttest` needs and cannot be split from.
- `//src/backend/definitions` mixes the Qt-typed `FileActions` family with one
  Qt-free target, `:models`, which the portable `//src/backend/flash` packages
  use. Its package default is `//bazel/qt:qt_layer` and `:models` carries the
  wider visibility, rather than the other way round.
- A backend `Q_OBJECT` fails at link with an undefined symbol rather than at a
  scan with an explanatory message.
- Two Qt `.bzl` files instead of one.
- `//:portable_closure` skipped `qt_cc_library` when identifying portable
  targets by rule kind; those targets are plain `cc_library` now, so it scans
  only the targets its registry names.
- Six packages gained a `legacy` or `qt_compat` subpackage, and the headers
  moving with them changed include path — `qt_bytes.h` in 32 files,
  `ssm_protocol.h` in 13. Both shims are transitional, so those packages should
  disappear rather than grow.

## Notes

`src/platform/desktop/common/ports` granted its whole package to
`//src/backend/definitions`, for three QtTest suites that exercise
`FileActions` against the real Qt ports. That grant moved to the `:ports`
target.
