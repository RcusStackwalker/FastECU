# Bazel Warning Cleanup and Bazel 9 Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the four Bazel-emitted CI warnings and the one confirmed Bazel 9 blocker in FastECU, then bump `.bazelversion` to the latest stable release (9.1.1).

**Architecture:** Four independent, sequential changes to Bazel plumbing files (no C++ source or Qt code touched): (1) add missing `load()` statements so a bzlmod-generated `BUILD.bazel` stops relying on Bazel-core autoload of `cc_import`/`cc_library`, (2) opt the whole repo into explicit `__init__.py` handling and right-size four fast `py_test` targets, (3) fix a CI step so the clang-tidy report test can actually finish instead of silently timing out at 300s every run, (4) flip `.bazelversion` to 9.1.1 once everything is proven to work against it locally via bazelisk.

**Tech Stack:** Bazel 8.4.2 → 9.1.1 (via `bazelisk`), Bzlmod (`MODULE.bazel`), `rules_cc`, `rules_python`, `rules_qt`, GitHub Actions.

## Global Constraints

- Scope is Bazel-emitted warnings and the Bazel 9 migration only. Do not touch MSVC/clang compiler or linker warnings (C4700, C4129, C4333, LNK4044, `-Wunused-variable`) — out of scope per the design spec.
- Do not attempt to patch `rules_qt`'s `exports_files(["qml", "plugins", "lib", "share"])` directory-input warning — documented as an accepted upstream warning, no local fix.
- Do not investigate or speed up what `clang_tidy_report` actually does — only its timeout ceiling changes, so it can complete rather than always hitting `TIMEOUT in 300.0s`.
- No `bazel_dep` version bumps are needed or in scope — `rules_cc` 0.2.22, `rules_python` 2.2.0, `rules_qt` 0.0.6, `platforms` 1.0.0 already work under Bazel 9.1.1 (verified locally).
- All local verification commands use `bazelisk` via `USE_BAZEL_VERSION=<version>` so the working tree's `.bazelversion` (still 8.4.2 until Task 4) is not disturbed until the final task.
- Work happens in the `bazel9-migration` worktree, branch `worktree-bazel9-migration`, based on `markelov/fix-bazel-windows`. All commands below assume cwd = the worktree root (`/Users/amarkelov/claude-hobby/externals/FastECU/.claude/worktrees/bazel9-migration`).

---

### Task 1: Fix the Bazel 9 blocker in `bazel/openssl_ext.bzl`

**Files:**
- Modify: `bazel/openssl_ext.bzl:29-50` (the `_BUILD_IMPORTED` and `_BUILD_AMBIENT` string templates)

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: a `BUILD.bazel` file generated at `@openssl//:BUILD.bazel` (via the `openssl` repository rule) that Bazel 9 can load without error. Later tasks don't depend on this directly, but Task 4's full-suite verification requires it (every target depending on `//:fastecu_core_common` transitively depends on `@openssl//:openssl`).

- [ ] **Step 1: Reproduce the failure against real Bazel 9.1.1**

Run:
```bash
mkdir -p /tmp/bazel9-task1-output
USE_BAZEL_VERSION=9.1.1 bazel --output_base=/tmp/bazel9-task1-output build @openssl//:openssl
```
Expected: build fails with an error containing `This rule has been removed from Bazel. Please add a 'load()' statement for it.`, referencing `cc_import(` in the generated `+openssl+openssl/BUILD.bazel`.

- [ ] **Step 2: Add the missing `load()` statements**

In `bazel/openssl_ext.bzl`, change:
```python
_BUILD_IMPORTED = """\
cc_import(
    name = "crypto_import",
{import_attrs}
)

cc_library(
    name = "openssl",
    hdrs = glob(["include/openssl/**/*.h"], allow_empty = True),
    includes = ["include"],
    deps = [":crypto_import"],
    visibility = ["//visibility:public"],
)
"""

_BUILD_AMBIENT = """\
cc_library(
    name = "openssl",
    linkopts = ["-lcrypto"],
    visibility = ["//visibility:public"],
)
"""
```
to:
```python
_BUILD_IMPORTED = """\
load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_import(
    name = "crypto_import",
{import_attrs}
)

cc_library(
    name = "openssl",
    hdrs = glob(["include/openssl/**/*.h"], allow_empty = True),
    includes = ["include"],
    deps = [":crypto_import"],
    visibility = ["//visibility:public"],
)
"""

_BUILD_AMBIENT = """\
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "openssl",
    linkopts = ["-lcrypto"],
    visibility = ["//visibility:public"],
)
"""
```

- [ ] **Step 3: Re-run the same build and confirm it now succeeds**

Run:
```bash
USE_BAZEL_VERSION=9.1.1 bazel --output_base=/tmp/bazel9-task1-output build @openssl//:openssl
```
Expected: `INFO: Build completed successfully` (no `cc_import`/`cc_library` removed-rule error).

- [ ] **Step 4: Verify the fix resolves the cascading failures on the full target set**

Run:
```bash
USE_BAZEL_VERSION=9.1.1 bazel --output_base=/tmp/bazel9-task1-output build -k --config=release //:fastecu //tests/...
```
Expected: `INFO: Build succeeded for X of X top-level targets` with **no** `no such target '@@+openssl+openssl//:openssl'` error (some individual targets may still fail for unrelated pre-existing reasons on macOS sandboxing, but the openssl-autoload error must not appear — grep the output for `removed from Bazel` and `+openssl+openssl` to confirm absence).

- [ ] **Step 5: Commit**

```bash
git add bazel/openssl_ext.bzl
git commit -m "$(cat <<'EOF'
fix: add explicit cc_import/cc_library loads to openssl_ext BUILD templates

Bazel 9 defaults --incompatible_autoload_externally to empty, so
cc_import/cc_library are no longer autoloaded from Bazel core. The
generated @openssl BUILD.bazel needs explicit load() statements or every
target depending on //:fastecu_core_common fails to analyze.
EOF
)"
```

---

### Task 2: Silence the `__init__.py` and test-size warnings

**Files:**
- Modify: `.bazelrc:1-2` (add one line near the existing `common` flags)
- Modify: `BUILD.bazel:153-192` (the four fast `py_test` targets: `qmake_bazel_sync`, `bazel_openssl_wiring`, `openpty_includes`, `windows_preprocessor_guards`)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: nothing consumed by later tasks; independently verifiable.

- [ ] **Step 1: Reproduce both warnings against the current Bazel 8.4.2**

Run:
```bash
bazel test --test_verbose_timeout_warnings //:qmake_bazel_sync //:bazel_openssl_wiring //:openpty_includes //:windows_preprocessor_guards 2>&1 | grep -E "implicit __init__|outside of range"
```
Expected: at least one `WARNING: ... is using implicit __init__.py creation` line and four `WARNING: //:X: Test execution time ... outside of range for MODERATE tests` lines (one per target).

- [ ] **Step 2: Add the repo-wide explicit-init-py flag**

In `.bazelrc`, change the top of the file from:
```
common --enable_bzlmod
common --enable_platform_specific_config
```
to:
```
common --enable_bzlmod
common --enable_platform_specific_config
build --incompatible_default_to_explicit_init_py
```

- [ ] **Step 3: Add `size = "small"` to the four fast py_test targets**

In `BUILD.bazel`, update each of the four targets. Before:
```python
py_test(
    name = "qmake_bazel_sync",
    srcs = ["scripts/check-bazel-qmake-sync.py"],
    data = [
        "FastECU.pro",
        "protocol/protocol.pri",
        "//bazel:fastecu_sources.bzl",
        "//tests:pro_files",
        "//tests/force_asserts:pro_files",
    ] + depset(APP_COMMON_HDRS + APP_MOC_HDRS + APP_UNIX_HDRS + APP_WIN_HDRS).to_list(),
    main = "scripts/check-bazel-qmake-sync.py",
)
```
After:
```python
py_test(
    name = "qmake_bazel_sync",
    srcs = ["scripts/check-bazel-qmake-sync.py"],
    data = [
        "FastECU.pro",
        "protocol/protocol.pri",
        "//bazel:fastecu_sources.bzl",
        "//tests:pro_files",
        "//tests/force_asserts:pro_files",
    ] + depset(APP_COMMON_HDRS + APP_MOC_HDRS + APP_UNIX_HDRS + APP_WIN_HDRS).to_list(),
    main = "scripts/check-bazel-qmake-sync.py",
    size = "small",
)
```

Before:
```python
py_test(
    name = "bazel_openssl_wiring",
    srcs = ["scripts/check-bazel-openssl.py"],
    data = [
        ".github/workflows/pr.yml",
        "BUILD.bazel",
        "MODULE.bazel",
    ],
    main = "scripts/check-bazel-openssl.py",
)
```
After:
```python
py_test(
    name = "bazel_openssl_wiring",
    srcs = ["scripts/check-bazel-openssl.py"],
    data = [
        ".github/workflows/pr.yml",
        "BUILD.bazel",
        "MODULE.bazel",
    ],
    main = "scripts/check-bazel-openssl.py",
    size = "small",
)
```

Before:
```python
py_test(
    name = "openpty_includes",
    srcs = ["scripts/check-openpty-includes.py"],
    data = ["//tests:cpp_sources"],
    main = "scripts/check-openpty-includes.py",
)
```
After:
```python
py_test(
    name = "openpty_includes",
    srcs = ["scripts/check-openpty-includes.py"],
    data = ["//tests:cpp_sources"],
    main = "scripts/check-openpty-includes.py",
    size = "small",
)
```

Before:
```python
py_test(
    name = "windows_preprocessor_guards",
    srcs = ["scripts/check-windows-preprocessor-guards.py"],
    data = [
        "file_actions.h",
        "serial_port/serial_port_actions_direct.h",
    ],
    main = "scripts/check-windows-preprocessor-guards.py",
)
```
After:
```python
py_test(
    name = "windows_preprocessor_guards",
    srcs = ["scripts/check-windows-preprocessor-guards.py"],
    data = [
        "file_actions.h",
        "serial_port/serial_port_actions_direct.h",
    ],
    main = "scripts/check-windows-preprocessor-guards.py",
    size = "small",
)
```

- [ ] **Step 4: Re-run and confirm both warnings are gone and tests still pass**

Run:
```bash
bazel test --test_verbose_timeout_warnings //:qmake_bazel_sync //:bazel_openssl_wiring //:openpty_includes //:windows_preprocessor_guards 2>&1 | tee /tmp/task2-verify.log
grep -E "implicit __init__|outside of range" /tmp/task2-verify.log
echo "exit: $?"
grep -c "PASSED" /tmp/task2-verify.log
```
Expected: the `grep -E` for warnings finds **no matches** (exit code 1), and `PASSED` appears 4 times (one per target).

- [ ] **Step 5: Commit**

```bash
git add .bazelrc BUILD.bazel
git commit -m "$(cat <<'EOF'
fix: silence implicit __init__.py and oversized-test-size Bazel warnings

--incompatible_default_to_explicit_init_py stops Bazel from synthesizing
__init__.py files for py_test targets that don't need them (none of
these scripts do intra-repo package imports). size = "small" matches
these targets' actual ~1.5s runtime instead of the "medium" default,
which was flagged as oversized by --test_verbose_timeout_warnings.
EOF
)"
```

---

### Task 3: Fix the clang-tidy report step (cache-discard warning + perpetual timeout)

**Files:**
- Modify: `BUILD.bazel:194-207` (`clang_tidy_report` target)
- Modify: `.github/workflows/pr.yml:297-300` (the "clang-tidy report" job step)

**Interfaces:**
- Consumes: nothing from Tasks 1–2.
- Produces: nothing consumed by later tasks; independently verifiable.

- [ ] **Step 1: Confirm the current timeout attribute and CI step (baseline)**

Run:
```bash
bazel query --output=build //:clang_tidy_report | grep -E "timeout|size"
```
Expected: no `timeout` or `size` line printed (both currently default — `size` defaults to `"medium"`, giving a 300s MODERATE timeout, which is what CI's `TIMEOUT in 300.0s` reflects).

- [ ] **Step 2: Add an explicit long timeout to `clang_tidy_report`**

In `BUILD.bazel`, change:
```python
py_test(
    name = "clang_tidy_report",
    srcs = ["scripts/clang_tidy_report.py"],
    args = ["bazel/fastecu_sources.bzl"],
    data = [
        ".clang-tidy",
        "//bazel:fastecu_sources.bzl",
    ],
    main = "scripts/clang_tidy_report.py",
    tags = [
        "clang-tidy",
        "manual",
    ],
)
```
to:
```python
py_test(
    name = "clang_tidy_report",
    srcs = ["scripts/clang_tidy_report.py"],
    args = ["bazel/fastecu_sources.bzl"],
    data = [
        ".clang-tidy",
        "//bazel:fastecu_sources.bzl",
    ],
    main = "scripts/clang_tidy_report.py",
    tags = [
        "clang-tidy",
        "manual",
    ],
    timeout = "eternal",
)
```

- [ ] **Step 3: Verify the attribute took effect**

Run:
```bash
bazel query --output=build //:clang_tidy_report | grep timeout
```
Expected: `timeout = "eternal"`.

- [ ] **Step 4: Fix the CI step's compilation-mode mismatch**

In `.github/workflows/pr.yml`, change:
```yaml
      - name: clang-tidy report
        continue-on-error: true
        run: |
          bazel test //:clang_tidy_report --test_output=all
```
to:
```yaml
      - name: clang-tidy report
        continue-on-error: true
        run: |
          bazel test --config=release //:clang_tidy_report --test_output=all
```

- [ ] **Step 5: Sanity-check the target still runs under `--config=release` locally**

Run:
```bash
timeout 60 bazel test --config=release //:clang_tidy_report --test_output=all 2>&1 | grep -E "WARNING: Build options|compilation_mode"
```
Expected: **no** `Build options --compilation_mode and --copt have changed, discarding analysis cache` line (the command may still be running clang-tidy itself past the 60s local timeout wrapper — that's fine and expected; this step only checks the analysis-cache warning is gone, not that the full clang-tidy pass completes in 60s).

- [ ] **Step 6: Commit**

```bash
git add BUILD.bazel .github/workflows/pr.yml
git commit -m "$(cat <<'EOF'
fix: let clang-tidy report actually finish instead of always timing out

clang_tidy_report defaulted to a 300s MODERATE timeout but a full,
non-incremental clang-tidy pass over the whole source manifest takes
longer, so CI has always reported TIMEOUT in 300.0s for this step
(masked by continue-on-error). timeout = "eternal" gives it room to
finish. Separately, the CI step ran without --config=release while the
preceding build/test steps in the same job used it, discarding the
whole analysis cache between steps; aligning the flag avoids that.
EOF
)"
```

---

### Task 4: Bump `.bazelversion` to the latest release and do full local verification

**Files:**
- Modify: `.bazelversion`

**Interfaces:**
- Consumes: the fixes from Tasks 1–3 (this task's verification only passes because those are already in place).
- Produces: nothing consumed by later tasks — this is the final task.

- [ ] **Step 1: Bump the pinned Bazel version**

Change `.bazelversion` from:
```
8.4.2
```
to:
```
9.1.1
```

- [ ] **Step 2: Full build under the newly-pinned version**

Run:
```bash
bazel build -k --config=release //:fastecu //tests/...
```
Expected: `bazel version` (implicitly invoked by bazelisk reading `.bazelversion`) resolves to `9.1.1`, and the build has no `cc_import`/`cc_library` "removed from Bazel" errors and no `no such target '@@+openssl+openssl...'` errors. (Pre-existing, unrelated target failures if any are not blocking for this task — only confirm the openssl/autoload class of error is absent.)

- [ ] **Step 3: Full test run under the newly-pinned version**

Run:
```bash
bazel test --config=release //tests/... //:qmake_bazel_sync //:bazel_openssl_wiring //:openpty_includes //:windows_preprocessor_guards 2>&1 | tee /tmp/task4-verify.log
grep -E "implicit __init__|outside of range for MODERATE" /tmp/task4-verify.log
echo "exit: $?"
```
Expected: `grep` finds no matches (exit code 1); the test summary shows the four `py_test` targets `PASSED`.

- [ ] **Step 4: Confirm the Bazel version bazelisk actually used**

Run:
```bash
bazel version | grep "Build label"
```
Expected: `Build label: 9.1.1`.

- [ ] **Step 5: Commit**

```bash
git add .bazelversion
git commit -m "$(cat <<'EOF'
chore: bump .bazelversion to 9.1.1

Bazel 9.1.1 is the latest stable release. Verified locally that the
full build and test suite pass under it after the Bazel 9 autoload fix
(openssl_ext.bzl) and the warning cleanups landed in prior commits.
EOF
)"
```

---

## Self-Review

**Spec coverage:**
- Warning 1 (`implicit __init__.py`) → Task 2. ✓
- Warning 2 (analysis-cache discard from mismatched `--config=release`) → Task 3. ✓
- Warning 3 (rules_qt directory input) → explicitly left as-is per Global Constraints; no task, matching the spec's "leave as-is" decision. ✓
- Warning 4 (oversized test size) → Task 2. ✓
- Bonus finding (`clang_tidy_report` perpetual timeout) → Task 3. ✓
- Bazel 9 obstacle (`openssl_ext.bzl` missing loads) → Task 1. ✓
- `.bazelversion` bump → Task 4. ✓
- Out-of-scope items (MSVC/clang warnings, rules_qt patch, clang-tidy performance) → excluded from all tasks, called out in Global Constraints. ✓

**Placeholder scan:** no TBD/TODO, no "add appropriate handling" phrasing, every step shows literal before/after code or exact commands with expected output.

**Type/name consistency:** target names (`qmake_bazel_sync`, `bazel_openssl_wiring`, `openpty_includes`, `windows_preprocessor_guards`, `clang_tidy_report`) and file paths are consistent across Tasks 2–4 and match the current `BUILD.bazel` contents verified before writing this plan.
