# ADR 0016: Qt Widgets Reachability Is Enforced by Visibility

## Status

Accepted and implemented in the Bazel target graph. Supersedes the
`//:backend_no_widgets` source scan, which is deleted.

## Context

`src/backend` must never construct or show a user interface. Step 6a-5
enforced that with `//:backend_no_widgets`, a `py_test` that walked every
source file under `src/backend` and rejected `QMessageBox`, `QFileDialog`,
`QDialog`, `QWidget`, and `Q_OBJECT` outside comments and QtTest fixtures.

A text scan is the wrong instrument for a dependency rule, and the scan paid
for it. `src/backend` spans nineteen packages and `glob()` does not cross
package boundaries, so the tree could not be declared as test data without
nineteen filegroups that a new package would silently fall out of. The scan
worked around that by resolving a `data` anchor through the runfiles API and
walking the directory the anchor landed in — which only works where runfiles
entries are symlinks into the real checkout. Windows materializes runfiles by
copying, so the check was marked incompatible there and a `MIN_EXPECTED_FILES`
backstop existed to catch the same failure on the platforms that did run it.
That is roughly 120 lines of Python, none of it about widgets.

The rule it was enforcing is a property of the build graph. `rules_qt` gives
each Qt module its own `cc_library` with its own headers and include prefix,
so a target that does not depend on Qt Widgets cannot compile `#include
<QWidget>` at all — the failure is "file not found", not a link error. Every
Qt-using target under `src/backend` already used `QT_DEPS_NO_WIDGETS`; nothing
enforced that choice but the scan.

## Decision

Gate reachability instead of scanning text, in two places.

**Target visibility.** Qt Widgets and Qt Charts (which pulls Widgets in
transitively) are reached through `//bazel/qt:widgets` and `//bazel/qt:charts`,
aliases whose visibility is the `//bazel/qt:widget_layer` package group:
`//src/ui/...`, `//src/platform/...`, `//apps/...`, `//tests/...`. `QT_DEPS`
points at the aliases. A target outside that group that reaches for widgets —
directly or through `QT_DEPS` — fails at analysis with a visibility error
naming both ends. Enforcement is per-edge and automatic, so no package can
fall out of scope by being added.

**Load visibility.** `bazel/qt_targets.bzl` carries a `visibility()` call for
the same four layers and holds `QT_DEPS`, `qt_cc_library`, and `qt_cc_binary`.
`src/backend` and `src/algorithms` cannot load it. Everything Qt-related that
is neither widget- nor moc-bearing moved to `bazel/qt_common.bzl`, which is
loadable from anywhere; `qt_targets.bzl` re-exports it so widget-layer BUILD
files still need one load statement.

Losing `qt_cc_library` is what replaces the `Q_OBJECT` half of the old scan.
That macro's `hdrs` attribute is the only route to moc in a library target, so
with it out of reach no `src/backend` header is moc'd, and a `Q_OBJECT` there
declares `staticMetaObject` and `qt_metacall` that nothing defines — the first
test linking it fails. QtTest fixtures, the one case the old scan exempted,
still get moc through `fastecu_qttest`.

`src/algorithms` is deliberately outside the widget layer: it sits below
`src/backend` and had no business with widgets either, though its `ssm`
`qt_compat` shim was pulling them in.

## Consequences

Positive consequences:

- The guard runs on Windows, because analysis-time visibility is a property of
  the graph rather than of the host's runfiles strategy.
- No enumeration and no registry: a new package under `src/backend` is covered
  the moment it exists.
- The error arrives at the line that caused it and names the offending target,
  rather than reporting a file and line number from a scan.
- `src/algorithms` gained the same protection at no extra cost.
- About 120 lines of Python and a 12-line platform-exemption comment are gone.

Costs and risks:

- **Residual: the raw external label still works.** `@rules_qt//:qt_widgets` is
  public in its own repo and our visibility cannot narrow it, so a BUILD file
  under `src/backend` that writes that label instead of `//bazel/qt:widgets`
  still builds. Every in-repo reference goes through the alias, so this is a
  deliberate act visible in review, not a copy-paste accident. Closing it would
  need a validation aspect on every build; that trade was not worth it.
- Equally, a backend package could load `qt_cpp_moc_headers` from
  `qt_common.bzl` and moc a header by hand. `fastecu_qttest` needs that macro
  and cannot be split from it without duplication, so it stays reachable.
- A `Q_OBJECT` in backend production code now fails at link with an
  undefined-symbol error rather than at a scan with an explanatory message.
- Two Qt `.bzl` files instead of one, and the widget-layer file re-exports the
  common one.
- `//:portable_closure` identified portable targets by rule kind, treating
  `qt_cc_library` invocations as out of scope. The backend `Legacy*Adapter`
  targets are plain `cc_library` rules now, so that check scans only the
  targets its `PORTABLE_ROOTS` registry names, matching the dual registration
  the closure `genquery` already required.

## Notes

`QT_FREE_PACKAGES` in `scripts/check-portable-closure.py` is not subsumed by
this ADR: `//src/backend/flash` and `//src/backend/checksum` promise no Qt *at
all*, including `@rules_qt//:qt_core`, which these aliases do not gate.
