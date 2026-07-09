# Bazel CI warning cleanup and Bazel 9 migration

Date: 2026-07-09
Branch base: `markelov/fix-bazel-windows`
Worktree: `bazel9-migration`

## Context

CI run [29011179755](https://github.com/RcusStackwalker/FastECU/actions/runs/29011179755) builds FastECU under Bazel 8.4.2 across Windows/macOS/Linux, plus a separate clang-tidy report step. Reviewing that run's logs for Bazel-emitted (not compiler/linker) warnings, and test-building the repo locally against Bazel 9.1.1 via bazelisk to find real Bazel 9 blockers, surfaced four warnings and one hard obstacle. This spec fixes all of them and bumps `.bazelversion` to the latest release.

Scope is explicitly limited to Bazel-emitted diagnostics and the Bazel 9 migration itself — MSVC/clang compiler warnings (C4700, C4129, C4333, LNK4044, `-Wunused-variable`) seen in the same CI run are pre-existing C++ issues, unrelated to this task, and are left untouched.

## Findings

### Bazel-emitted warnings (reproduced against Bazel 8.4.2 and 9.1.1)

1. **`Target @@//:X is using implicit __init__.py creation`** — every `py_test` in root `BUILD.bazel` (`qmake_bazel_sync`, `bazel_openssl_wiring`, `openpty_includes`, `windows_preprocessor_guards`, `clang_tidy_report`) hits this at least intermittently; verified locally that 4 of the 5 warn on a given invocation. None of the underlying scripts do intra-repo package imports, so the implicit `__init__.py` behavior is unnecessary.
2. **`Build options --compilation_mode and --copt have changed, discarding analysis cache`** — `.github/workflows/pr.yml`'s "clang-tidy report" step runs `bazel test //:clang_tidy_report --test_output=all` without `--config=release`, while the preceding "Build Bazel targets" / "Test Bazel targets" steps in the same job use `--config=release`. The mismatched compilation mode invalidates the whole analysis cache between steps.
3. **`input 'lib' of //tests:X is a directory; dependency checking of directories is unsound`** (macOS only, ~30 occurrences) — traced to `rules_qt`'s vendored `extension/qt/6.8.3/mac_aarch64.BUILD`, which does `exports_files(["qml", "plugins", "lib", "share"])`, exporting the Qt SDK's `lib` framework directory as a raw directory label. This is upstream `rules_qt` behavior, not FastECU's own BUILD files. **Decision: leave as-is**, documented as a known, non-blocking, out-of-repo warning.
4. **`tests whose specified size is too big`** — the same 5 `py_test` targets run in ~1.4–1.6s but default to `size = "medium"` (MODERATE, expects tens of seconds). Confirmed via `--test_verbose_timeout_warnings`.

### Bonus finding (in scope per user confirmation)

`//:clang_tidy_report` actually **times out at 300s** on both Linux and macOS CI (`TIMEOUT in 300.0s` / `TIMEOUT in 300.1s` in the log), masked by `continue-on-error: true` on that workflow step. The clang-tidy report has never completed in CI — it silently times out every run. It shells out to `clang-tidy` across the full source manifest (`scripts/clang_tidy_report.py`), which plausibly takes longer than the default MODERATE timeout on a full, non-incremental run.

### Bazel 9 obstacle (confirmed by building against real Bazel 9.1.1 locally)

`bazel/openssl_ext.bzl` generates a `BUILD.bazel` file for the `openssl` repo (`_BUILD_IMPORTED` / `_BUILD_AMBIENT` templates) that calls native `cc_import(...)` / `cc_library(...)` **with no `load()` statement**. Bazel 9 defaults `--incompatible_autoload_externally` to empty, so these rules are no longer autoloaded from Bazel core. Reproduced directly:

```
ERROR: .../external/+openssl+openssl/BUILD.bazel:1:10: cc_import(...)
Error in fail: This rule has been removed from Bazel. Please add a `load()` statement for it.
```

This cascades into analysis failures for every target depending on `//:fastecu_core_common` (i.e. `//:fastecu` and all of `//tests/...`) — 30 of 74 top-level targets failed to analyze in the local repro.

After establishing the fix (adding the two `load()` lines to the generated BUILD text), no other obstacle was found: the repo is already bzlmod-only (no `WORKSPACE`), and `rules_cc`/`rules_qt` (as currently pinned: `rules_cc` 0.2.22, `rules_python` 2.2.0, `rules_qt` 0.0.6, `platforms` 1.0.0) already load `cc_binary`/`cc_library`/`cc_test` explicitly from `@rules_cc`, verified by inspecting the fetched repos directly. No `bazel_dep` version bumps are needed. The remaining 44/74 targets, including a full compile+link of `tests/force_asserts:tst_force_asserts`, built successfully under Bazel 9.1.1 on macOS with today's pinned dependencies.

Windows-specific behavior can't be verified locally (no Windows machine available); the fix is a platform-independent Starlark change, so CI on the branch is the verification gate for that leg.

## Changes

1. **`bazel/openssl_ext.bzl`** — add to the top of both `_BUILD_IMPORTED` and `_BUILD_AMBIENT` string templates:
   ```
   load("@rules_cc//cc:cc_import.bzl", "cc_import")   # _BUILD_IMPORTED only
   load("@rules_cc//cc:cc_library.bzl", "cc_library")  # both templates
   ```

2. **`.bazelrc`** — add `build --incompatible_default_to_explicit_init_py` under the `common`/`build` flags. This is the single repo-wide flag Bazel's own warning text recommends, and is preferred over annotating `legacy_create_init = False` on five individual targets: it's a one-line fix, it's the documented forward-compatible default that will eventually flip anyway, and it covers any `py_test`/`py_binary` added later without requiring a reviewer to remember the per-target attribute. Verified none of the five current `py_test` scripts rely on implicit `__init__.py` (no intra-repo relative imports).

3. **`BUILD.bazel`** — add `size = "small"` to `qmake_bazel_sync`, `bazel_openssl_wiring`, `openpty_includes`, and `windows_preprocessor_guards`. Add `timeout = "eternal"` to `clang_tidy_report` (kept at its current implicit size; only the timeout ceiling changes, since a real full clang-tidy pass is long-running and the exact duration hasn't been measured — eternal (3600s) gives headroom rather than guessing a tighter number).

4. **`.github/workflows/pr.yml`** — add `--config=release` to the "clang-tidy report" step's `bazel test //:clang_tidy_report --test_output=all` invocation, matching the compilation mode used by the preceding build/test steps in the same job.

5. **`.bazelversion`** — bump `8.4.2` → `9.1.1` (latest stable release as of 2026-07-09).

## Out of scope

- MSVC/clang compiler and linker warnings (C4700, C4129, C4333, LNK4044, `-Wunused-variable`) — not Bazel-specific, left untouched per explicit scoping decision.
- The `rules_qt` directory-input warning — upstream ruleset behavior, not worth a new patch for a cosmetic warning.
- Investigating *why* `clang_tidy_report` takes so long, or making it faster/incremental — only its timeout ceiling is being fixed here so it can complete and actually report something.

## Verification plan

- Build and test locally against Bazel 9.1.1 via `bazelisk` (`USE_BAZEL_VERSION=9.1.1`) on macOS: `bazel build --config=release //:fastecu //tests/...` and `bazel test --config=release //:qmake_bazel_sync //:bazel_openssl_wiring //:openpty_includes //:windows_preprocessor_guards //:clang_tidy_report`, confirming the openssl fix resolves the analysis failure and the four warning classes are gone from the output.
- Update `.bazelversion` to 9.1.1 last, after the above passes, so the working tree is verified against the target version before pinning it.
- Push the branch and rely on CI (`Bazel (windows-latest)`, `Bazel (ubuntu-latest)`, `Bazel (macos-latest)`) for final cross-platform confirmation, since Windows can't be tested locally.
