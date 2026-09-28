# gazelle_cc for BUILD file management — design

Status: revised draft for review (rev 2, after a feasibility spike). Date: 2026-09-29.

## Goal

Introduce [`gazelle_cc`](https://github.com/EngFlow/gazelle_cc) so `BUILD.bazel`
files stay in sync with the C++ sources, with:

- a **separate CI job** that fails when checked-in BUILD files differ from what
  gazelle would generate, and
- a **local regeneration gate** that runs before a PR can be created.

## Decisions taken during brainstorming

| Question | Decision |
|---|---|
| How much does gazelle own? | **Pilot first.** One area under gazelle, the rest untouched. |
| Pilot area | `src/algorithms/`, excluding the `qt_compat` subpackages. |
| What is generated in the pilot | **New and existing** `cc_library` and test targets. Tests use `map_kind` to `fastecu_portable_gtest`. |
| Grouping mode | **`# gazelle:cc_group unit`** (see spike findings). Directory mode was rejected. |
| Local gate | `prek` **pre-push** hook. |
| Hook implementation | Python script (`scripts/gazelle_check.py`), standard library only. |
| CI shape | Separate `gazelle` job in `pr.yml`, ubuntu only, running the same hook via prek. |

## Spike findings (verified 2026-09-29)

Run on a throwaway worktree with `gazelle` 0.54.0 and `gazelle_cc` 0.6.0 on
Bazel 9.1.1 (both build; `gazelle_cc` 0.6.0 itself asks for `gazelle` >= 0.50.0
and `rules_go` 0.59.0). Gazelle was run in diff mode over `src/algorithms`.

- **`cc_group directory` does not fit this repo.** It adds a second, merged
  `cc_test` per directory next to the existing per-file test targets, creates a
  `testing` library overlapping the deliberately split `byte_matchers` /
  `byte_test_utils`, and pulls `uds_service_ids.h` into `uds` although
  `uds_service_ids` is its own target.
- **`cc_group unit` fits.** No library was split or renamed (`checksum`, `uds`,
  `protocol`, ... kept). `map_kind cc_test fastecu_portable_gtest` matched the
  existing per-file test targets. The diff over 11 BUILD files consisted of
  dependency changes only, apart from `qt_compat` (excluded from the pilot):
  - `@googletest//:gtest` added to 23 test targets. This is redundant with the
    `@googletest//:gtest_main` the macro already supplies (`gtest_main` depends
    on `gtest`), but the labels differ so it is not a duplicate. **Accepted
    as-is:** each test names what it includes, and suppressing it with
    `gazelle:resolve` would also give libraries such as `byte_matchers`
    `gtest_main`, which links a `main()` into a library;
  - `//src/algorithms/protocol/testing:byte_matchers` added to 7 test targets;
  - some `deps` moved to `implementation_deps` (headers used only in `.cpp`);
  - `//pkg` labels normalised to `:pkg` for same-package references;
  - four hand-written `//src/algorithms/protocol` deps and two
    `//src/algorithms/protocol/uds` deps removed or rewritten.

The spike diff is evidence for the plan's first task, not a committed artifact.

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

1. **Dependencies.** `bazel_dep(name = "gazelle", version = "0.54.0", dev_dependency = True)`
   and `bazel_dep(name = "gazelle_cc", version = "0.6.0", dev_dependency = True)`
   in `MODULE.bazel`.
2. **`//:gazelle` target** in the root `BUILD.bazel`: a `gazelle_binary` with
   `languages = ["@gazelle_cc//language/cc"]` and a `gazelle` rule whose
   `args` list the managed paths (`src/algorithms`). **Scope is the `args`
   list**, so widening later means adding a path. Root directives:
   - `# gazelle:cc_group unit`
   - `# gazelle:map_kind cc_test fastecu_portable_gtest //bazel:gtest_targets.bzl`
   - `# gazelle:exclude src/algorithms/protocol/qt_compat`
3. **`fastecu_portable_gtest` deduplicates its dependencies.** The macro adds
   `byte_matchers` and `gtest_main` implicitly; gazelle now also lists
   `byte_matchers` explicitly, and Bazel rejects a label that appears twice in
   `deps`. The macro drops duplicates from the combined list.
4. **`scripts/gazelle_check.py`** (standard library only):
   1. resolve the repo root with `git rev-parse --show-toplevel`;
   2. run `bazel run //:gazelle`;
   3. run `git add --intent-to-add` on `*.bazel` files under the managed
      paths, so files gazelle *created* appear in `git diff`;
   4. run `git diff --exit-code` on the same paths; on a difference print the
      diff and exit 1, telling the contributor to review, `git add`, and re-push.
5. **prek hook** in `.pre-commit-config.yaml`: `repo: local`, `language: python`,
   `entry: scripts/gazelle_check.py`, `pass_filenames: false`,
   `stages: [pre-push]`, with a `files:` filter for C++ sources, `BUILD.bazel`,
   `MODULE.bazel` and the script itself.
6. **CI job** `gazelle` in `.github/workflows/pr.yml`: ubuntu only, no `needs:`
   (runs in parallel with `pre-commit`), same `setup-bazel` step as the `bazel`
   job, then
   `prek run --hook-stage pre-push --all-files gazelle --show-diff-on-failure`.
7. **Documentation.** A short ADR recording the decision, and a note in the
   [coding style guide](../../coding-style.md) on how to regenerate locally.
8. **One-time baseline commit.** The pilot lands together with the reviewed
   result of the first gazelle run over `src/algorithms`, so the check is green
   from day one. Each change in that baseline is reviewed and must still build
   and pass tests.

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

1. **Baseline is explained.** The first gazelle run over `src/algorithms`
   produces only the change classes listed in the spike findings, including the
   accepted redundant `@googletest//:gtest` on tests. Any other
   class is either fixed with a directive or explained in the baseline commit.
2. **Baseline is safe.** After the baseline commit,
   `bazel build --config=release //...` and
   `bazel test --config=release //src/algorithms/...` pass, and
   `//:portable_closure` still holds. `implementation_deps` rewrites are checked
   specifically, since they change what downstream targets see transitively.
3. **Idempotence.** A second gazelle run produces no diff.
4. **Macro.** Generated tests use `fastecu_portable_gtest`; the macro tolerates
   a `deps` list that repeats `byte_matchers`.
5. **Creation.** In a scratch package, adding `foo.cpp`, `foo.h` and
   `foo_test.cpp` makes gazelle create the library and test, and
   `gazelle_check.py` then exits 1. Throwaway verification, not committed.
6. **Formatting.** Gazelle output is buildifier-clean (no fight between the two
   hooks).
7. **Hook behavior.** The Python hook runs under prek 0.4.0 at the pre-push
   stage, locally and in CI.
8. **Script tests.** The script's decision logic (scope filtering, diff and
   exit-code handling) has co-located unit tests, following the layout of
   `scripts/clang_tidy_runner_test.py` and its `py_test` in the root
   `BUILD.bazel`.

## Risks and mitigations

- **Bazel is slow at push time.** Hook is `pre-push` only and file-filtered.
  `--no-verify` is a conscious bypass; CI remains the hard gate.
- **Baseline changes semantics.** `implementation_deps` and removed hand-written
  deps could change transitive visibility. Criterion 2 builds and tests the whole
  tree before the baseline is accepted.
- **Gazelle vs. buildifier formatting.** Criterion 6.
- **Whole-repo indexing.** Gazelle indexes the repository to resolve labels even
  when only `src/algorithms` is written; runtime is measured in the plan and
  reported, and `-r=false` with `cc_search` lazy indexing is the fallback.
- **Windows contributors.** Python script, no bash dependency.

## Widening later

Each widening adds a path to the `gazelle` rule's `args` and is its own PR whose
diff is the acceptance test for that area, run alongside the existing guardrails
(`//:portable_closure`, ratchet lists, visibility groups). Packages with macros
other than `fastecu_portable_gtest` (`qt_cc_library`, `fastecu_gtest`) will need
their own `map_kind` and moc `hdrs` / `normal_hdrs` handling. Order and cadence
are not decided here.

## Open questions (resolve during planning, not by guessing)

1. Whether `language: python` in a `local` prek hook works without extra
   configuration on prek 0.4.0 (criterion 7).
2. The exact `# gazelle:exclude` spelling that also silences `qt_compat` in
   dependency resolution (criterion 1 shows whether it does).
