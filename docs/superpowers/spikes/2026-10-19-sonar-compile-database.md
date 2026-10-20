# Spike: SonarCloud from a cached Bazel compile database

Status: **in progress** (workflow written, not yet run).

## Question

Can SonarCloud take its C++ compile database from a cached Bazel build (Hedron
`compile_commands.json`) instead of the SonarSource build wrapper, without
bringing back the Qt `emit` false positives? The wrapper forces an uncached
build (`--disk_cache=`), which is ~10 of the SonarCloud job's ~13 minutes.

## Background

- #32 introduced the Hedron database; #284 (2026-09-09) replaced it with the
  build wrapper. #284's stated reason: Sonar read `emit Signal(...)` as a
  declaration (466 `cpp:S1117` findings bulk-resolved). The PR called the
  wrapper the fix "almost certainly" and never verified it.
- Sonar forum thread
  [99172](https://community.sonarsource.com/t/unexpected-warning-against-to-qt-code-based-on-c-programming-language-build-wrapper-analysis/99172):
  a Sonar engineer says `emit` should be seen as a macro and calls the
  misparse a configuration problem (CPP-4770). That points at Qt headers not
  being resolved, not at the tool that wrote the database.
- Checked locally on macOS (`aquery` for `biu_operations_subaru.cpp`): Qt
  include paths and `-D` defines are identical under the default and
  `--config=sonar`; only `-g` and the coverage flags differ.
- Every Qt include is a `bazel-out/.../_virtual_includes/...` path. Those
  exist on disk only if the build materialised them, which a remote-cache
  build may not do.

## Hypothesis

`emit` is misparsed when `qobjectdefs.h` cannot be opened at the recorded path.
With the header trees present on the runner, a cached build's database should
analyse like the wrapper's.

## Method

`.github/workflows/sonar-spike.yml` (manual dispatch), scanning into project
`rcusstackwalker_fastecu-sonar-spike` (token: `SONAR_SPIKE_TOKEN`):

1. `wrapper` job: today's build-wrapper path, as `master` (baseline).
2. `compdb` job: BuildBuddy-cached coverage build with
   `--remote_download_regex=.*/_virtual_includes/.*`, Hedron `:refresh`,
   `scripts/sonar_spike_compdb_check.py` (entry count, `argv[0]`, include dirs
   that exist, whether `qobjectdefs.h` resolves; rewrites `cc_wrapper.sh` to
   `clang` if present), then scan as `master`.
3. After each scan, `scripts/sonar_spike_results.sh` waits for processing and
   records issue totals, `cpp:S1117`, `cpp:S824` and `ncloc` for the main
   branch in the job summary. The free plan analyses only the main branch
   (`master`), so the variants take turns on it and are read back immediately.

Run it twice: the first `compdb` run populates the BuildBuddy cache for the
coverage config (the other jobs only build `release`), the second is the
warm measurement. CFamily unit counts come from the scanner log
("Starting the analysis of N CFamily compilation units"); the wrapper's is 261.

## Pass / no-go

Go if, on the warm run: unit count within 5% of the wrapper's; `S1117`/`S824`
differ by no more than a handful; job at least 5 minutes faster. Header
handling options if `qobjectdefs.h` does not resolve: (a) the regex above,
(b) `--remote_download_toplevel`/`_all`, (c) a local build of the Qt targets
before the scan. No-go leads to `Q_EMIT` (343 `emit` sites) or leaving the
wrapper as is.

## Results

_To fill in after the runs._

| | wrapper | compdb (cold) | compdb (warm) |
|---|---|---|---|
| Job time | | | |
| CFamily units | | | |
| Include dirs existing | | | |
| `qobjectdefs.h` resolves | | | |
| Total issues | | | |
| `cpp:S1117` | | | |
| `cpp:S824` | | | |

## Decision

_Go / no-go / go with conditions._
