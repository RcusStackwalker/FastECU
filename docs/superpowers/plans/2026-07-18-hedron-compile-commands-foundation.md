# Hedron compile_commands Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a working `compile_commands.json` generator (Hedron extractor) on Bazel 9.1.1 as a standalone, independently-mergeable foundation that the clang-tidy (#24) and SonarCloud (#23) migrations later build on.

**Architecture:** Pin `hedron_compile_commands` to upstream HEAD via `archive_override` with an `integrity` hash, carrying a small patch that makes it load `py_binary`/`cc_binary` from rules_python/rules_cc (both removed from Bazel builtins in Bazel 9). Expose one neutral `refresh_compile_commands` target. Separately, relocate the repo's `external/` directory (which collides with Hedron's generation-time root `external` symlink) to `3rdparty/hardware/`.

**Tech Stack:** Bazel 9.1.1 (bzlmod), Hedron bazel-compile-commands-extractor, rules_cc, rules_python, qmake (`.pro`, still present on master).

## Global Constraints

- Bazel version in use: **9.1.1**.
- Hedron pinned commit: `abb61a688167623088f8768cc9264798df6a9d10` (upstream HEAD, Aug 2025).
- Hedron patch adds `bazel_dep`s at exactly `rules_cc` **0.2.22** and `rules_python` **2.2.0** (match the FastECU root `MODULE.bazel` versions).
- Hedron is a `dev_dependency = True`.
- Compile-commands target: `//bazel/compile_commands:refresh`, scoped to `//:fastecu` + `//tests/...`, with `exclude_external_sources = True`, `exclude_headers = "all"`, and `--config=release` embedded per-target.
- `external/` moves to `3rdparty/hardware/` (NOT root-level `hardware/`).
- NO `pip.parse`, pyyaml, python 3.11 toolchain, or `.bazelrc` `--incompatible_autoload_externally` flag — those are consumer-PR concerns.
- NO CI (`.github/workflows/`) changes in this plan.
- Commit directly to the current branch (repo convention: no feature branches for routine work).

---

### Task 1: Relocate `external/` → `3rdparty/hardware/`

Resolves the root `external` symlink collision independently of the Hedron wiring. Self-contained: verifiable by git state and a stale-reference grep.

**Files:**
- Move: `external/FastECU-mc68hc16-bdm.ino` → `3rdparty/hardware/FastECU-mc68hc16-bdm.ino`
- Move: `external/NANOFastECU-mc68hc16bdm.ino` → `3rdparty/hardware/NANOFastECU-mc68hc16bdm.ino`
- Move: `external/MC68HC16Y5_BDM_TP_800x600.jpg` → `3rdparty/hardware/MC68HC16Y5_BDM_TP_800x600.jpg`
- Modify: `FastECU.pro:392`
- Create: `3rdparty/hardware/README.md`

**Interfaces:**
- Consumes: nothing.
- Produces: the directory `3rdparty/hardware/` (no build target); the repo no longer has a root `external/` directory.

- [ ] **Step 1: Move the three files with git**

```bash
cd /Users/amarkelov/claude-hobby/externals/FastECU
mkdir -p 3rdparty/hardware
git mv external/FastECU-mc68hc16-bdm.ino    3rdparty/hardware/FastECU-mc68hc16-bdm.ino
git mv external/NANOFastECU-mc68hc16bdm.ino 3rdparty/hardware/NANOFastECU-mc68hc16bdm.ino
git mv external/MC68HC16Y5_BDM_TP_800x600.jpg 3rdparty/hardware/MC68HC16Y5_BDM_TP_800x600.jpg
```

- [ ] **Step 2: Update the one qmake reference**

In `FastECU.pro`, line 392, change:

```
    external/FastECU-mc68hc16-bdm.ino \
```

to:

```
    3rdparty/hardware/FastECU-mc68hc16-bdm.ino \
```

- [ ] **Step 3: Add a README so the sketches stay self-describing**

Create `3rdparty/hardware/README.md`:

```markdown
# Hardware programmer sketches

Reference Arduino sketches for the MC68HC16 BDM (background debug mode)
programmer used to read/write Subaru Denso MC68HC16Y5 ECUs on the bench.
These are not compiled by the FastECU build; they are flashed to an
Arduino/Nano with the Arduino IDE.

- `FastECU-mc68hc16-bdm.ino` — BDM programmer sketch (Uno/Mega).
- `NANOFastECU-mc68hc16bdm.ino` — BDM programmer sketch (Nano).
- `MC68HC16Y5_BDM_TP_800x600.jpg` — BDM test-point wiring diagram.

Previously lived under `external/`; moved to avoid colliding with the
Bazel/Hedron generation-time `external` symlink at the workspace root.
```

- [ ] **Step 4: Verify no stale `external/` references remain and the dir is gone**

Run:

```bash
cd /Users/amarkelov/claude-hobby/externals/FastECU
test ! -d external && echo "external/ removed OK" || echo "FAIL: external/ still exists"
grep -rn "external/FastECU-mc68hc16\|external/NANOFastECU\|external/MC68HC16Y5" \
  --include="*.pro" --include="*.pri" --include="*.bazel" --include="*.bzl" . \
  | grep -v "/bazel-" | grep -v "/.claude/" || echo "no stale references OK"
```

Expected: `external/ removed OK` and `no stale references OK` (the grep prints nothing, so the `||` branch fires). References under `/.claude/worktrees/` are other worktrees and are intentionally ignored.

- [ ] **Step 5: Commit**

```bash
cd /Users/amarkelov/claude-hobby/externals/FastECU
git add -A
git commit -m "refactor: move external/ hardware sketches to 3rdparty/hardware/

Frees the root external/ path so Hedron's generation-time external symlink
does not collide. No build inputs affected; updates the one qmake DISTFILES
reference and adds a README.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Wire Hedron with the Bazel 9 patch and expose the compile-commands target

Adds the dependency, the patch, the target, the gitignore entry, and the lock bump as one unit — the patch's effect is only observable once the `refresh_compile_commands` macro is evaluated by the target, so they share a single test cycle (`bazel run //bazel/compile_commands:refresh`).

**Files:**
- Create: `bazel/patches/hedron_bazel9_py_binary.patch`
- Modify: `bazel/patches/BUILD.bazel`
- Modify: `MODULE.bazel` (append after the existing `register_toolchains(...windows_x86_msvc...)` line)
- Create: `bazel/compile_commands/BUILD.bazel`
- Modify: `.gitignore` (insert after line 59, `compile_commands.json`)
- Modify: `MODULE.bazel.lock` (regenerated by Bazel)

**Interfaces:**
- Consumes: the `3rdparty/hardware/` relocation from Task 1 (so no root `external/` collision when the target generates).
- Produces: repo `@hedron_compile_commands` (dev dep); runnable target `//bazel/compile_commands:refresh` that writes `compile_commands.json` at the workspace root. Consumers reference this exact label.

- [ ] **Step 1: Create the Bazel 9 patch file**

Create `bazel/patches/hedron_bazel9_py_binary.patch` with exactly this content:

```diff
diff --git a/BUILD b/BUILD
index e120f11..2693852 100644
--- a/BUILD
+++ b/BUILD
@@ -1,3 +1,4 @@
+load("@rules_cc//cc:defs.bzl", "cc_binary")
 load(":refresh_compile_commands.bzl", "refresh_compile_commands")

 # See README.md for interface.
diff --git a/MODULE.bazel b/MODULE.bazel
index 23e060f..3b76e9d 100644
--- a/MODULE.bazel
+++ b/MODULE.bazel
@@ -1,5 +1,8 @@
 module(name = "hedron_compile_commands")

+bazel_dep(name = "rules_cc", version = "0.2.22")
+bazel_dep(name = "rules_python", version = "2.2.0")
+
 p = use_extension("//:workspace_setup.bzl", "hedron_compile_commands_extension")
 pt = use_extension("//:workspace_setup_transitive.bzl", "hedron_compile_commands_extension")
 ptt = use_extension("//:workspace_setup_transitive_transitive.bzl", "hedron_compile_commands_extension")
diff --git a/refresh_compile_commands.bzl b/refresh_compile_commands.bzl
index 0210d42..f86e533 100644
--- a/refresh_compile_commands.bzl
+++ b/refresh_compile_commands.bzl
@@ -57,6 +57,7 @@ refresh_compile_commands(
 # Implementation

 load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
+load("@rules_python//python:defs.bzl", "py_binary")


 def refresh_compile_commands(
@@ -92,7 +93,7 @@ def refresh_compile_commands(
     _expand_template(name = script_name, labels_to_flags = targets, exclude_headers = exclude_headers, exclude_external_sources = exclude_external_sources, **kwargs)

     # Combine them so the wrapper calls the main script
-    native.py_binary(
+    py_binary(
         name = name,
         main = version_checker_script_name,
         srcs = [version_checker_script_name, script_name],
```

- [ ] **Step 2: Register the patch in `bazel/patches/BUILD.bazel`**

Replace the file's current single `exports_files(["rules_qt_windows_addons.patch"])` with:

```python
exports_files([
    "hedron_bazel9_py_binary.patch",
    "rules_qt_windows_addons.patch",
])
```

- [ ] **Step 3: Derive the archive integrity hash**

Run:

```bash
cd /Users/amarkelov/claude-hobby/externals/FastECU
curl -sL "https://github.com/hedronvision/bazel-compile-commands-extractor/archive/abb61a688167623088f8768cc9264798df6a9d10.tar.gz" \
  | openssl dgst -sha256 -binary | openssl base64
```

Expected: prints a base64 string. Prefix it with `sha256-` to form the integrity value. It should equal `sha256-Gwir/7++ifbb7mpbM3U3kugAT2o283wPchFb7IbmhyQ=`. If it differs (GitHub re-tarred the archive), use the freshly derived value in Step 4 instead — the run in Step 7 fails loudly on any mismatch.

- [ ] **Step 4: Append the dependency + override to `MODULE.bazel`**

At the end of `MODULE.bazel` (after the existing `register_toolchains("//bazel/toolchains/windows_x86_msvc:windows_x86_msvc_toolchain")` line), add:

```python

bazel_dep(name = "hedron_compile_commands", dev_dependency = True)

# Upstream HEAD still calls native.py_binary / native cc_binary, both removed in
# Bazel 9. Pin the commit + integrity via archive_override (it carries the patch
# hermetically) and patch hedron to load py_binary / cc_binary from
# rules_python / rules_cc. Bump the commit deliberately.
archive_override(
    module_name = "hedron_compile_commands",
    integrity = "sha256-Gwir/7++ifbb7mpbM3U3kugAT2o283wPchFb7IbmhyQ=",
    strip_prefix = "bazel-compile-commands-extractor-abb61a688167623088f8768cc9264798df6a9d10",
    urls = ["https://github.com/hedronvision/bazel-compile-commands-extractor/archive/abb61a688167623088f8768cc9264798df6a9d10.tar.gz"],
    patch_strip = 1,
    patches = ["//bazel/patches:hedron_bazel9_py_binary.patch"],
)
```

(If Step 3 produced a different hash, substitute it in the `integrity` field.)

- [ ] **Step 5: Add the compile-commands target**

Create `bazel/compile_commands/BUILD.bazel`:

```python
load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

# Shared compile_commands.json foundation. Consumed by the clang-tidy migration
# (//:clang_tidy_* targets) and the SonarCloud C/C++ analyzer -- keep this target
# and its label stable for those. Scoped to app + tests: Hedron's default @//...
# pulls in the Windows MSVC toolchain targets, which fail extraction on
# non-Windows hosts.
#   bazel run //bazel/compile_commands:refresh
# exclude_headers="all": Sonar treats every DB entry as a translation unit;
# including headers ballooned the DB to ~5000 entries and the scan to 2+ hours.
# exclude_external_sources: our code only.
refresh_compile_commands(
    name = "refresh",
    exclude_external_sources = True,
    exclude_headers = "all",
    targets = {
        "//:fastecu": "--config=release",
        "//tests/...": "--config=release",
    },
)
```

- [ ] **Step 6: Ignore Hedron's generation-time symlink**

In `.gitignore`, immediately after line 59 (`compile_commands.json`), insert:

```
# Hedron drops an `external` symlink to Bazel's external repos at the workspace
# root when generating compile_commands.json.
/external
```

- [ ] **Step 7: Verify the dependency resolves, the patch applies, and the target runs**

Run:

```bash
cd /Users/amarkelov/claude-hobby/externals/FastECU
bazel run //bazel/compile_commands:refresh -- --config=release
```

Expected: Bazel fetches `hedron_compile_commands`, applies `hedron_bazel9_py_binary.patch` (no "patch failed" / "native.py_binary" errors), builds the app + tests, the runner prints progress, and it writes `compile_commands.json`. Exit status 0.

- [ ] **Step 8: Verify the output database looks right**

Run:

```bash
cd /Users/amarkelov/claude-hobby/externals/FastECU
test -f compile_commands.json && echo "db written OK"
python3 -c "import json; db=json.load(open('compile_commands.json')); print(len(db), 'entries'); print('has fastecu source:', any(e['file'].endswith('.cpp') for e in db))"
```

Expected: `db written OK`, a nonzero entry count, and `has fastecu source: True`. (`compile_commands.json` and the `/external` symlink are gitignored and must NOT be committed.)

- [ ] **Step 9: Confirm the lockfile was updated**

Run:

```bash
cd /Users/amarkelov/claude-hobby/externals/FastECU
git status --short MODULE.bazel.lock
```

Expected: `MODULE.bazel.lock` shows as modified (the resolution added the hedron entry). If Bazel did not touch it, run `bazel mod deps >/dev/null` to force a lock refresh.

- [ ] **Step 10: Commit**

```bash
cd /Users/amarkelov/claude-hobby/externals/FastECU
git add MODULE.bazel MODULE.bazel.lock .gitignore \
  bazel/patches/hedron_bazel9_py_binary.patch bazel/patches/BUILD.bazel \
  bazel/compile_commands/BUILD.bazel
git commit -m "build: add hedron_compile_commands with Bazel 9 patch

Pin hedron to upstream HEAD via archive_override + integrity, patched to load
py_binary/cc_binary from rules_python/rules_cc (both removed from Bazel 9
builtins). Expose //bazel/compile_commands:refresh as the shared
compile_commands.json foundation for the clang-tidy and SonarCloud migrations.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- MODULE.bazel dep + archive_override + integrity + patch → Task 2, Steps 3–4. ✓
- `hedron_bazel9_py_binary.patch` + `exports_files` registration → Task 2, Steps 1–2. ✓
- Neutral `//bazel/compile_commands:refresh` target, app+tests, `exclude_*`, embedded `--config=release` → Task 2, Step 5. ✓
- No pip/pyyaml/python-toolchain/`.bazelrc` flag → honored (absent from plan); Global Constraints. ✓
- `.gitignore` `/external` → Task 2, Step 6. ✓
- `MODULE.bazel.lock` bump → Task 2, Step 9. ✓
- `external/` → `3rdparty/hardware/` + `FastECU.pro` update + README → Task 1. ✓
- No CI changes → honored; Global Constraints. ✓
- Verification (`bazel run` / `bazel build`) → Task 2, Steps 7–8. ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"/"similar to". Patch and BUILD contents are given in full. ✓

**Type/label consistency:** Target label `//bazel/compile_commands:refresh` and `name = "refresh"` are consistent across the spec, the BUILD file (Step 5), and the run commands (Steps 7, 9). Pinned commit `abb61a68...` and integrity `sha256-Gwir/...` are identical in Steps 3 and 4. ✓
