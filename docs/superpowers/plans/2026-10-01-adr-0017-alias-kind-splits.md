# ADR 0017 `alias_kind` Splits Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the last three `alias_kind` whole-rule keeps from `KEPT_CPP_PRODUCTION_RULES` so only the two DLL fixtures remain as permanent exceptions.

**Architecture:** Split each of three mixed packages into an ordinary parent and a moc child that maps `cc_library` to `qt_cc_library` locally, then let Gazelle generate the child. Each task moves files, repoints includes and consumers, deletes one allowlist entry and one `alias_kind` directive, and leaves the tree green.

**Tech Stack:** Bazel (version pinned in `.bazelversion`), Gazelle 0.54.0 + gazelle_cc 0.6.0 (unmodified), Qt 6 moc via `//bazel:qt_targets.bzl`, `scripts/gazelle_check.py`, `prek`.

**Spec:** [2026-10-01-adr-0017-alias-kind-splits-design.md](../specs/2026-10-01-adr-0017-alias-kind-splits-design.md)

## Global Constraints

- Gazelle 0.54.0 and gazelle_cc 0.6.0 stay unmodified.
- No behavior change in any target; no label may change meaning silently.
- The allowlist only shrinks: an entry is deleted in the same change that migrates its rule, and no new whole-rule keep may be added (narrow attribute/dependency keeps are fine).
- A package is either ordinary (`cc_library`, `COMMON_COPTS`) or a moc package (local `map_kind cc_library qt_cc_library`, every generated `hdrs` entry declares `Q_OBJECT`); a non-moc child of a moc package undoes the mapping with `map_kind cc_library cc_library`.
- Visibility is written with groups from `//bazel/layers`; a one-off per-package grant is a commented literal.
- Platform differences stay in BUILD selects, not `#ifdef` in shared sources; keep the existing OS `target_compatible_with` selects verbatim.
- Cross-document references in Markdown are links, not backticked paths.
- Work lands through a PR from a branch; `prek` refuses commits on `master`. Do not push or open the PR unless the user says so.
- Commit trailers (every commit):
  `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01ERK8Rmjec6hpdoxGgozp76`.

## Review Focus

- A stale old include path or label in a file the greps below do not reach (docs, scripts, IDE configs): Tasks 1-3 end with a repository-wide `git grep` for the old path that must return nothing outside `docs/adr/` history.
- A consumer silently gaining or losing access through visibility: Task 1's child is private to the `ports` package and Task 2/3 children use existing groups; each task runs `bazel build` on every consumer package so a visibility mistake fails at analysis time.
- The moc object dropping out of a final link because the sink is only reached through the `ports` re-export: Task 1 builds and runs the `apps/desktop` binary target and the platform worker tests that link it.
- Gazelle creating a duplicate rule instead of merging into the hand-written moved rule (library named after the directory): each task runs `--fix` twice and inspects the diff; the second run must be empty.
- Windows-only and Linux-only select branches that cannot be built on this Mac: Task 3 queries the Windows J2534 and serial-selection packages to confirm they still load and resolve labels.

---

## File Structure

| Path | Responsibility after the change |
| --- | --- |
| `src/platform/desktop/common/ports/event_sink/` (new) | moc package: `qt_event_sink.{h,cpp}` and its BUILD |
| `src/platform/desktop/common/ports/BUILD.bazel` | ordinary package; re-exports the sink via a dependency keep |
| `src/ui/desktop/definition/dialog/` (new) | moc package: authoring dialog, its test and BUILD |
| `src/ui/desktop/definition/BUILD.bazel` | ordinary package: `definition_header_form` and its test |
| `src/platform/desktop/unix/j2534/driver/` (new) | moc package: `J2534_unix.{h,cpp}` and its BUILD |
| `src/platform/desktop/unix/j2534/BUILD.bazel` | ordinary package: `j2534_api`, `j2534_types`, `serial_byte_buffer` |
| `scripts/gazelle_check.py` | allowlist loses three entries |
| `docs/adr/0017-generate-bazel-targets-with-gazelle.md` | alias-merging text and exception list rewritten |

---

### Task 1: Event sink child package

**Files:**
- Create: `src/platform/desktop/common/ports/event_sink/BUILD.bazel`
- Move: `src/platform/desktop/common/ports/qt_event_sink.h` and `qt_event_sink.cpp` to `src/platform/desktop/common/ports/event_sink/`
- Modify: `src/platform/desktop/common/ports/BUILD.bazel`
- Modify (include path + resolve directive): `apps/desktop/BUILD.bazel`, `apps/desktop/desktop_composition.h`, `src/platform/desktop/common/diagnostics/workers/BUILD.bazel`, `.../diagnostics/workers/dtc_worker.cpp`, `src/platform/desktop/common/flash/worker/BUILD.bazel`, `.../flash/worker/flash_worker.cpp`, `src/platform/desktop/common/logging/runtime/BUILD.bazel`, `.../logging/runtime/logging_engine.h`, `src/platform/desktop/common/service_functions/worker/BUILD.bazel`, `.../service_functions/worker/service_function_worker.cpp`, `src/ui/desktop/service_functions/dialog/BUILD.bazel`, `src/ui/desktop/widgets/BUILD.bazel`, `src/ui/desktop/widgets/mainwindow.h`, `src/ui/desktop/widgets/mainwindow_test.cpp`, `src/ui/desktop/widgets/settings_test.cpp`, `src/platform/desktop/common/ports/qt_port_adapters_test.cpp`
- Modify: `scripts/gazelle_check.py` (allowlist)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: label `//src/platform/desktop/common/ports/event_sink:qt_event_sink` (visible only to package `//src/platform/desktop/common/ports`); header path `src/platform/desktop/common/ports/event_sink/qt_event_sink.h`. The public label `//src/platform/desktop/common/ports` still exposes the sink, so no consumer's dependency list changes.

- [ ] **Step 1: Record the green baseline**

```bash
python3 scripts/gazelle_check.py --fix && git status --short
bazel test --config=release //src/platform/desktop/common/ports:all //src/platform/desktop/common/diagnostics/... //src/platform/desktop/common/flash/... //src/platform/desktop/common/logging/... //src/platform/desktop/common/service_functions/... //src/ui/desktop/widgets/... //src/ui/desktop/service_functions/...
```

Expected: `git status` shows only the already-written spec and plan; tests PASS (or are skipped as incompatible). If the baseline is red, stop and report.

- [ ] **Step 2: Move the files and rewrite includes**

```bash
mkdir -p src/platform/desktop/common/ports/event_sink
git mv src/platform/desktop/common/ports/qt_event_sink.h src/platform/desktop/common/ports/qt_event_sink.cpp src/platform/desktop/common/ports/event_sink/
old='src/platform/desktop/common/ports/qt_event_sink.h'
new='src/platform/desktop/common/ports/event_sink/qt_event_sink.h'
git grep -l -F "$old" -- ':!docs' ':!.claude' | xargs sed -i '' "s#${old}#${new}#g"
git grep -n -F "ports/qt_event_sink.h" -- ':!docs' ':!.claude'
```

Expected: the final grep prints nothing. `git diff --stat` shows the 7 BUILD resolve directives (apps/desktop, diagnostics/workers, flash/worker, logging/runtime, service_functions/worker, ui service_functions/dialog, ui widgets) and the source includes changed; resolve targets are unchanged (`//src/platform/desktop/common/ports`, widgets uses `...ports:ports`).

- [ ] **Step 3: Create the child BUILD file**

Write `src/platform/desktop/common/ports/event_sink/BUILD.bazel`:

```starlark
# The event sink declares Q_OBJECT; generated hdrs must run through moc.
# gazelle:map_kind cc_library qt_cc_library //bazel:qt_targets.bzl
load("//bazel:qt_targets.bzl", "qt_cc_library")

# Private to the parent package, which re-exports the sink through the stable
# public `ports` label for apps, platform workers and the UI.
package(default_visibility = ["//src/platform/desktop/common/ports:__pkg__"])

qt_cc_library(
    name = "qt_event_sink",
    srcs = ["qt_event_sink.cpp"],
    hdrs = ["qt_event_sink.h"],
    deps = [
        "//bazel/qt:core",
        "//src/backend/ports",
    ],
)
```

- [ ] **Step 4: Edit the parent BUILD file**

In `src/platform/desktop/common/ports/BUILD.bazel`:

1. Delete the line `# gazelle:alias_kind qt_cc_library cc_library`.
2. Change `load("//bazel:qt_targets.bzl", "COMMON_COPTS", "qt_cc_library")` to `load("//bazel:qt_targets.bzl", "COMMON_COPTS")`.
3. In the `ports` library, change `":qt_event_sink",  # keep` to
   `"//src/platform/desktop/common/ports/event_sink:qt_event_sink",  # keep`
   (keep the preceding comment about re-exporting the signal endpoint).
4. Delete the trailing `qt_cc_library(name = "qt_event_sink", ...)` rule together with its three comment lines (`# Temporarily hand-owned...`, `# See ADR 0017...`, `# keep`).
5. In `test_qt_port_adapters`, change the `":qt_event_sink",` dep to `"//src/platform/desktop/common/ports/event_sink:qt_event_sink",`.

- [ ] **Step 5: Delete the allowlist entry**

In `scripts/gazelle_check.py`, remove the line
`"src/platform/desktop/common/ports:qt_event_sink",` from `KEPT_CPP_PRODUCTION_RULES`.

- [ ] **Step 6: Regenerate and confirm it is stable**

```bash
python3 scripts/gazelle_check.py --fix
git diff --stat
python3 scripts/gazelle_check.py --fix
git status --short
```

Expected: the first run may rewrite BUILD files (review the diff: only formatting or dependency ordering in the ports and event_sink BUILD files, no duplicate library such as a rule named `event_sink`); the second run changes nothing beyond what the first produced. If Gazelle added a second rule named after the directory, delete it and make sure the hand-written `qt_event_sink` rule's `srcs`/`hdrs` are intact, then rerun.

- [ ] **Step 7: Build and test every consumer**

```bash
bazel test --config=release //src/platform/desktop/common/ports/... //src/platform/desktop/common/diagnostics/... //src/platform/desktop/common/flash/... //src/platform/desktop/common/logging/... //src/platform/desktop/common/service_functions/... //src/ui/desktop/widgets/... //src/ui/desktop/service_functions/... //apps/...
bazel build --config=release //:fastecu
```

Expected: PASS and a linked app. A visibility error naming `event_sink` means a consumer depends on the child directly; repoint it to `//src/platform/desktop/common/ports`.

- [ ] **Step 8: Confirm no stale references remain**

```bash
git grep -n -E "ports/qt_event_sink|ports:qt_event_sink" -- ':!docs/adr' ':!docs/superpowers' ':!.claude'
```

Expected: no output.

- [ ] **Step 9: Commit**

```bash
git add -A src apps scripts
git commit -m "build: split the Qt event sink into a moc child package

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ERK8Rmjec6hpdoxGgozp76"
```

Note: the spec and plan documents are committed separately in Step 6 of Task 4, so `git add -A src apps scripts` deliberately excludes `docs`.

---

### Task 2: Authoring dialog child package

**Files:**
- Create: `src/ui/desktop/definition/dialog/BUILD.bazel`
- Move: `src/ui/desktop/definition/definition_authoring_dialog.h`, `definition_authoring_dialog.cpp`, `definition_authoring_dialog_test.cpp` to `src/ui/desktop/definition/dialog/`
- Modify: `src/ui/desktop/definition/BUILD.bazel`, `src/platform/desktop/common/definition/BUILD.bazel:15`, `src/ui/desktop/widgets/BUILD.bazel:154`, `src/ui/desktop/widgets/mainwindow.h:58`, `scripts/gazelle_check.py`

**Interfaces:**
- Consumes: Task 1 is independent; none required.
- Produces: label `//src/ui/desktop/definition/dialog:definition_authoring_dialog` (UI layer visibility) and header path `src/ui/desktop/definition/dialog/definition_authoring_dialog.h`. `//src/ui/desktop/definition:definition_header_form` keeps its label.

- [ ] **Step 1: Move the files and rewrite includes**

```bash
mkdir -p src/ui/desktop/definition/dialog
git mv src/ui/desktop/definition/definition_authoring_dialog.h src/ui/desktop/definition/definition_authoring_dialog.cpp src/ui/desktop/definition/definition_authoring_dialog_test.cpp src/ui/desktop/definition/dialog/
old='src/ui/desktop/definition/definition_authoring_dialog.h'
new='src/ui/desktop/definition/dialog/definition_authoring_dialog.h'
git grep -l -F "$old" -- ':!docs' ':!.claude' | xargs sed -i '' "s#${old}#${new}#g"
git grep -n -F "definition/definition_authoring_dialog" -- ':!docs' ':!.claude'
```

Expected: the final grep prints nothing. The dialog's includes of `definition_header_form.h` already use the full path and stay unchanged.

- [ ] **Step 2: Create the child BUILD file**

Write `src/ui/desktop/definition/dialog/BUILD.bazel`:

```starlark
# The dialog declares Q_OBJECT; generated hdrs must run through moc.
# gazelle:map_kind cc_library qt_cc_library //bazel:qt_targets.bzl
# gazelle:map_kind cc_test fastecu_gtest //bazel:gtest_targets.bzl
# gazelle:resolve cc src/ui/desktop/config_fields.h //src/ui/desktop:config_fields
load("//bazel:gtest_targets.bzl", "fastecu_gtest")
load("//bazel:qt_targets.bzl", "qt_cc_library")

package(default_visibility = ["//bazel/layers:ui"])

# Q_OBJECT owns the LOG_* signals and requires generated moc code.
qt_cc_library(
    name = "definition_authoring_dialog",
    srcs = ["definition_authoring_dialog.cpp"],
    hdrs = ["definition_authoring_dialog.h"],  # MOC header
    implementation_deps = ["//src/ui/desktop:config_fields"],
    deps = [
        "//bazel/qt:core",
        "//bazel/qt:widgets",
        "//src/backend/config:config_session",
        "//src/backend/definition:definition_writer",
        "//src/backend/ports",
        "//src/platform/desktop/common/definition:definition_catalog_session",
        "//src/ui/desktop/definition:definition_header_form",
    ],
)

fastecu_gtest(
    name = "definition_authoring_dialog_test",
    srcs = ["definition_authoring_dialog_test.cpp"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [
        ":definition_authoring_dialog",
        "//bazel/qt:core",
        "//bazel/qt:widgets",
        "//src/backend/config/testing:config_session_fixture",
        "//src/backend/ports",
        "//src/backend/ports/testing:in_memory_atomic_file_writer",
        "//src/backend/ports/testing:in_memory_file_repository",
        "//src/backend/ports/testing:result_matchers",
        "//src/platform/desktop/common/definition:definition_catalog_session",
        "//src/platform/desktop/common/ports",
        "//src/platform/desktop/common/testing:signal_recorder",
        "//src/ui/desktop/definition:definition_header_form",
        "@googletest//:gtest",
    ],
)
```

- [ ] **Step 3: Edit the parent BUILD file**

In `src/ui/desktop/definition/BUILD.bazel`:

1. Delete the first line `# gazelle:resolve cc src/ui/desktop/config_fields.h //src/ui/desktop:config_fields` and the line `# gazelle:alias_kind qt_cc_library cc_library`.
2. Change the second load to `load("//bazel:qt_targets.bzl", "COMMON_COPTS")`.
3. Delete the whole `qt_cc_library(name = "definition_authoring_dialog", ...)` rule with its three-line comment block, and the whole `fastecu_gtest(name = "definition_authoring_dialog_test", ...)` rule.

What remains: `definition_header_form` and `definition_header_form_test`.

- [ ] **Step 4: Repoint the consumers and visibility**

1. `src/ui/desktop/widgets/BUILD.bazel`: change `"//src/ui/desktop/definition:definition_authoring_dialog",` to `"//src/ui/desktop/definition/dialog:definition_authoring_dialog",` (keep the list sorted).
2. `src/platform/desktop/common/definition/BUILD.bazel`: in `definition_catalog_session`'s `visibility`, change `"//src/ui/desktop/definition:__pkg__",` to `"//src/ui/desktop/definition/dialog:__pkg__",`.
3. `scripts/gazelle_check.py`: remove `"src/ui/desktop/definition:definition_authoring_dialog",` from the allowlist.

- [ ] **Step 5: Regenerate and confirm it is stable**

```bash
python3 scripts/gazelle_check.py --fix
git diff --stat
python3 scripts/gazelle_check.py --fix
git status --short
```

Expected: same standard as Task 1 Step 6. Pay attention to the widgets BUILD: Gazelle may reorder or rewrite the dialog dependency; accept its output.

- [ ] **Step 6: Build and test**

```bash
bazel test --config=release //src/ui/desktop/definition/... //src/ui/desktop/widgets/... //src/platform/desktop/common/definition/...
bazel build --config=release //:fastecu
```

Expected: PASS, including `definition_authoring_dialog_test` and `definition_header_form_test`.

- [ ] **Step 7: Confirm no stale references remain**

```bash
git grep -n -E "ui/desktop/definition/definition_authoring_dialog|definition:definition_authoring_dialog|ui/desktop/definition:__pkg__" -- ':!docs/adr' ':!docs/superpowers' ':!.claude'
```

Expected: no output.

- [ ] **Step 8: Commit**

```bash
git add -A src scripts
git commit -m "build: split the definition authoring dialog into a moc child package

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ERK8Rmjec6hpdoxGgozp76"
```

---

### Task 3: Unix J2534 driver child package

**Files:**
- Create: `src/platform/desktop/unix/j2534/driver/BUILD.bazel`
- Move: `src/platform/desktop/unix/j2534/J2534_unix.h` and `J2534_unix.cpp` to `src/platform/desktop/unix/j2534/driver/`
- Modify: `src/platform/desktop/unix/j2534/BUILD.bazel`, `src/platform/desktop/unix/j2534/j2534_api.h:6`, `src/platform/desktop/unix/j2534/testing/BUILD.bazel:19`, `tests/BUILD.bazel` (resolve at line 15, dependency near line 204), `tests/tst_serial_port_crash.cpp:39`, `scripts/gazelle_check.py`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: label `//src/platform/desktop/unix/j2534/driver:j2534` (visibility `//bazel/layers:platform_and_composition_root`, as the old rule) and header path `src/platform/desktop/unix/j2534/driver/J2534_unix.h`. The label `//src/platform/desktop/unix/j2534` (default target) no longer exists. `j2534_api`, `j2534_types` and `serial_byte_buffer` keep their labels.

- [ ] **Step 1: Move the files and rewrite includes**

```bash
mkdir -p src/platform/desktop/unix/j2534/driver
git mv src/platform/desktop/unix/j2534/J2534_unix.h src/platform/desktop/unix/j2534/J2534_unix.cpp src/platform/desktop/unix/j2534/driver/
old='src/platform/desktop/unix/j2534/J2534_unix.h'
new='src/platform/desktop/unix/j2534/driver/J2534_unix.h'
git grep -l -F "$old" -- ':!docs' ':!.claude' | xargs sed -i '' "s#${old}#${new}#g"
sed -i '' 's#"J2534_tactrix_unix.h"#"src/platform/desktop/unix/j2534/J2534_tactrix_unix.h"#' src/platform/desktop/unix/j2534/driver/J2534_unix.h
git grep -n -E "unix/j2534/J2534_unix|\"J2534_tactrix_unix.h\"" -- ':!docs' ':!.claude'
```

Expected: the final grep prints nothing. The sed on the header turns the relative include of the types header into the full repository path required once the file lives in a child directory.

- [ ] **Step 2: Create the child BUILD file**

Write `src/platform/desktop/unix/j2534/driver/BUILD.bazel`:

```starlark
# The driver declares Q_OBJECT; generated hdrs must run through moc.
# gazelle:map_kind cc_library qt_cc_library //bazel:qt_targets.bzl
load("//bazel:qt_targets.bzl", "qt_cc_library")

package(default_visibility = ["//bazel/layers:platform_and_composition_root"])

qt_cc_library(
    name = "j2534",
    srcs = ["J2534_unix.cpp"],
    hdrs = ["J2534_unix.h"],
    target_compatible_with = select({
        "@platforms//os:windows": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    deps = [
        "//bazel/qt:core",
        "//bazel/qt:serial_port",
        "//src/platform/desktop/unix/j2534:j2534_types",
        "//src/platform/desktop/unix/j2534:serial_byte_buffer",
    ],
)
```

- [ ] **Step 3: Edit the parent BUILD file**

In `src/platform/desktop/unix/j2534/BUILD.bazel`:

1. Delete `# gazelle:alias_kind qt_cc_library cc_library`.
2. Change the load to `load("//bazel:qt_targets.bzl", "COMMON_COPTS")`.
3. Delete the `qt_cc_library(name = "j2534", ...)` rule and its three comment lines.
4. In `j2534_api`, change `deps = [":j2534"],` to `deps = ["//src/platform/desktop/unix/j2534/driver:j2534"],`.

Keep `j2534_types`, `serial_byte_buffer`, its test and the `include_prefix`/`strip_include_prefix` keeps unchanged.

- [ ] **Step 4: Repoint the consumers**

1. `src/platform/desktop/unix/j2534/testing/BUILD.bazel`: `deps = ["//src/platform/desktop/unix/j2534"],` becomes `deps = ["//src/platform/desktop/unix/j2534/driver:j2534"],`.
2. `tests/BUILD.bazel`: the resolve directive was already rewritten by Step 1's sed to the new header path; change its target to `//src/platform/desktop/unix/j2534/driver:j2534`, and change the `"//src/platform/desktop/unix/j2534",` dependency of `serial_crash_tests_test` to `"//src/platform/desktop/unix/j2534/driver:j2534",`.
3. `scripts/gazelle_check.py`: remove `"src/platform/desktop/unix/j2534:j2534",` from the allowlist.

- [ ] **Step 5: Regenerate and confirm it is stable**

```bash
python3 scripts/gazelle_check.py --fix
git diff --stat
python3 scripts/gazelle_check.py --fix
git status --short
```

Expected: same standard as Task 1 Step 6.

- [ ] **Step 6: Build and test**

```bash
bazel test --config=release //src/platform/desktop/unix/... //tests/... //src/platform/desktop/common/serial/...
bazel build --config=release //:fastecu
bazel query 'deps(//src/platform/desktop/windows/j2534:all)' >/dev/null && echo windows-j2534-loads
bazel query 'deps(//src/platform/desktop/common/serial/direct:all)' >/dev/null && echo serial-direct-loads
```

Expected: tests PASS (Unix-only tests run on macOS); both query lines print their message, proving the Windows package and the OS-selecting serial package still load and resolve labels even though they cannot be built here.

- [ ] **Step 7: Confirm no stale references remain**

```bash
git grep -n -E "unix/j2534/J2534_unix|unix/j2534\"|unix/j2534:j2534\b" -- ':!docs/adr' ':!docs/superpowers' ':!.claude'
```

Expected: no output (comments in `tests/tst_mut_dma_integration.cpp` that mention `J2534_unix.cpp` by bare file name are fine and do not match).

- [ ] **Step 8: Commit**

```bash
git add -A src tests scripts
git commit -m "build: split the Unix J2534 driver into a moc child package

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ERK8Rmjec6hpdoxGgozp76"
```

---

### Task 4: ADR text, documents and final verification

**Files:**
- Modify: `docs/adr/0017-generate-bazel-targets-with-gazelle.md`
- Add (already on disk, uncommitted): the spec and this plan under `docs/superpowers/`

**Interfaces:**
- Consumes: the three allowlist entries removed by Tasks 1-3 (the allowlist must now hold exactly the two DLL fixtures, `windows/j2534:pe_bitness_x64_fixture` and `tests:fake_j2534_dll_native`).
- Produces: an ADR that matches the checker.

- [ ] **Step 1: Confirm the allowlist state**

```bash
sed -n '/^KEPT_CPP_PRODUCTION_RULES/,/^)/p' scripts/gazelle_check.py
```

Expected: exactly two entries, `src/platform/desktop/windows/j2534:pe_bitness_x64_fixture` and `tests:fake_j2534_dll_native`.

- [ ] **Step 2: Rewrite the alias-merging paragraph**

In `docs/adr/0017-generate-bazel-targets-with-gazelle.md`, replace the whole paragraph that starts `Gazelle 0.54.0 and gazelle_cc 0.6.0 remain unmodified. In mixed packages,` through `... require updating these rules by hand.` (it includes the three-item bullet list of `qt_event_sink`, `definition_authoring_dialog` and `j2534`) with:

```markdown
Gazelle 0.54.0 and gazelle_cc 0.6.0 remain unmodified, and `alias_kind` is not
used. The pinned combination emits the alias kind on generated rules while
Gazelle's merger looks up mergeable attributes by that kind without its
underlying `cc_library` metadata, so existing attributes on an aliased rule never
regenerate. Mixed packages therefore split their moc libraries into children
instead: `common/ports/event_sink`, `ui/desktop/definition/dialog` and
`unix/j2534/driver`. The event sink child is private to the ports package, which
keeps re-exporting it through the existing `ports` label with a dependency keep,
so apps, workers and the UI see no change. Moving a header requires repointing
its includes and any `gazelle:resolve` directive that names it.
```

Also change the sentence `Unix J2534 publishes ordinary type declarations through `j2534_types`.` to
`Unix J2534 publishes ordinary type declarations through `j2534_types` and keeps its driver in a moc child.`

- [ ] **Step 3: Rewrite the remaining-exceptions list**

In the section `### Homogeneous packages and remaining exceptions`, replace the first bullet (the one listing `qt_event_sink`, `definition_authoring_dialog` and `j2534` with "the pinned `alias_kind` merging limitation described above") so that the list contains only the two DLL-fixture bullet's content, and change the lead-in `now holds only these whole-rule exceptions:` to `now holds only these whole-rule exceptions, both externally sourced or native fixtures with no discoverable local sources:` followed by the two entries (`//src/platform/desktop/windows/j2534:pe_bitness_x64_fixture` and `//tests:fake_j2534_dll_native`). Reword the surrounding text so it no longer mentions a pinned-merging exception.

- [ ] **Step 4: Confirm no `alias_kind` directive or stale label remains**

```bash
git grep -n "gazelle:alias_kind" ; echo "directives: $?"
git grep -n -E "ports:qt_event_sink|definition:definition_authoring_dialog|unix/j2534:j2534\b" -- ':!docs/superpowers'
```

Expected: first grep prints nothing (`directives: 1`); second prints nothing or only historical ADR prose you deliberately kept (there should be none; delete such mentions).

- [ ] **Step 5: Full verification**

```bash
python3 scripts/gazelle_check.py --fix
python3 scripts/gazelle_check.py --fix
git status --short
bazel test --config=release //...
prek run --all-files
```

Expected: the second `--fix` leaves nothing new; `//...` PASS; `prek` clean (it includes the link checker over the new documents). If `ANDROID_NDK_HOME` is set, also run `scripts/android-cross-compile.sh`; it is not expected to be affected.

- [ ] **Step 6: Commit the ADR and documents**

```bash
git add docs/adr/0017-generate-bazel-targets-with-gazelle.md docs/superpowers
git commit -m "docs: record the alias_kind splits in ADR 0017

Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01ERK8Rmjec6hpdoxGgozp76"
```

- [ ] **Step 7: Hand off**

Report the commit list and verification output to the user. Do not push or open the pull request until the user asks; when they do, the PR description ends with the attribution lines required by this session.

---

## Self-Review

- **Spec coverage:** event sink (Task 1, with the re-export design and seven directives), dialog (Task 2, including the `common/definition` grant and widgets consumer), driver (Task 3, including the relative include fix and consumer repoints), bookkeeping (allowlist entries per task, ADR in Task 4; managed roots deliberately untouched because they match by prefix), verification (per task and Task 4), delivery (commits per package plus a docs commit, no push). No checker test was added: `scripts/test_ownership_test.py::test_rejects_stale_allowlist_entry` already pins the stale-entry rule with a synthetic list.
- **Placeholders:** none; ADR replacement text and BUILD contents are given in full. Task 4 Step 3 describes a list edit by structure because the existing bullet text is in the ADR the executor reads.
- **Consistency:** labels used across tasks match the Produces blocks (`ports/event_sink:qt_event_sink`, `definition/dialog:definition_authoring_dialog`, `unix/j2534/driver:j2534`).
