# clang-tidy as a Bazel aspect: spike

Status: proposed. Additive spike; the compile-database runner stays the CI gate
until this matches it.

## Goal

Let Bazel's action cache decide what clang-tidy re-checks, so `config_changed`,
the changed-files mode and the merge-base logic in
`tools/clang-tidy/scripts/clang_tidy_runner.py` can go away. Today a config
change makes the run whole-tree, and `config_changed` is a hand-kept list of
inputs that can drift.

## Decisions

| Decision | Choice |
| --- | --- |
| Base project | `aspect_rules_lint`, patched with `single_version_override(patches = [...])` so each patch can be upstreamed later. |
| Custom aspect from scratch | Rejected; `rules_lint` already declares configs as inputs and has a fix flow. |
| Plugin | A generic `plugins` `label_list` attribute: each plugin is a tool input and a `-load=` argument. Built by Bazel from `//tools/clang-tidy/plugin`. |
| LLVM | Pinned `http_archive` in `MODULE.bazel` (version and SHA moved from the `pr.yml` env vars), so a bump invalidates cached results. Linux first. |
| Config chain | Current `.clang-tidy` plus parents, only while `InheritParentConfig: true`. See below. |
| Fix flow | The aspect runs `--export-fixes`; a separate step applies it with `clang-apply-replacements`. `rules_lint`'s in-sandbox `--fix` patch flow is not used: it drops fixed issues from the report. |
| Exceptions, annotations, profiling | A Python wrapper behind `bazel run //:clang_tidy` post-processes the aspect output with `clang_tidy_exceptions.py` and `clang_tidy_output.py`. Exception edits never invalidate the cache. |
| Targets and sources | `cc_library`, `cc_binary`, `cc_test`; first-party, non-generated sources only. Generated files (moc output, `ui_*`, `rep_*_replica`) are excluded by path but stay visible as headers through `CcInfo`, which replaces `_prebuild`. `HeaderFilterRegex` and `ExcludeHeaderFilterRegex` in `.clang-tidy` still decide header diagnostics. |
| Entry point | One target, `bazel run //:clang_tidy`, checks; `-- --fix` also fixes. No `_changed` variant. |

## Config chain

An aspect gets one global attribute and cannot read file contents. So:

- The aspect takes every `.clang-tidy` file as a single attribute.
- A generated lookup table, `bazel/clang_tidy_config_chains.bzl`, maps package to
  config paths. The aspect keeps only that package's files as action inputs.
- `scripts/clang_tidy_configs.py` generates the table and keeps `exports_files`
  for each `.clang-tidy` in step. `--check` mode fails when either is stale.
- It is its own pre-push hook plus a CI check, separate from the Gazelle hook
  (ADR 0017), because the trigger files and failure messages differ.

Today there are two configs: the root file and `src/ui/desktop/.clang-tidy`,
which inherits.

## Delivery

Stacked PRs (`gh stack`), each safe to merge alone, all additive:

1. Pin LLVM in `MODULE.bazel`, add the `rules_lint` dependency and an empty patch.
2. Patch for `plugins`; run the aspect on one package.
3. Config-chain generator with its pre-push hook and CI check.
4. Patch for `--export-fixes`.
5. The `bazel run //:clang_tidy` wrapper and a parity script that diffs findings
   against the current runner.

## Success criteria (Linux)

1. The aspect and the compile-database runner report the same findings, with the
   same checks enabled, on the current tree.
2. A warm-cache run after a one-file change is much faster than today's
   changed-files run.
3. Editing a `.clang-tidy` re-runs only the translation units under it.

## Kill criteria

Abandon the spike, leaving PRs 1–4 unmerged or reverted, if:

- after PR 2 the aspect cannot reproduce the runner's findings on one package, or
- after PR 5 the Linux parity diff has unexplained differences, or a warm run
  after a one-file change is not clearly faster than the changed-files run.

## Deferred

- Windows: the `--scope-os` handling and `.clang-tidy-scope.toml`.
- macOS: the SDK lookup.
- Both wait until Linux passes. The aspect's cross-platform behavior is the
  highest-risk unknown, so a Linux failure ends the spike before that cost.

## Unverified

- How Envoy and other large C++ Bazel projects run clang-tidy. The search found
  nothing, so no claim about them is made here.
- `rules_lint` and `bazel_clang_tidy` behavior comes from their published
  README and source comments, not from running them.
