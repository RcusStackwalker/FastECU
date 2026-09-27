# Step 6i: retire `serial_qt_compat` and close the facade's transitive reach into the UI

## Goal

Finish the "Drain the `serial_qt_compat` allowlist" entry in the
[tech-debt roadmap](../../tech-debt.md) and the step 6 bullet "Remove
compatibility wrappers, obsolete facades" in the
[modularization plan](../../modularization-plan.md), as far as the serial
facade is concerned.

Success means:

- No target named `serial_qt_compat` or `serial_platform_api` exists. The
  facade library is `//src/platform/desktop/common/serial:serial_port_actions`,
  visible only to `//src/platform/desktop:__subpackages__` and
  `//tests:__pkg__`.
- `//:serial_compat_allowlist` and `scripts/check-serial-compat-allowlist.py`
  are deleted. Visibility is the guard for direct dependencies, as ADR 0016
  made it for Qt.
- No facade header (`serial_port_actions.h`, `serial_backend.h`,
  `serial_backend_host.h`, `serial_facade_codes.h`) is in the compile inputs
  of any `//src/ui/...` or `//src/backend/...` target. On the sandboxed Linux
  and macOS builds a UI `#include` of one fails with "file not found"; this is
  demonstrated once, by hand, and recorded in the 6i-2 PR.
- No production source changes behavior. Only BUILD files, one target split,
  and documentation change.

## Non-goals

- The two GRANDFATHERED `ui → platform` edges, `//src/platform/desktop/common/logging`
  (`SystemLogger`) and `//src/platform/desktop/common/remote_utility`
  (`RemoteUtility`). Each needs its own seam through `MainWindow`; they become
  step 6j, which is not designed here.
- Narrowing the facade's API or changing what `SerialPortActions` does. The
  "Narrow serial and hardware interfaces" tech-debt entry is unchanged apart
  from a note.
- Deleting `//src/algorithms/protocol/qt_compat`. `qt_bytes.h` is the
  sanctioned `QByteArray` boundary conversion, not facade debt.
- A new source-scan guard. A text scan is what ADR 0016 retired; this step
  relies on the build graph.
- Windows enforcement. Windows builds are unsandboxed, so a missing header
  input does not fail there. Linux and macOS CI catch the same include.

## Current state (2026-09-27, `master` at `49269875`)

`serial_qt_compat` compiles `serial_backend_host.cpp` and
`serial_port_actions.cpp`, exports the four facade headers, and has a frozen
visibility list of three entries: the serial package itself,
`//src/platform/desktop/common/transport`, and `//tests`.
`serial_platform_api` is a sourceless re-export of it, visible to
`//src/platform:__subpackages__`, created so platform packages could reach the
facade without joining the frozen list.

Direct dependents of either target (`bazel query 'rdeps(//..., …, 1)'`, moc
genrules omitted):

| Package | Targets |
|---|---|
| `serial` | `desktop_serial_factory`, `direct_serial_backend_{unix,windows}`, `serial_port_actions_direct_moc`, `remote_serial_backend`, `serial_idle`, and the tests `desktop_serial_factory_test`, `serial_idle_test`, `test_direct_backend`, `test_direct_backend_pty`, `test_facade_threading` |
| `serial/testing` | `fake_serial_backend`, `fake_backend_test` |
| `transport` | `transport`, `flash_transports`, `logging_protocol_registration`, `fake_backed_serial` |
| `connection` | `adapter_connection`; `connection/testing:adapter_connection_harness` |
| `diagnostics` | `serial_diagnostic_link` |
| `service_functions` | `service_function_worker` |
| `//tests` | `mut_dma_integration_tests`, `serial_crash_tests`, `serial_pty_e2e_test` |

Every one of them is under `src/platform` or `//tests`, so no UI or backend
package names the facade. But ten UI targets reach it transitively, among
them `//src/ui/desktop:desktop`, `flash/common:flash_dialog`,
`flash/operation:flash_operation_controller`, and
`service_functions:service_function_dialog`. Because every edge above is an
ordinary `deps` edge, the facade headers are in those targets' compile
inputs, and a UI `#include` of `serial_port_actions.h` builds today.

Which public headers carry facade headers onward:

- **Forward-declare only**, so they can take the facade as
  `implementation_deps`: `transport` (`fastecu_{can,kline,ssm}_transport.h`),
  `flash_transports`, `logging_protocol_registration`, `adapter_connection`,
  `serial_diagnostic_link`, `service_function_worker`
  (`serial_facade_configurator.h` includes no facade header), `serial_idle`,
  and `desktop_serial_factory`.
- **Include a facade header publicly**, so they keep plain `deps`:
  - `serial_port_actions_direct.h` includes `serial_backend.h` and
    `serial_facade_codes.h` (the direct backends and their moc target).
  - `remote_serial_backend.h` includes `serial_backend.h`.
  - `serial/testing/fake_backend.h` includes `serial_port_actions_direct.h`.
  - `transport/fake_backed_serial.h` includes `serial_port_actions.h`.
  - `connection/testing/adapter_connection_harness.h` includes
    `fake_backend.h`.

None of the second group is reachable from the UI except one path:
`//src/ui/desktop` → `remote_utility` (GRANDFATHERED) →
`serial:remote_serial_backend` → `serial_backend.h`. `remote_utility` depends
on `remote_serial_backend` only because `remote_utility.h` includes
`websocketiodevice.h` and `qtrohelper.hpp`, which that target also owns.
`remote_utility.cpp` uses nothing else from it.

## Design

### Rename in place

`serial_qt_compat` becomes `serial_port_actions` in the same package, with the
same sources, headers (`serial_port_actions.h` in `hdrs` for moc; the other
three in `normal_hdrs`), and deps. Its visibility becomes:

```starlark
visibility = [
    "//src/platform/desktop:__subpackages__",
    "//tests:__pkg__",
],
```

`//src/platform/desktop:__subpackages__` rather than an explicit package list:
the rule being enforced is "backend and UI never name the facade", and the
future `src/platform/android` tree is excluded by construction.
`serial_platform_api` is deleted; each of its consumers depends on
`:serial_port_actions` directly.

The sources already live in the package that owns them, so "fold its sources
into the owning packages" from the tech-debt entry needs no file moves beyond
the one below.

Rejected alternatives: keeping `serial_platform_api` as the public label with
the old target made private (two labels for one library, the alias's only
reason to exist being the freeze); splitting `serial_facade_codes.h` into its
own target (its only external user, `diagnostics`, includes
`serial_port_actions.h` too).

### Retire the allowlist guard

Delete the `serial_compat_allowlist` `py_test` from the root `BUILD.bazel`,
`scripts/check-serial-compat-allowlist.py`, and `"BUILD.bazel"` from the serial
package's `exports_files` (its only reader was the script;
`serial_port_actions_direct.h` stays exported for
`//:windows_preprocessor_guards`). A ratchet that has reached its end state
has nothing left to freeze, and the new visibility list says the same thing in
the build graph.

### Stop the headers propagating

Every target in the "forward-declare only" group lists
`//src/platform/desktop/common/serial:serial_port_actions` under
`implementation_deps` instead of `deps`. `qt_cc_library` forwards `**kwargs`
to `cc_library` (`third_party/qt/qt.bzl`), so no macro change is needed.
`implementation_deps` still links the library into dependents; it only keeps
its headers out of their compilation context.

Tests and fixtures that include a facade header but got it only through one
of the converted edges gain a direct `deps` entry on
`:serial_port_actions`. Targets that reach it through `fake_serial_backend`
or `fake_backed_serial` (for example `flash/flash_workflow_test.cpp`) are
unaffected, because those fixtures keep plain `deps`. The sandboxed build names
the full set; the fix is always a direct `deps` entry on the including target,
never turning an `implementation_deps` edge back into `deps`.

### Split `websocket_io` out of `remote_serial_backend`

New target in the serial package:

```starlark
qt_cc_library(
    name = "websocket_io",
    srcs = ["websocketiodevice.cpp"],
    hdrs = ["websocketiodevice.h"],
    copts = COMMON_COPTS,
    normal_hdrs = ["qtrohelper.hpp"],
    visibility = [
        "//src/platform/desktop/common/remote_utility:__pkg__",
        "//src/platform/desktop/common/serial:__pkg__",
    ],
    deps = QT_DEPS,
)
```

`remote_serial_backend` drops those files and depends on `:websocket_io`;
`remote_utility` depends on `:websocket_io` instead of
`:remote_serial_backend`, and `remote_serial_backend`'s visibility loses
`remote_utility`. This removes the last path from the UI to a facade header
while leaving the GRANDFATHERED edge itself for 6j.

`websocketiodevice.h` is moc'd (`Q_OBJECT`), and its moc genrule is named
after the header's basename, so it must be listed in `hdrs` of exactly one
target (see "One header cannot be moc'd by two targets" in the
[design notes](../../design-notes.md)); moving it, not copying it, satisfies
that.

### Remove freeze leftovers

- Move `fake_backed_serial` (`fake_backed_serial.h` and its target, plus
  `test_fake_backed_serial`) from `transport` to `serial/testing`, beside
  `fake_backend.h`. It lived in `transport` only because that package was
  already on the frozen list. Update its includers' paths and deps.
- Delete the comments explaining a placement by the freeze or calling a
  target transitional: in the serial, `transport`, `connection`,
  `connection/testing`, `diagnostics`, `service_functions`, `serial/testing`,
  and `ui/desktop/flash/operation` BUILD files. The `transport` package's
  `default_visibility` comment for `//src/ui/desktop/service_functions`
  keeps its reason but drops the reference to the allowlist.

### Documentation

- **Modularization plan:** mark 6i complete; name 6j (the two GRANDFATHERED
  edges) as next and not yet designed; update the baseline's guard list (two
  root guards become one plus the ADR 0016 visibility rule).
- **Tech-debt roadmap:** delete "P1: Drain the `serial_qt_compat` allowlist";
  add a line to "P1: Narrow serial and hardware interfaces" that the facade
  is now platform-only by visibility and `implementation_deps`; add a P1 entry
  for the two GRANDFATHERED edges.
- **Design notes:** a "Serial facade retirement" section covering why
  visibility plus `implementation_deps` replaced the script, the Windows
  sandbox limit, the `websocket_io` split, and that earlier notes naming
  `serial_qt_compat` or `serial_platform_api` describe history.
- **FastECU `CLAUDE.md`:** the "Ratchet lists only shrink" bullet names the
  `serial_qt_compat` visibility list; it then names only the `qt_layer`
  package group.

## Implementation sequence

Stacked with `gh stack`, each PR green on its own:

1. **6i-0 — this spec and its plan.** Documentation only.
2. **6i-1 — rename and retire the guard.** Rename to `serial_port_actions`
   with the new visibility; delete `serial_platform_api`; delete the root
   guard, its script, and the `BUILD.bazel` export; move `fake_backed_serial`
   to `serial/testing`; remove the freeze comments. Label changes only.
3. **6i-2 — close the UI reach and close out.** `implementation_deps`
   conversion and the direct test deps it exposes; the `websocket_io` split;
   the negative-include proof; the documentation above; delete this spec and
   its plan, as the 6h close-out did.

## Verification

- `bazel build --config=release //...` and `bazel test --config=release //...`
  locally on macOS; CI on Linux, macOS, and Windows.
- `prek run --all-files`; `bazel run //:clang_tidy_report_changed`.
- 6i-1: `bazel query 'rdeps(//..., //src/platform/desktop/common/serial:serial_port_actions, 1)'`
  lists only `//src/platform/desktop/...` and `//tests` targets; recorded in
  the PR.
- 6i-2, negative proof: add `#include "src/platform/desktop/common/serial/serial_port_actions.h"`
  to `src/ui/desktop/mainwindow.cpp`, run `bazel build //src/ui/desktop:desktop`
  on macOS, observe the "file not found" failure, revert. Repeat with
  `serial_backend.h` to prove the `remote_utility` path is closed. Record both
  in the PR.
- No bench qualification: no behavior changes, so no checklist entry. The
  known intermittent Windows crash in `//tests:serial_backend_tests` is
  pre-existing; rerun it rather than attribute it to this stack.
