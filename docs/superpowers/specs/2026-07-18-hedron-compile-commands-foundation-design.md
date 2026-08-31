# Hedron compile_commands foundation — design

**Date:** 2026-07-18
**Status:** Approved (brainstorming)

## Problem

Two open PRs each independently add the `hedron_compile_commands` extractor to
generate `compile_commands.json`, and each carries its own Bazel 9 compatibility
workaround:

| | PR #23 (`remove-qmake-bazel`) | PR #24 (`markelov/actionable-clang-tidy`) |
|---|---|---|
| Override | `archive_override` pinned to upstream HEAD `abb61a6` + `integrity` | `git_override` pinned to older commit `0e99003` |
| Bazel 9 fix | **Patch** hedron to `load()` `py_binary`/`cc_binary` from rules_python/rules_cc | Global `.bazelrc` flag `--incompatible_autoload_externally=cc_binary,py_binary` |
| Consumer | `refresh_compile_commands` for **SonarCloud** | `refresh_compile_commands` for **clang-tidy** |
| Python | system `python3` | adds rules_python 3.11 toolchain + `pip.parse` (pyyaml) |
| `external/` | renames `external/` → `hardware/` to dodge hedron's root `external` symlink | (not addressed) |

The duplicated dependency wiring conflicts between the two branches, and the two
Bazel 9 mechanisms diverge. Whichever PR merges second must resolve a hedron
merge conflict that has nothing to do with its actual feature.

Root cause of the Bazel 9 break: upstream hedron HEAD (`abb61a6`, Aug 2025)
still calls `native.py_binary` and native `cc_binary`, both removed in Bazel 9.
There is no Bazel-9-compatible upstream release, so *something* must carry the
fix. Bazel in use here is **9.1.1**.

## Goal

Extract a single, tightly-scoped, independently-mergeable PR that adds a working
`compile_commands.json` generator on Bazel 9.1.1 — the **shared foundation** that
both the clang-tidy migration (#24) and the SonarCloud migration (#23) then build
on. After it lands, #23 and #24 rebase and delete their duplicated hedron bits.

Non-goal: this PR does not add any consumer (no Sonar rewiring, no clang-tidy
runner) and no CI wiring. Those stay in #23/#24.

## Decisions

- **Mechanism: patch, pinned to upstream HEAD.** Use `archive_override` pinned to
  the current upstream HEAD commit `abb61a6` with an `integrity` hash, carrying a
  patch that makes hedron load `py_binary`/`cc_binary` from rules_python/rules_cc.
  Chosen over the global `--incompatible_autoload_externally` flag because the
  patch is scoped to hedron alone, whereas the flag changes native-rule
  resolution for the entire build and is itself a temporary migration shim.
  Pinning to *HEAD* (not an arbitrary older commit) means the pin is current and
  the patch is guaranteed to apply; bumping the commit later is a deliberate,
  reviewable action.
- **Correcting a false constraint.** PR #23's comment claims "git_override cannot
  carry patches." That is inaccurate — `git_override` forwards kwargs to
  `git_repository`, which supports `patches`/`patch_strip`. `archive_override` is
  still preferred here for the `integrity` hash (hermetic, reproducible), but the
  choice is on its merits, not a limitation.
- **No extra Python plumbing.** The repo already runs `py_test` targets in CI with
  no explicit `python.toolchain` registration, proving rules_python's default
  toolchain resolves hedron's generated `py_binary`. So: no `pip.parse`, no
  pyyaml, no python 3.11 toolchain (those were PR #24's clang-tidy-runner needs,
  not hedron's).
- **Neutral, documented target.** One `refresh_compile_commands` target at a
  neutral location (`bazel/compile_commands/BUILD.bazel`, target `:refresh`),
  explicitly commented as the shared base for the clang-tidy and Sonar
  migrations, rather than living in either consumer's package.
- **Embed `--config=release` in the target.** `bazel run
  //bazel/compile_commands:refresh` works with no extra flags (PR #24's style),
  which reads better as a shared entry point than requiring `-- --config=release`.
- **Directory deconfliction here, not in #23.** hedron drops an `external`
  symlink at the workspace root during generation, colliding with the repo's
  `external/` directory. Move that directory to `3rdparty/hardware/` (preferred
  over PR #23's root-level `hardware/`), consolidating the full deconfliction into
  this one PR.

## Scope — in

1. **`MODULE.bazel`** — add the dev dependency + override:
   ```python
   bazel_dep(name = "hedron_compile_commands", dev_dependency = True)

   # Upstream HEAD still calls native.py_binary / native cc_binary, both removed
   # in Bazel 9. Pin the commit + integrity and patch it to load py_binary /
   # cc_binary from rules_python / rules_cc. Bump the commit deliberately.
   archive_override(
       module_name = "hedron_compile_commands",
       integrity = "sha256-Gwir/7++ifbb7mpbM3U3kugAT2o283wPchFb7IbmhyQ=",
       strip_prefix = "bazel-compile-commands-extractor-abb61a688167623088f8768cc9264798df6a9d10",
       urls = ["https://github.com/hedronvision/bazel-compile-commands-extractor/archive/abb61a688167623088f8768cc9264798df6a9d10.tar.gz"],
       patch_strip = 1,
       patches = ["//bazel/patches:hedron_bazel9_py_binary.patch"],
   )
   ```

2. **`bazel/patches/hedron_bazel9_py_binary.patch`** — the Bazel 9 fix (verbatim
   from PR #23, the minimal correct form). It:
   - adds `load("@rules_cc//cc:defs.bzl", "cc_binary")` to hedron's `BUILD`;
   - adds `bazel_dep(rules_cc)` and `bazel_dep(rules_python)` to hedron's
     `MODULE.bazel`;
   - adds `load("@rules_python//python:defs.bzl", "py_binary")` and rewrites
     `native.py_binary(` → `py_binary(` in `refresh_compile_commands.bzl`.

3. **`bazel/patches/BUILD.bazel`** — add `hedron_bazel9_py_binary.patch` to the
   existing `exports_files([...])`.

4. **`bazel/compile_commands/BUILD.bazel`** (new) — the shared target:
   ```python
   load("@hedron_compile_commands//:refresh_compile_commands.bzl", "refresh_compile_commands")

   # Shared compile_commands.json foundation. Consumed by the clang-tidy migration
   # (//:clang_tidy_* targets) and the SonarCloud C/C++ analyzer. Scoped to app +
   # tests: hedron's default @//... pulls in the Windows MSVC toolchain targets,
   # which fail extraction on non-Windows hosts.
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

5. **Relocate `external/` → `3rdparty/hardware/`** — resolve the root `external`
   symlink collision.
   - `git mv` the three files (`FastECU-mc68hc16-bdm.ino`,
     `NANOFastECU-mc68hc16bdm.ino`, `MC68HC16Y5_BDM_TP_800x600.jpg`).
   - Update `FastECU.pro:392`: `external/FastECU-mc68hc16-bdm.ino` →
     `3rdparty/hardware/FastECU-mc68hc16-bdm.ino` (keeps qmake correct; this PR
     lands before #23 removes qmake).
   - Add a one-line `3rdparty/hardware/README.md` noting these are MC68HC16 BDM
     programmer sketches (they lose the self-describing `external/` context).
   - No `BUILD.bazel` there — nothing builds these; the app's Bazel targets use
     explicit source lists, not globs over that path.

6. **`.gitignore`** — add `/external` (hedron's generation-time symlink).
   `compile_commands.json` is already ignored.

7. **`MODULE.bazel.lock`** — regenerated by the resolution.

## Scope — out (stays in the consumer PRs)

- SonarCloud: `sonar-project.properties`, `scripts/gen-compile-commands.sh`
  (the `cc_wrapper.sh` → `clang` compiler rewrite). → #23
- clang-tidy: the runner, python 3.11 + `pip.parse`/pyyaml, README docs,
  `.bazelrc` clang-tidy changes, `//:clang_tidy_*` targets. → #24
- All CI wiring (`.github/workflows/pr.yml`). Each consumer PR adds its own.

## Verification

Local (stated in the PR description):

```
bazel run //bazel/compile_commands:refresh   # writes compile_commands.json, exit 0
bazel build //bazel/compile_commands:refresh  # patch applies cleanly
```

The build of the target on the CI matrix (once #23/#24 rebase onto this) proves
the patch applies on Linux, macOS, and Windows. No new CI step is added by this
PR.

## Downstream follow-up (documented, not done here)

- **#23** rebases: drop its `MODULE.bazel` hedron block and patch (now shared),
  drop its `external/` → `hardware/` rename (now `3rdparty/hardware/` here) and
  repoint the Sonar `external/**` exclusion accordingly; keep only
  `sonar-project.properties` + `scripts/gen-compile-commands.sh`, repointed at
  `//bazel/compile_commands:refresh`.
- **#24** rebases: drop its `git_override` and the `.bazelrc`
  `--incompatible_autoload_externally` flag; repoint its `clang_tidy_compdb`
  consumers at `//bazel/compile_commands:refresh` (or alias it).
