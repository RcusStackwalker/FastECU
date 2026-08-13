# clang-tidy changed-files gate

## Problem

`//:clang_tidy_fix` and `//:clang_tidy_report` run `run-clang-tidy` over the
entire `//...` compile database on every invocation and stream every
subprocess's raw stdout — `run-clang-tidy`'s per-translation-unit "N warnings
generated" boilerplate, and the full `bazel build` log for the prebuild and
(in fix mode) the post-fix rebuild — even when the run is completely clean.
That combination (whole-repo scope, unfiltered output) makes `clang_tidy_fix`
too slow and too noisy to run before every PR, so in practice it isn't run
locally at all; `.clang-tidy` compliance is only checked once, in
`pr.yml`, after the PR is already open.

## Design

### Changed-file detection

Add a `--changed` flag to `scripts/clang_tidy_runner.py`'s existing
`report`/`fix` CLI. Omitting it preserves today's full-`//...` behavior
exactly, for both existing targets.

When `--changed` is set, the changed-file set is the union of:

- `git diff --name-only <merge-base of HEAD and origin/master>..HEAD` —
  commits already made on the branch
- `git diff --name-only` — unstaged working-tree changes
- `git diff --name-only --cached` — staged changes
- `git ls-files --others --exclude-standard` — new untracked files

The base ref is fixed at `origin/master`; there is no `--base` override. Git
commands run through the same injectable `command_runner` seam
`run_workflow` already uses for `bazel`/tool invocations, so the detection
logic is unit-testable with fakes the way the rest of the runner is tested.

Direct source files (`.c`/`.cc`/`.cpp`/`.cxx`) in the changed set map
straight to their compile-database entry by path. A changed header (`.h`)
falls back to co-located sources in the same directory: an entry whose
filename stem exactly matches the header's stem, or matches
`<stem>_test`, is included (this is the `foo.h`/`foo.cpp`/`foo_test.cpp`
co-location convention this repo already follows). If a changed header has
no co-located source in the compile database, print one line naming it and
noting that its includers weren't checked, then continue — the design does
not attempt to trace transitive includers of a changed header.

If the filtered entry set ends up empty (for example, a docs-only PR),
skip `run-clang-tidy` entirely and exit 0 with a one-line message.

The prebuild and compile-database refresh continue to target `//...`, not a
scoped subset — this is a deliberate simplification, not an oversight.
Bazel's own incremental cache already makes a repeat `bazel build //...`
cheap on a warm tree, and in CI the `bazel` job in `pr.yml` already runs
`bazel build -k --config=release //...` earlier in the same job, so the
prebuild inside the runner is close to a no-op there too. The actual cost
`run-clang-tidy` pays is analysis time per translation unit, which scales
with the *filtered* TU count regardless of how broad the surrounding Bazel
build is. Scoping the build step itself (e.g. via `bazel query` to find only
the targets containing changed files) would add real complexity for a step
that's already fast in the common case, so it's left alone.

### Output

Both build subprocess calls (prebuild, and fix mode's post-fix rebuild)
change from streaming output live to capturing it. Each prints one line
before running ("Building analyzed targets...") and only emits the captured
log if the build fails.

`run-clang-tidy` always runs with `-export-fixes` now, in both `report` and
`fix` mode — in `report` mode nothing is applied, but the exported YAML gives
a structured diagnostic count to summarize instead of relying on parsed
stdout. Its stdout/stderr is captured rather than streamed; the runner prints
"Analyzing N translation units..." before running, then:

- on a clean run: `clang-tidy: N files clean, 0 findings`, nothing else
- on a failing run: the captured diagnostic text (file:line, message, and
  source snippet, exactly as `run-clang-tidy` produced it) followed by a
  summary count

This changes nothing about what counts as failure — the exit-code logic in
`run_workflow` is unchanged — only what gets printed on the way there.

### New Bazel targets

`//:clang_tidy_fix_changed` and `//:clang_tidy_report_changed` in
`BUILD.bazel`, mirroring the existing `clang_tidy_fix`/`clang_tidy_report`
`py_binary` targets with `--changed` appended to `args`. The existing
`clang_tidy_fix`/`clang_tidy_report` targets are unchanged and remain
available for occasional full-repo sweeps.

### CI wiring

`pr.yml`'s `bazel` job replaces its single `clang-tidy report` step's command
with `bazel run --config=release //:clang_tidy_report_changed`, still run
once per OS in the existing three-way matrix — that matrix exists because
platform-specific compile commands produce genuinely different diagnostics
(see the Windows-specific `clang-diagnostic-error` suppression already in
`.clang-tidy`), and changed-files mode doesn't change that. The job's
checkout step gains `fetch-depth: 0`, matching the pattern `release.yml`
already uses, so `origin/master` is reachable for the merge-base
computation.

`release.yml` is not changed. Full-repo `clang_tidy_report` drops out of CI
entirely — it no longer runs anywhere automatically. It remains available as
a manual/local target for occasional full sweeps. Enforcing full-repo
cleanliness continuously is the tech-debt document's P2 "turn static analysis
into a ratchet" item, and is out of scope here: that item is about making the
*full* report's CI signal trustworthy (baseline + ratchet by check/path),
which is orthogonal to giving local development a fast per-PR gate.

## Testing

Extend `scripts/clang_tidy_runner_test.py`, following its existing
fake-`command_runner` pattern, to cover:

- changed-set detection: direct source match, header co-location fallback
  (`_test` and exact-stem variants), a header with no co-located source
  (note printed, run continues), and an empty filtered set (short-circuits
  before invoking `run-clang-tidy`)
- the merge-base git invocation sequence, faked the same way existing tests
  fake `bazel`/tool calls
- the quiet-output behavior: build output suppressed on success and surfaced
  on failure, for both the prebuild and post-fix-build steps, and the same
  for the `run-clang-tidy` invocation itself
- that omitting `--changed` reproduces the existing full-`//...` behavior
  unchanged, so the existing full-mode test coverage continues to hold

Manual verification: run `bazel run --config=release
//:clang_tidy_fix_changed` locally against a branch with a small pending
change and confirm the output is a few lines, not the full transcript; run
`//:clang_tidy_report_changed` against a branch with a deliberately
introduced clang-tidy violation and confirm it fails with just that
violation's diagnostic text.
