# Python 3.14 Upgrade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Python 3.14 the sole Bazel-managed runtime and align Ruff's language target with it.

**Architecture:** `MODULE.bazel` remains the single source of truth for the hermetic CPython toolchain and pip resolution. Ruff remains a pre-commit tool whose target version mirrors Bazel; Bazel regenerates `MODULE.bazel.lock` from the updated module declarations.

**Tech Stack:** Bazel 9.1.1, rules_python 2.2.0, CPython 3.14, PyYAML 6.0.3, Ruff 0.15.14, pre-commit

## Global Constraints

- Start from `master` with design commit `33bfb30` based on upstream commit `0278bdc5192aa64b20d50e98d172acfc1f06ab39`.
- Python 3.14 is the sole supported Python runtime; do not retain a Python 3.11 compatibility matrix.
- Keep `rules_python` 2.2.0, PyYAML 6.0.3, and Ruff 0.15.14 pinned.
- Keep the existing minor-version toolchain selection model.
- Do not manually edit `MODULE.bazel.lock`.
- Do not add a system-Python or source-build fallback.
- Limit script changes to fixes directly required by Python 3.14 or Ruff `py314` checks.
- Preserve the existing Linux, macOS, and Windows CI platform coverage.
- Do not touch the user's untracked `.qtcreator/` or `reports/` directories.

---

## File Map

- `MODULE.bazel`: declares the default Python version, hermetic toolchain repository, and pip resolution version.
- `MODULE.bazel.lock`: Bazel-generated module extension and dependency resolution state.
- `requirements.txt`: existing hash-pinned PyYAML 6.0.3 input; expected to remain unchanged.
- `ruff.toml`: declares the Python language version used by Ruff's lint and format checks.
- `scripts/*.py`: existing Python tools checked by Bazel and Ruff; modify only if 3.14 validation exposes a concrete incompatibility.

### Task 1: Upgrade the Hermetic Bazel Runtime

**Files:**
- Modify: `MODULE.bazel:8-17`
- Regenerate: `MODULE.bazel.lock`
- Verify unchanged: `requirements.txt`

**Interfaces:**
- Consumes: `rules_python` 2.2.0's `python` and `pip` module extensions and the PyYAML 6.0.3 hashes in `requirements.txt`.
- Produces: the `python_3_14` repository, Python 3.14 as the default Bazel runtime, and `@python_deps//pyyaml` resolved for Python 3.14.

- [ ] **Step 1: Record the current runtime as the failing baseline**

Run:

```bash
bazel run @rules_python//python/bin:python -- --version
```

Expected: the command succeeds but reports `Python 3.11.x`, demonstrating that the requested Python 3.14 state is not yet present.

- [ ] **Step 2: Change every Bazel Python declaration to 3.14**

Replace the Python extension block at the top of `MODULE.bazel` with exactly:

```starlark
python = use_extension("@rules_python//python/extensions:python.bzl", "python")
python.defaults(python_version = "3.14")
python.toolchain(python_version = "3.14")
use_repo(python, "python_3_14")

pip = use_extension("@rules_python//python/extensions:pip.bzl", "pip")
pip.parse(
    hub_name = "python_deps",
    python_version = "3.14",
    requirements_lock = "//:requirements.txt",
)
use_repo(pip, "python_deps")
```

- [ ] **Step 3: Regenerate Bazel's module lock state**

Run:

```bash
bazel mod deps
```

Expected: exit code 0 and an automatically updated `MODULE.bazel.lock`. Do not modify the lockfile by hand if resolution fails; investigate the toolchain or wheel error instead.

- [ ] **Step 4: Verify the hermetic interpreter is now Python 3.14**

Run:

```bash
bazel run @rules_python//python/bin:python -- --version
```

Expected: exit code 0 with `Python 3.14.x` in the output.

- [ ] **Step 5: Verify all Python tests and the PyYAML dependency**

Run:

```bash
bazel test //:clang_tidy_runner_test //:bazel_openssl_wiring //:openpty_includes //:windows_preprocessor_guards
```

Expected: all four targets pass. `//:clang_tidy_runner_test` imports `yaml`, so its success verifies `@python_deps//pyyaml` under the Python 3.14 toolchain.

- [ ] **Step 6: Confirm version consistency and dependency stability**

Run:

```bash
rg -n '3\.11|python_3_11' MODULE.bazel
git diff --exit-code -- requirements.txt
git diff --check
```

Expected: `rg` prints no matches, `requirements.txt` has no changes, and `git diff --check` exits 0.

- [ ] **Step 7: Review and commit the runtime upgrade**

Run:

```bash
git diff -- MODULE.bazel MODULE.bazel.lock requirements.txt
git add MODULE.bazel MODULE.bazel.lock
git commit -m "build: upgrade Python toolchain to 3.14"
```

Expected: the diff contains only the four coordinated 3.14 declarations and Bazel-generated lock state; the commit succeeds without staging unrelated files.

### Task 2: Align Ruff and Complete Repository Verification

**Files:**
- Modify: `ruff.toml:1`
- Conditionally modify: `scripts/*.py` only when a named Ruff or Python 3.14 failure requires it

**Interfaces:**
- Consumes: the Python 3.14 runtime produced by Task 1 and the existing Ruff 0.15.14 pre-commit hooks.
- Produces: Ruff configured for `py314` and a repository verified against the same Python language level used by Bazel.

- [ ] **Step 1: Record Ruff's stale target as the failing baseline**

Run:

```bash
rg -n '^target-version = "py314"$' ruff.toml
```

Expected: exit code 1 with no output because Ruff still targets `py311`.

- [ ] **Step 2: Change Ruff's target version**

Change the first line of `ruff.toml` to exactly:

```toml
target-version = "py314"
```

Keep the current line length, lint selection, and formatting settings unchanged.

- [ ] **Step 3: Run the configured Ruff hooks**

Run:

```bash
pre-commit run ruff-check --all-files
pre-commit run ruff-format --all-files
```

Expected: both hooks report `Passed`. The configured `ruff-check` hook may apply safe fixes because it carries `--fix`; review any such diff, retain only changes required by the `py314` target, and rerun both commands until they pass.

- [ ] **Step 4: Run the complete pre-commit suite**

Run:

```bash
pre-commit run --all-files
```

Expected: clang-format, buildifier, buildifier-lint, ruff-check, and ruff-format all pass. If pre-commit modifies a file, review that diff and rerun until the suite exits 0.

- [ ] **Step 5: Run the maintained Bazel test suite**

Run:

```bash
bazel test --config=release //tests/... //:bazel_openssl_wiring //:clang_tidy_runner_test //:openpty_includes //:windows_preprocessor_guards
```

Expected: every compatible target passes. Platform-incompatible targets may be reported as skipped by Bazel; test failures are not acceptable.

- [ ] **Step 6: Prove the final configuration is consistently 3.14**

Run:

```bash
rg -n '3\.11|python_3_11|py311' MODULE.bazel ruff.toml .github scripts BUILD.bazel README.md
bazel run @rules_python//python/bin:python -- --version
git diff --check
git status --short
```

Expected: the search prints no stale project configuration matches; Bazel reports `Python 3.14.x`; `git diff --check` exits 0; status lists only intentional tracked changes plus the user's pre-existing untracked `.qtcreator/` and `reports/` directories.

- [ ] **Step 7: Review and commit Ruff alignment**

Run:

```bash
git diff -- ruff.toml scripts
git add ruff.toml
```

If and only if Step 3 required narrow script fixes, stage their exact paths with `git add <path>`, then run:

```bash
git diff --cached --check
git commit -m "build: target Python 3.14 in Ruff"
```

Expected: the staged diff contains `target-version = "py314"` and only directly required script fixes; the commit succeeds.

- [ ] **Step 8: Perform the final clean-state review**

Run:

```bash
git log -3 --oneline
git status --short
git diff HEAD~2..HEAD -- MODULE.bazel MODULE.bazel.lock requirements.txt ruff.toml scripts
```

Expected: the two implementation commits follow design commit `33bfb30`; `requirements.txt` remains unchanged; no stale 3.11 declarations or unrelated tracked edits appear. The only allowable untracked entries are the user's pre-existing `.qtcreator/` and `reports/` directories.

Cross-platform completion requires the existing Linux, macOS, and Windows CI jobs to pass after these commits are proposed upstream.
