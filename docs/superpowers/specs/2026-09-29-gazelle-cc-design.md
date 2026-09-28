# gazelle_cc for BUILD file management — design

Status: draft for review. Date: 2026-09-29.

## Goal

Introduce [`gazelle_cc`](https://github.com/EngFlow/gazelle_cc) so `BUILD.bazel`
files stay in sync with the C++ sources, with:

- a **separate CI job** that fails when checked-in BUILD files differ from what
  gazelle would generate, and
- a **local regeneration gate** that runs before a PR can be created.

## Decisions taken during brainstorming

| Question | Decision |
|---|---|
| How much does gazelle own? | **Pilot first.** One area under gazelle, the rest excluded. |
| Pilot area | `src/algorithms/`, excluding the `qt_compat` subpackages. |
| What is generated in the pilot | **New and existing** `cc_library` and test targets (tests via `map_kind` to `fastecu_portable_gtest`). Not deps-only. |
| Local gate | `prek` **pre-push** hook. |
| Hook implementation | Python script (`scripts/gazelle_check.py`), standard library only. |
| CI shape | Separate `gazelle` job in `pr.yml`, ubuntu only, running the same hook via prek. |

## Non-goals

- Anything outside `src/algorithms/` (widening is separate work, see below).
- `PORTABLE_PACKAGES` in `bazel/portable_targets.bzl` and the `qt_layer`
  package group. They stay hand-maintained; gazelle never edits them.
- Writing `visibility` attributes. Visibility comes from each package's
  hand-written `package(default_visibility = ...)` using `//bazel/layers`
  groups.
- Generating BUILD files for **new packages**. A new package needs a
  hand-written `package(default_visibility = ...)` line first.

## Design

### Components

1. **Dependencies.** `gazelle` and `gazelle_cc` as `bazel_dep(..., dev_dependency = True)`
   in `MODULE.bazel`, pinned to explicit versions.
2. **`//:gazelle` target** in the root `BUILD.bazel`, built with the cc language
   extension. Root-level directives:
   - `# gazelle:exclude` for every top-level tree except `src/algorithms`;
   - `# gazelle:exclude` for the `qt_compat` subpackages;
   - `# gazelle:map_kind cc_test fastecu_portable_gtest //bazel:gtest_targets.bzl`
     (exact directive spelling to be confirmed against the gazelle_cc version
     pinned; see Open questions).
3. **`scripts/gazelle_check.py`** (standard library only):
   1. resolve the repo root with `git rev-parse --show-toplevel`;
   2. run `bazel run //:gazelle`;
   3. run `git add --intent-to-add` on `*.bazel` files under the pilot scope, so
      files gazelle *created* appear in `git diff`;
   4. run `git diff --exit-code` on the same paths; on a difference print the
      diff and exit 1, telling the contributor to review, `git add`, and re-push.
4. **prek hook** in `.pre-commit-config.yaml`: `repo: local`, `language: python`,
   `entry: scripts/gazelle_check.py`, `pass_filenames: false`,
   `stages: [pre-push]`, with a `files:` filter for C++ sources, `BUILD.bazel`,
   `MODULE.bazel` and the script itself.
5. **CI job** `gazelle` in `.github/workflows/pr.yml`: ubuntu only, no `needs:`
   (runs in parallel with `pre-commit`), same `setup-bazel` step as the `bazel`
   job, then
   `prek run --hook-stage pre-push --all-files gazelle --show-diff-on-failure`.
6. **Documentation.** A short ADR recording the decision, and a note in the
   [coding style guide](../../coding-style.md) or contributing docs on how to
   regenerate locally.

### Why a script and not prek's own modification detection

prek fails a hook that *modifies tracked files*, but it does **not** notice
files a hook creates. Verified with prek 0.4.0: a hook that wrote an untracked
file passed with exit 0 and left it as `??`; a hook that appended to a tracked
file failed with "files were modified by this hook". Because this design lets
gazelle create files, the script marks new BUILD files intent-to-add so they
show up in the diff and are gated.

### Data flow

```
edit sources/BUILD -> git push
  -> prek pre-push -> gazelle_check.py -> bazel run //:gazelle
       -> git add -N '*.bazel' -> git diff --exit-code
          clean: push proceeds
          drift: diff printed, exit 1, fixed files stay in the working tree
PR opened -> CI `gazelle` job runs the same hook -> pass/fail
```

## Pilot acceptance criteria

The pilot is done when all of the following hold, verified in this order:

1. **Idempotence on the current tree.** With gazelle configured, running it on
   `src/algorithms` yields no diff, or a diff that has been reviewed and each
   change explained. Unexplained churn means a directive is missing; fix the
   directive rather than accept the churn.
2. **Macro mapping.** Generated tests use `fastecu_portable_gtest`, and re-running
   on existing ones is a no-op.
3. **Creation.** In a scratch package, adding `foo.cpp`, `foo.h` and
   `foo_test.cpp` makes gazelle create the library and test, and
   `gazelle_check.py` then exits 1. Throwaway verification, not committed.
4. **Include resolution.** `#include "src/algorithms/..."` resolves to the
   correct labels with the repo's include layout.
5. **Formatting.** Gazelle output is buildifier-clean (no fight between the two
   hooks).
6. **Hook behavior.** The Python hook runs under prek 0.4.0 at the pre-push
   stage, locally and in CI.
7. **Script tests.** The script's decision logic (scope filtering, diff and
   exit-code handling) has co-located unit tests, following the layout of
   `scripts/clang_tidy_runner_test.py`.

## Risks and mitigations

- **Bazel is slow at push time.** Hook is `pre-push` only and file-filtered.
  `--no-verify` is a conscious bypass; CI remains the hard gate.
- **Symbolic-macro attribute merging.** `fastecu_portable_gtest` was recently
  converted to symbolic attributes; gazelle's `map_kind` merge behavior on it is
  the largest technical unknown. Criterion 2 exists to resolve it.
- **Gazelle vs. buildifier formatting.** Criterion 5.
- **Windows contributors.** Python script, no bash dependency.

## Widening later

Each widening removes one `exclude` line and is its own PR whose diff is the
acceptance test for that area, run alongside the existing guardrails
(`//:portable_closure`, ratchet lists, visibility groups). Order and cadence are
not decided here.

## Open questions (resolve during planning, not by guessing)

1. Exact gazelle_cc directive names for: disabling target deletion, mapping
   `cc_test` through `map_kind`, per-file versus per-package test grouping, and
   include-path resolution. Confirm against the pinned version's source and docs.
2. Which gazelle and gazelle_cc versions are compatible with Bazel 9.1.1 and
   `rules_cc` 0.2.22.
3. Whether `language: python` in a `local` prek hook works without extra
   configuration on prek 0.4.0 (criterion 6).
