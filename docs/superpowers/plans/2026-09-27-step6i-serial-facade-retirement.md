# Step 6i: Serial Facade Retirement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire `serial_qt_compat`, `serial_platform_api`, and the `//:serial_compat_allowlist` ratchet, and stop the serial facade's headers reaching production UI targets.

**Architecture:** Rename the facade library in place to a platform-only `serial_port_actions` target, so visibility guards direct dependencies. Adapters whose public headers only forward-declare `SerialPortActions` take it through `implementation_deps`, so its headers leave their dependents' compile inputs. One target split (`websocket_io`) removes the last public-header path via `remote_utility`.

**Tech Stack:** Bazel 9.1.1 (Bzlmod), `rules_cc` `cc_library` via the vendored `qt_cc_library` macro (`third_party/qt/qt.bzl`), Qt 6, prek.

**Spec:** [the step 6i design](../specs/2026-09-27-step6i-serial-facade-retirement-design.md)

## Global Constraints

- No production source changes behavior. BUILD files, one target split, two include-path spellings in `remote_utility.h`, one moved test fixture, and documentation only.
- The facade target is `//src/platform/desktop/common/serial:serial_port_actions`, visibility exactly `["//src/platform/desktop:__subpackages__", "//tests:__pkg__"]`.
- A missing header is fixed with a direct `deps` entry on the target that includes it. An `implementation_deps` edge is never turned back into `deps` to make something compile.
- A `Q_OBJECT` header is in `hdrs` of exactly one `qt_cc_library` (moc genrule names collide otherwise).
- No new source-scan guard. No change to `bazel/qt/BUILD.bazel`'s `qt_layer` group.
- The GRANDFATHERED `logging` and `remote_utility` visibility entries for `//src/ui/desktop` stay; they are step 6j.
- Work lands as a `gh stack`: `docs/step6i-serial-facade-retirement` (6i-0, this plan and the spec) → `refactor/step6i-1-serial-port-actions` (Tasks 1–2) → `refactor/step6i-2-close-ui-reach` (Tasks 3–5).
- Every commit ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
  ```

## Review Focus

- **A facade header included from a production UI source.** Expected: the macOS/Linux build of `//src/ui/desktop:desktop` fails with "file not found". Pinned by the negative proof in Task 4, for both `serial_port_actions.h` and `serial_backend.h`.
- **A test that compiled only because a converted adapter leaked the facade headers.** Expected: it gets a direct `deps` entry, not a widened adapter. Pinned by the full `bazel test //...` in Task 4.
- **`remote_utility` after it stops depending on `remote_serial_backend`.** Expected: it still finds `websocketiodevice.h` and `qtrohelper.hpp`, and the remote backend's smoke test still passes. Pinned in Task 3.
- **Windows, which builds unsandboxed.** Expected: all targets still compile there; the negative proof is not expected to fail on Windows. Pinned by CI on the 6i-2 PR, and stated as a limit in the design notes (Task 5).
- **A later change re-adding a UI or backend dependency on the facade.** Expected: an analysis-time visibility error. Pinned by the `rdeps` query in Task 1 and the visibility list itself.

---

## File Structure

| File | Change | Task |
|---|---|---|
| `src/platform/desktop/common/serial/BUILD.bazel` | rename target, delete alias, trim `exports_files`, update internal deps and comments | 1, 3, 4 |
| `BUILD.bazel` (root) | delete `serial_compat_allowlist` | 1 |
| `scripts/check-serial-compat-allowlist.py` | delete | 1 |
| `src/platform/desktop/common/{transport,connection,connection/testing,diagnostics,service_functions}/BUILD.bazel`, `src/platform/desktop/common/serial/testing/BUILD.bazel`, `tests/BUILD.bazel` | relabel facade deps, drop freeze comments | 1, 2, 4 |
| `src/ui/desktop/flash/operation/BUILD.bazel` | drop freeze comment | 2 |
| `src/platform/desktop/common/transport/fake_backed_serial{.h,_test.cpp}` → `src/platform/desktop/common/serial/testing/` | `git mv` | 2 |
| five test sources including `fake_backed_serial.h` | include path | 2 |
| `src/platform/desktop/common/remote_utility/{BUILD.bazel,remote_utility.h}` | depend on `websocket_io`, full include paths | 3 |
| `docs/modularization-plan.md`, `docs/tech-debt.md`, `docs/design-notes.md`, `CLAUDE.md` | close-out | 5 |
| this plan and the spec | delete | 5 |

---

### Task 1: Rename the facade target and retire the allowlist guard (6i-1)

**Files:**
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`
- Modify: `src/platform/desktop/common/serial/testing/BUILD.bazel`
- Modify: `src/platform/desktop/common/transport/BUILD.bazel`
- Modify: `src/platform/desktop/common/connection/BUILD.bazel`
- Modify: `src/platform/desktop/common/connection/testing/BUILD.bazel`
- Modify: `src/platform/desktop/common/diagnostics/BUILD.bazel`
- Modify: `src/platform/desktop/common/service_functions/BUILD.bazel`
- Modify: `tests/BUILD.bazel`
- Modify: `BUILD.bazel`
- Delete: `scripts/check-serial-compat-allowlist.py`

**Interfaces:**
- Produces: `//src/platform/desktop/common/serial:serial_port_actions` — same `srcs`, `hdrs`, `normal_hdrs`, `deps` as today's `serial_qt_compat`. Tasks 2–4 use this label.
- Removes: `:serial_qt_compat`, `:serial_platform_api`, `//:serial_compat_allowlist`.

- [ ] **Step 1: Create the branch on top of the spec branch**

```bash
git switch docs/step6i-serial-facade-retirement
git switch -c refactor/step6i-1-serial-port-actions
```

- [ ] **Step 2: Record the "before" state the change is judged against**

Run:
```bash
bazel query --noshow_progress 'rdeps(//..., //src/platform/desktop/common/serial:serial_qt_compat + //src/platform/desktop/common/serial:serial_platform_api, 1)' | grep -v ':moc_' | sort > /tmp/6i-before.txt; wc -l /tmp/6i-before.txt
```
Expected: 26 lines (the two old targets plus the 24 dependents listed in the spec's "Current state" table). Keep the file for Step 8.

- [ ] **Step 3: Rewrite the facade target**

In `src/platform/desktop/common/serial/BUILD.bazel`, replace the comment block above and the whole `serial_qt_compat` rule with:

```starlark
# The SerialPortActions facade and the SerialBackend seam its two backends
# implement. Platform-only: backend and UI code reach serial I/O through
# backend ports and the platform adapters that implement them, never through
# this target. Adapters whose public headers forward-declare
# SerialPortActions take it as implementation_deps so its headers stay out
# of their dependents' compile inputs.
qt_cc_library(
    name = "serial_port_actions",
    srcs = [
        "serial_backend_host.cpp",
        "serial_port_actions.cpp",
    ],
    hdrs = ["serial_port_actions.h"],
    copts = COMMON_COPTS,
    normal_hdrs = [
        "serial_backend.h",
        "serial_backend_host.h",
        "serial_facade_codes.h",
    ],
    visibility = [
        "//src/platform/desktop:__subpackages__",
        "//tests:__pkg__",
    ],
    deps = QT_DEPS + [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/qt_compat",
    ],
)
```

- [ ] **Step 4: Delete `serial_platform_api` and trim `exports_files`**

In the same file, delete the `serial_platform_api` rule and its comment block (the last rule in the file). Change `exports_files` and its comment to:

```starlark
# root //:windows_preprocessor_guards reads serial_port_actions_direct.h as
# `data`; that reference is plumbing, not a layer dependency, so export it to
# the root package specifically rather than widen the package's general
# default_visibility.
exports_files(
    ["serial_port_actions_direct.h"],
    visibility = ["//:__pkg__"],
)
```

- [ ] **Step 5: Relabel every remaining reference**

Run:
```bash
grep -rl 'serial:serial_qt_compat\|serial:serial_platform_api\|":serial_qt_compat"' --include=BUILD.bazel src tests apps \
  | xargs sed -i '' \
      -e 's#//src/platform/desktop/common/serial:serial_qt_compat#//src/platform/desktop/common/serial:serial_port_actions#g' \
      -e 's#//src/platform/desktop/common/serial:serial_platform_api#//src/platform/desktop/common/serial:serial_port_actions#g' \
      -e 's#":serial_qt_compat"#":serial_port_actions"#g'
grep -rn 'serial_qt_compat\|serial_platform_api' --include=BUILD.bazel src tests apps
```
(On Linux use `sed -i` without `''`.) Expected: the second `grep` prints only comments. No target lists both old labels today, so the rename cannot create a duplicate `deps` entry; `prek` (buildifier) would flag one in Step 9.

- [ ] **Step 6: Delete the root guard and its script**

In the root `BUILD.bazel`, delete the whole `py_test(name = "serial_compat_allowlist", ...)` rule. Then:
```bash
git rm scripts/check-serial-compat-allowlist.py
```

- [ ] **Step 7: Build and test**

Run:
```bash
bazel build --config=release //...
bazel test --config=release //...
```
Expected: both succeed; no test changes count. A "target is not visible" error means a package outside `//src/platform/desktop/...` and `//tests` depended on the old labels — stop and report it; the spec says none does.

- [ ] **Step 8: Prove only platform and tests name the new target**

Run:
```bash
bazel query --noshow_progress 'rdeps(//..., //src/platform/desktop/common/serial:serial_port_actions, 1)' | grep -v ':moc_' | sort | tee /tmp/6i-after.txt
grep -v '^//src/platform/desktop/\|^//tests:' /tmp/6i-after.txt
```
Expected: the second `grep` prints nothing. `/tmp/6i-after.txt` has the 24 dependents from Step 2 plus the target itself. Paste the list into the 6i-1 PR description.

- [ ] **Step 9: Lint and commit**

```bash
prek run --all-files
git add -A
git commit -F - <<'EOF'
refactor(serial): rename serial_qt_compat to serial_port_actions (step 6i-1)

The facade library keeps its sources and headers under a non-transitional
name, visible to src/platform/desktop and //tests only. Visibility now
guards direct dependencies, so serial_platform_api (a re-export that let
platform packages avoid the frozen list) and the //:serial_compat_allowlist
ratchet are deleted.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
EOF
```

---

### Task 2: Move `fake_backed_serial` beside `fake_backend.h` and drop freeze comments (6i-1)

**Files:**
- Move: `src/platform/desktop/common/transport/fake_backed_serial.h` → `src/platform/desktop/common/serial/testing/fake_backed_serial.h`
- Move: `src/platform/desktop/common/transport/fake_backed_serial_test.cpp` → `src/platform/desktop/common/serial/testing/fake_backed_serial_test.cpp`
- Modify: `src/platform/desktop/common/serial/testing/BUILD.bazel`
- Modify: `src/platform/desktop/common/transport/BUILD.bazel`
- Modify: `src/platform/desktop/common/connection/BUILD.bazel`
- Modify: `src/platform/desktop/common/diagnostics/BUILD.bazel`
- Modify: `src/ui/desktop/flash/operation/BUILD.bazel`
- Modify (include path): `transport/desktop_logging_protocol_registration_test.cpp`, `transport/desktop_can_flash_transport_test.cpp`, `transport/desktop_kline_flash_transport_test.cpp`, `connection/adapter_connection_test.cpp`, `diagnostics/serial_diagnostic_link_test.cpp` (all under `src/platform/desktop/common/`)

**Interfaces:**
- Consumes: `//src/platform/desktop/common/serial:serial_port_actions` (Task 1).
- Produces: `//src/platform/desktop/common/serial/testing:fake_backed_serial` (testonly) and `:test_fake_backed_serial` in that package; `//src/platform/desktop/common/transport:fake_backed_serial` no longer exists.

- [ ] **Step 1: Move the files**

```bash
git mv src/platform/desktop/common/transport/fake_backed_serial.h src/platform/desktop/common/serial/testing/
git mv src/platform/desktop/common/transport/fake_backed_serial_test.cpp src/platform/desktop/common/serial/testing/
grep -rl 'common/transport/fake_backed_serial.h' src tests \
  | xargs sed -i '' 's#common/transport/fake_backed_serial.h#common/serial/testing/fake_backed_serial.h#'
```

- [ ] **Step 2: Move the targets**

In `src/platform/desktop/common/transport/BUILD.bazel`, delete the `fake_backed_serial` rule with its comment block, and the `test_fake_backed_serial` rule. Append to `src/platform/desktop/common/serial/testing/BUILD.bazel`:

```starlark
# A SerialPortActions facade over a Google Mock FakeBackend.
qt_cc_library(
    name = "fake_backed_serial",
    testonly = True,
    srcs = [],
    hdrs = [],
    copts = COMMON_COPTS,
    # Header-only class template, no Q_OBJECT: normal_hdrs, not hdrs, so moc
    # does not run over it.
    normal_hdrs = ["fake_backed_serial.h"],
    deps = QT_DEPS + [
        ":fake_serial_backend",
        "//src/platform/desktop/common/serial:serial_port_actions",
        "@googletest//:gtest",
    ],
)

fastecu_qttest(
    name = "test_fake_backed_serial",
    src = "fake_backed_serial_test.cpp",
    deps = [":fake_backed_serial"],
)
```

Then relabel its users:
```bash
grep -rl 'transport:fake_backed_serial\|":fake_backed_serial"' --include=BUILD.bazel src \
  | grep -v 'serial/testing/BUILD.bazel' \
  | xargs sed -i '' \
      -e 's#//src/platform/desktop/common/transport:fake_backed_serial#//src/platform/desktop/common/serial/testing:fake_backed_serial#g' \
      -e 's#":fake_backed_serial"#"//src/platform/desktop/common/serial/testing:fake_backed_serial"#g'
```

- [ ] **Step 3: Replace the freeze comments**

Make exactly these comment edits (no rule changes):

`src/platform/desktop/common/transport/BUILD.bazel`, in `default_visibility`:
```starlark
    # The service-function dialog owns the SSM transport adapter it passes
    # into ServiceFunctionWorker.
    "//src/ui/desktop/service_functions:__pkg__",
```

`src/platform/desktop/common/connection/BUILD.bazel`, above `adapter_connection`:
```starlark
# MainWindow's connection handling over the serial facade. The header
# forward-declares SerialPortActions, so the UI never includes the facade
# header through it.
```

`src/platform/desktop/common/diagnostics/BUILD.bazel`, above `serial_diagnostic_link`:
```starlark
# IDiagnosticLink over the serial facade. The header forward-declares
# SerialPortActions, so UI callers never see the facade header.
```

`src/platform/desktop/common/serial/testing/BUILD.bazel`, in `fake_serial_backend`'s deps comment:
```starlark
        # FakeBackend derives from SerialPortActionsDirect
        # (serial_port_actions_direct.h), owned by the platform's direct
        # backend; :direct_serial_backend_for_tests is its testonly alias.
```

`src/ui/desktop/flash/operation/BUILD.bazel`, above `flash_operation_controller`, change the last sentence to: `It only passes SerialPortActions* through, so it needs no facade dependency.`

In `src/platform/desktop/common/serial/BUILD.bazel`, replace the `desktop_serial_factory` comment with:
```starlark
# Constructor-only entry point to SerialPortActions for the composition
# roots: they own the facade without seeing its API.
```
and the `serial_idle` comment with:
```starlark
# The idle line state MainWindow restores before and after every ECU
# operation. It calls the facade, so it lives beside it.
```

- [ ] **Step 4: Confirm no freeze language is left**

Run:
```bash
grep -rn 'serial_qt_compat\|serial_platform_api\|frozen\|FROZEN\|allowlist' --include=BUILD.bazel src tests apps
```
Expected: no output.

- [ ] **Step 5: Build, test, lint, commit**

```bash
bazel test --config=release //src/platform/... //src/ui/... //tests/...
prek run --all-files
git add -A
git commit -F - <<'EOF'
refactor(serial): move fake_backed_serial beside fake_backend (step 6i-1)

The fixture lived in transport only because that package was already on
the frozen serial_qt_compat list. Comments that justified placements by
the freeze now state the real reason or are gone.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
EOF
```
Expected: all tests pass, including `//src/platform/desktop/common/serial/testing:test_fake_backed_serial`.

---

### Task 3: Split `websocket_io` out of `remote_serial_backend` (6i-2)

**Files:**
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`
- Modify: `src/platform/desktop/common/remote_utility/BUILD.bazel`
- Modify: `src/platform/desktop/common/remote_utility/remote_utility.h:5-6`

**Interfaces:**
- Produces: `//src/platform/desktop/common/serial:websocket_io` (`websocketiodevice.{h,cpp}`, `qtrohelper.hpp`), visible to the serial and `remote_utility` packages.
- `remote_serial_backend` keeps its label and public header; its visibility loses `remote_utility`.

- [ ] **Step 1: Create the branch**

```bash
git switch refactor/step6i-1-serial-port-actions
git switch -c refactor/step6i-2-close-ui-reach
```

- [ ] **Step 2: Show the path being removed (the failing check)**

Run:
```bash
bazel query --noshow_progress 'somepath(//src/platform/desktop/common/remote_utility:remote_utility, //src/platform/desktop/common/serial:serial_port_actions)'
```
Expected: a path through `//src/platform/desktop/common/serial:remote_serial_backend`. After Step 5 the same query must print nothing.

- [ ] **Step 3: Add `websocket_io` and slim `remote_serial_backend`**

In `src/platform/desktop/common/serial/BUILD.bazel`, replace the `remote_serial_backend` comment and rule with:

```starlark
# The QIODevice-over-websocket adapter and the Qt Remote Objects helper,
# shared by the remote serial backend and remote_utility. Carries no facade
# header, so remote_utility (and the UI above it) does not reach the facade.
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

# The remote backend (Qt Remote Objects over a websocket).
qt_cc_library(
    name = "remote_serial_backend",
    srcs = ["remote_serial_backend.cpp"],
    hdrs = ["remote_serial_backend.h"],
    copts = COMMON_COPTS,
    visibility = ["//src/platform/desktop/common/serial:__pkg__"],
    deps = QT_DEPS + [
        ":serial_port_actions",
        ":serial_replicas",
        ":websocket_io",
    ],
)
```

`websocketiodevice.h` moves from one target's `hdrs` to another's; it must not stay in both.

- [ ] **Step 4: Point `remote_utility` at `websocket_io`**

In `src/platform/desktop/common/remote_utility/BUILD.bazel`, in the `remote_utility` rule, replace the dependency and its comment with:

```starlark
        # remote_utility.h includes websocketiodevice.h and qtrohelper.hpp.
        "//src/platform/desktop/common/serial:websocket_io",
```

In `src/platform/desktop/common/remote_utility/remote_utility.h`, replace lines 5–6:

```cpp
#include "src/platform/desktop/common/serial/websocketiodevice.h"
#include "src/platform/desktop/common/serial/qtrohelper.hpp"
```

The bare spellings resolved only because `serial_replicas` (a `qt_replica_library` with `includes = ["."]`) put the serial directory on the include path through `remote_serial_backend`; `websocket_io` does not.

- [ ] **Step 5: Verify the path is gone and remote code still works**

Run:
```bash
bazel query --noshow_progress 'somepath(//src/platform/desktop/common/remote_utility:remote_utility, //src/platform/desktop/common/serial:serial_port_actions)'
bazel test --config=release //src/platform/desktop/common/serial:test_remote_backend_smoke //src/platform/desktop/common/serial:desktop_serial_factory_test
bazel build --config=release //:fastecu //apps/bench:fastecu-bench
```
Expected: the query prints nothing; tests pass; both binaries build.

- [ ] **Step 6: Lint and commit**

```bash
prek run --all-files
git add -A
git commit -F - <<'EOF'
refactor(serial): split websocket_io out of remote_serial_backend (step 6i-2)

remote_utility needs only the websocket device and QtRO helper, but took
them from remote_serial_backend, whose public header includes the facade's
serial_backend.h. Through the grandfathered UI edge to remote_utility, that
put facade headers in the UI's compile inputs.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
EOF
```

---

### Task 4: Take the facade through `implementation_deps` (6i-2)

**Files:**
- Modify: `src/platform/desktop/common/transport/BUILD.bazel` (`transport`, `flash_transports`, `logging_protocol_registration`)
- Modify: `src/platform/desktop/common/connection/BUILD.bazel` (`adapter_connection`)
- Modify: `src/platform/desktop/common/connection/testing/BUILD.bazel` (`adapter_connection_harness`)
- Modify: `src/platform/desktop/common/diagnostics/BUILD.bazel` (`serial_diagnostic_link`)
- Modify: `src/platform/desktop/common/service_functions/BUILD.bazel` (`service_function_worker`)
- Modify: `src/platform/desktop/common/serial/BUILD.bazel` (`serial_idle`, `desktop_serial_factory`)
- Modify as the build demands: test targets' `deps`

**Interfaces:**
- Consumes: `:serial_port_actions` (Task 1), `:websocket_io` (Task 3).
- Produces: no new labels. The listed targets expose no facade header to dependents.

Targets that **keep** plain `deps` on the facade, because a public header includes a facade header: `direct_serial_backend_{unix,windows}`, `serial_port_actions_direct_moc`, `remote_serial_backend`, `serial/testing:fake_serial_backend`, `serial/testing:fake_backed_serial`. None is reachable from a production UI target.

- [ ] **Step 1: Demonstrate the gap (the failing test)**

Add as the first `#include` line in `src/ui/desktop/mainwindow.cpp`:
```cpp
#include "src/platform/desktop/common/serial/serial_port_actions.h"
```
Run: `bazel build --config=release //src/ui/desktop:desktop`
Expected: **builds**. This is the gap. Revert the line: `git checkout src/ui/desktop/mainwindow.cpp`.

- [ ] **Step 2: Move the facade edge to `implementation_deps`**

In each target below, delete `"//src/platform/desktop/common/serial:serial_port_actions",` (or `":serial_port_actions",` inside the serial package) from `deps` and add, as a sibling attribute after `deps`:

```starlark
    implementation_deps = ["//src/platform/desktop/common/serial:serial_port_actions"],
```
(inside the serial package: `implementation_deps = [":serial_port_actions"],`)

Targets: `transport:transport`, `transport:flash_transports`, `transport:logging_protocol_registration`, `connection:adapter_connection`, `connection/testing:adapter_connection_harness`, `diagnostics:serial_diagnostic_link`, `service_functions:service_function_worker`, `serial:serial_idle`, `serial:desktop_serial_factory`.

Buildifier orders attributes; let `prek` fix placement rather than hand-sorting.

- [ ] **Step 3: Build and test; fix what the sandbox names**

Run:
```bash
bazel build --config=release -k //...
bazel test --config=release -k //...
```
Expected: most targets pass. A failure reading `fatal error: '.../serial_port_actions.h' file not found` (or another facade header) in a **test or testonly** target means it included the header but received it only through a converted adapter. Fix each by adding `"//src/platform/desktop/common/serial:serial_port_actions"` to that target's `deps`. Most tests already receive it through `fake_serial_backend` or `fake_backed_serial`, so expect few or none. A failure in a **production** target outside the serial package means its own `.cpp` includes the facade header while it depended only transitively; add the same label to its `implementation_deps`. Repeat until both commands pass.

- [ ] **Step 4: Prove the gap is closed (the passing test)**

Add the Step 1 include to `src/ui/desktop/mainwindow.cpp` again.
Run: `bazel build --config=release //src/ui/desktop:desktop`
Expected: FAIL with `'src/platform/desktop/common/serial/serial_port_actions.h' file not found`. Revert.

Repeat with `#include "src/platform/desktop/common/serial/serial_backend.h"`. Expected: the same failure for `serial_backend.h`. Revert.

Repeat both includes in `src/ui/desktop/service_functions/service_function_dialog.cpp`, building `//src/ui/desktop/service_functions:service_function_dialog`. Expected: the same two failures. Revert.

Copy the four error lines into a note for the 6i-2 PR description. Then confirm the tree is clean: `git status --short src/ui` prints nothing.

- [ ] **Step 5: Lint and commit**

```bash
prek run --all-files
bazel run //:clang_tidy_report_changed
git add -A
git commit -F - <<'EOF'
refactor(platform): take the serial facade through implementation_deps (step 6i-2)

Every adapter whose public headers forward-declare SerialPortActions now
depends on the facade through implementation_deps, so the facade headers
leave the compile inputs of everything above them. A production UI source
that includes serial_port_actions.h or serial_backend.h now fails to build
on the sandboxed Linux and macOS builds.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
EOF
```

---

### Task 5: Close out step 6i in the docs (6i-2)

**Files:**
- Modify: `docs/modularization-plan.md`
- Modify: `docs/tech-debt.md`
- Modify: `docs/design-notes.md`
- Modify: `CLAUDE.md`
- Delete: `docs/superpowers/specs/2026-09-27-step6i-serial-facade-retirement-design.md`, `docs/superpowers/plans/2026-09-27-step6i-serial-facade-retirement.md`

**Interfaces:**
- Consumes: the facts from Tasks 1–4 (labels, the Windows limit, the negative-proof result).

- [ ] **Step 1: Modularization plan**

In the Status section, replace the sentence beginning "Next is **6i**" through "Step 7 (Android seam) has not started." with:

```markdown
**6i (serial facade retirement)** is complete: `serial_qt_compat` is now the
platform-only `serial_port_actions`, `serial_platform_api` and
`//:serial_compat_allowlist` are gone, and no facade header reaches a
production UI target. Next is **6j**: the two GRANDFATHERED `ui → platform`
edges, `SystemLogger` and `RemoteUtility`, not yet designed. Step 7 (Android
seam) has not started.
```

In "Verified Current Baseline", in the bullet beginning "Two CI guards enforce", change "Two CI guards" to "One CI guard" and delete the clause `and \`//:serial_compat_allowlist\` (frozen, shrink-only)`. In the bullet beginning "The `serial_qt_compat` allowlist has shrunk", append:

```markdown
  Step 6i deleted the target and its guard; see the 6i entry under step 6.
```

In the 6h entry, replace `(an unguarded gap, recorded in the [tech-debt roadmap](tech-debt.md) for 6i)` with `(closed by 6i)`.

Insert after the 6h entry and before the "Remove compatibility wrappers" bullet:

```markdown
   - **6i serial facade retirement — complete.** `serial_qt_compat` was
     renamed in place to `//src/platform/desktop/common/serial:serial_port_actions`,
     visible only to `//src/platform/desktop:__subpackages__` and `//tests`,
     so visibility now guards direct dependencies. `serial_platform_api`,
     `//:serial_compat_allowlist`, and its script are deleted. Every adapter
     that forward-declares `SerialPortActions` takes it through
     `implementation_deps`, and `websocket_io` was split out of
     `remote_serial_backend` so `remote_utility` stops carrying
     `serial_backend.h`; a production UI include of a facade header now fails
     on the sandboxed builds. `fake_backed_serial` moved to
     `serial/testing`. No behavior change, so no bench checklist. See the
     [design notes](design-notes.md#serial-facade-retirement).
```

- [ ] **Step 2: Tech-debt roadmap**

Delete the whole `### P1: Drain the \`serial_qt_compat\` allowlist` section (from its heading to the line before `### P2: Identify Subaru CAN ECUs with SSM \`AA\``).

Append to the prose of `### P1: Narrow serial and hardware interfaces`, before its "Actions:" line:

```markdown
Since step 6i the facade is platform-only: `serial_port_actions` is visible
to `src/platform/desktop` and `//tests` alone, and the adapters above it take
it through `implementation_deps`, so its headers do not reach production UI
targets. UI tests still see them through `FakeBackend`, which derives from
`SerialPortActionsDirect`.
```

Insert a new section immediately before `### P2: Identify Subaru CAN ECUs with SSM \`AA\``:

```markdown
### P1: Remove the GRANDFATHERED UI → platform edges (step 6j)

`//src/ui/desktop` still depends directly on two platform packages, each
marked GRANDFATHERED in its `default_visibility`:
`//src/platform/desktop/common/logging` (`SystemLogger`, used across
`MainWindow` and `menu_actions`) and
`//src/platform/desktop/common/remote_utility` (`RemoteUtility`, used by
`MainWindow`). They are the last `ui → platform` edges outside the
composition root.

Actions:

- Design a seam for each through `MainWindowServices`, as 6f and 6h did
  for logging protocols and the connection.
- Remove each GRANDFATHERED entry with the code that needed it.
```

- [ ] **Step 3: Design notes**

Insert before `## Testing`:

```markdown
## Serial facade retirement

### Visibility and `implementation_deps` replaced the allowlist script

`//:serial_compat_allowlist` froze `serial_qt_compat`'s visibility list so it
could only shrink. Once only platform packages and `//tests` remained, the
freeze had nothing left to protect, and it never covered transitive reach:
the facade headers still arrived in UI compile actions through ordinary
`deps` on the adapters. Step 6i renamed the target to `serial_port_actions`,
restricted its visibility to `//src/platform/desktop:__subpackages__` and
`//tests`, and moved every adapter whose public header forward-declares
`SerialPortActions` onto `implementation_deps`. Bazel still links the facade
into dependents, but its headers are no longer inputs to their compiles.
Notes above that name `serial_qt_compat` or `serial_platform_api` describe
the state before this step.

### Windows does not enforce it

The sandbox is what turns a missing header input into "file not found".
Windows builds are unsandboxed, so there the header is on disk under the
execroot and a UI include of it would compile. Linux and macOS CI catch the
same include, so the check holds across the three-platform matrix, not on
any one Windows machine.

### UI tests still see the facade headers

`connection/testing:adapter_connection_harness` hands UI tests a
`FakeBackend`, which derives from `SerialPortActionsDirect`, so testonly UI
targets receive `serial_backend.h` and its siblings. Tests need the concrete
fake to set expectations; production targets do not reach it.

### `websocket_io` is separate from the remote backend

`remote_utility.h` needs `websocketiodevice.h` and `qtrohelper.hpp` and took
them from `remote_serial_backend`, whose public header includes
`serial_backend.h`. Through the GRANDFATHERED UI edge to `remote_utility`
that was the one remaining path from production UI code to a facade header.
The two files are now `serial:websocket_io`, and `remote_utility.h` includes
them by full path: the bare spellings had resolved only through
`serial_replicas`' `includes = ["."]`.
```

- [ ] **Step 4: FastECU `CLAUDE.md`**

In the "Ratchet lists only shrink" bullet, replace
`Some guards freeze a list of remaining transitional debt — the \`serial_qt_compat\` visibility list and the \`qt_layer\` package group in \`bazel/qt/BUILD.bazel\`.`
with
`Some guards freeze a list of remaining transitional debt — the \`qt_layer\` package group in \`bazel/qt/BUILD.bazel\` and the GRANDFATHERED \`//src/ui/desktop\` entries in platform packages' \`default_visibility\`.`

- [ ] **Step 5: Delete the spec and this plan**

```bash
git rm docs/superpowers/specs/2026-09-27-step6i-serial-facade-retirement-design.md \
       docs/superpowers/plans/2026-09-27-step6i-serial-facade-retirement.md
```

- [ ] **Step 6: Lint and commit**

```bash
prek run --all-files
git add -A
git commit -F - <<'EOF'
docs: close out step 6i (serial facade retirement)

Records 6i in the modularization plan, replaces the drained allowlist
tech-debt entry with the step 6j GRANDFATHERED-edge entry, adds the design
notes, and removes the spec and plan.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
EOF
```
Expected: `lychee` passes (the new `design-notes.md#serial-facade-retirement` anchor exists).

---

### Task 6: Publish the stack

Only after the user authorizes pushing.

- [ ] **Step 1: Create and submit the stack**

```bash
gh stack init --base master docs/step6i-serial-facade-retirement refactor/step6i-1-serial-port-actions refactor/step6i-2-close-ui-reach
gh stack submit --auto
```

- [ ] **Step 2: Fill in the PR descriptions**

- 6i-0: spec and plan; documentation only; prek clean; notes that the close-out deletes them.
- 6i-1: the `rdeps` list from Task 1 Step 8; "Label changes only; no behavior change."
- 6i-2: the four negative-proof error lines from Task 4 Step 4; the Windows limit; "No behavior change; no bench checklist."

Each description ends with:
```
🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_016apFtj1w8tGipmUYm63qyQ
```

- [ ] **Step 3: Watch CI**

If `//tests:serial_backend_tests` crashes on Windows only, rerun it; it is a known pre-existing flake.
