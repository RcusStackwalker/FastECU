# Tooling

How to run the static-analysis, mutation and fuzzing checks and the NLOC
hill-climbing loop locally, and how CI runs them. The rules they enforce live in the [coding style guide](coding-style.md).

## Build code generation

[Bazel configuration](../.bazelrc) enables `--force_pic` by default, with
`--noforce_pic` on Windows. On Linux this replaces the raw `-fPIC` flag: Bazel
now generates one PIC compilation instead of separate `.o` and `.pic.o`
actions that both already emitted PIC. Executables also link as PIE, retaining
the Qt `staticMetaObject` identity needed by `qobject_cast` without copy
relocations. macOS uses its native PIC-capable toolchain. MSVC and clang-cl
retain their native COFF mode; declaring unsupported PIC features is unnecessary.

The [Windows x86 toolchain](../bazel/toolchains/windows_x86_msvc/BUILD.bazel)
is target-compatible only with Windows, so Unix wildcard builds skip it rather
than validate an unselected MSVC toolchain under `--force_pic`. The
[Android rules patch](../bazel/patches/rules_android_ndk_pic.patch) declares the
PIC mode already implemented by the NDK's unconditional `-fPIC`, allowing
Android cross-compilation from Unix hosts with the same default flag.

## Static analysis

### prek

`prek run --all-files` runs formatting, the `#pragma once` check, Lizard, the
fuzz-seed size limit and the lychee link checker. Run it before pushing.

### Lizard

`prek run lizard --all-files` checks per-function cyclomatic complexity (limit
15) across first-party C/C++ sources, headers and tests, scanning the whole
tree from the repository root so `.gitignore` applies; generated, vendor and
third-party directories are excluded. The hook is pinned in
[the prek configuration](../.pre-commit-config.yaml). Lizard's CLI requires
integer limits, so other metrics are set effectively unbounded.

### clang-tidy

```sh
bazel run //:clang_tidy_report_changed            # the PR gate's changed-files scope
bazel run //:clang_tidy_report_changed -- --profile  # phase timings, slowest checks
bazel run //:clang_tidy_report_changed -- --annotate # also GitHub annotations
bazel run //:clang_tidy_fix_changed               # apply fixes (macOS/Linux, system LLVM on PATH)
```

- **Output.** Findings print as each translation unit finishes, one block per
  finding (`error: src/foo.cpp:12:5 [check]`, message, source snippet, notes),
  each printed once even when several translation units report it. With
  `--annotate`, which CI passes, each finding in a repository file is also a
  GitHub annotation; GitHub shows at most 10 error annotations per step, so
  past that the job log is the full list.
- **Compilation commands.** Analysis keeps the first equivalent command from
  the Bazel database, without rewriting that shared database. This removes the
  Linux `.o`/`.pic.o` duplicate when both already compile with `-fPIC`. Distinct
  compiler, directory, define, include, language and target settings retain
  separate analyses. Unknown options, response files and forwarded arguments
  are handled conservatively; shell command strings are compared literally.
  The log reports retained command configurations and unique source files
  separately.
- **LLVM pin.** CI pins LLVM (`CLANG_TIDY_LLVM_*` in
  [pr.yml](../.github/workflows/pr.yml)) on Linux and Windows. The supported
  local analysis workflow requires that exact release.
- **CI.** The `clang-tidy` job runs in parallel with the build and test job, on
  Linux and, for Windows-exclusive code, on Windows. The Linux job uses the
  shared [Qt runtime setup](../.github/actions/qt-runtime-linux/action.yml)
  before its prebuild.
- **Platform scope.** The [scope manifest](../.clang-tidy-scope.toml) lists
  platform-gated code: Linux analyzes everything it can build, Windows only its
  exclusive code (`-- --scope-os windows`). The runner fails when a
  `BUILD.bazel` gates targets by platform and no manifest prefix covers it.
- **Configuration changes.** Changing any `.clang-tidy` file widens the
  changed-files run to every translation unit in scope. Before delivering a
  check change, run the full report and fix everything it finds in the same
  PR; passing fixtures alone do not show a clean production scope.

#### Repository checks

Repository checks are compiled in
[`scripts/clang_tidy_plugin/`](../scripts/clang_tidy_plugin/); the enabled
`fastecu-*` names live in [`.clang-tidy`](../.clang-tidy). Bazel builds the
analysis artifact automatically for the report, fix, benchmark and fixture
commands. The runner validates the selected LLVM installation, release and
registered checks before analyzing code. Missing artifacts or required checks
fail the run.

Set `FASTECU_LLVM_ROOT` to an installation of CI's exact LLVM release, with
its development headers. On Linux also select that installation's compiler
for the analysis build, since GCC can diagnose LLVM's inline matcher headers:

```sh
export FASTECU_LLVM_ROOT=/absolute/path/to/llvm
# Linux analysis builds:
export CC="$FASTECU_LLVM_ROOT/bin/clang"
export CXX="$FASTECU_LLVM_ROOT/bin/clang++"
bazel run --config=release //:clang_tidy_report_changed
```

On Windows, also set `BAZEL_LLVM` to the same root and `USE_CLANG_CL=1`
for analysis builds. The pinned compiler builds against its own development
headers while retaining warnings as errors in first-party code.

macOS/Linux load a shared extension. The pinned Windows executable does not
export the required symbols, so Bazel builds an analysis host with the checks
linked into it and supplies its missing compression dependencies. The
[packaging qualification](../scripts/clang_tidy_plugin/qualification/README.md)
records the archive evidence; the
[rollout qualification](reference/clang-tidy-plugin-qualification.md) records
checks, scope, parity, runtime measurements, and platform evidence. Ordinary
application builds do not need LLVM development headers.

Built-in and repository checks run together with the wider first-party header
scope under `src/`, `apps/`, `tests/`, and `scripts/`, including `.hpp`. Generated
headers retain their exclusions; vendor headers stay outside the scope.
Changing enforcement sources or configuration widens a changed-files run to
all translation units in its platform scope. Before delivering an enabled
check change, run the full report and fix its findings; fixtures alone do not
establish a clean production scope.

```sh
# Required real-dependency positive and permitted-counterexample fixtures.
# Prepare the compilation database with a report first.
bazel run --config=release //:clang_tidy_check_fixtures
# Compare built-ins alone with built-ins plus repository checks.
bazel run --config=release //:clang_tidy_benchmark -- --jobs 4 --output reports/clang-tidy-custom
```

Fixtures require the registered checks and do not silently skip when an
extension is missing. The benchmark pins translation-unit scheduling and
fingerprints check sources, configuration, host identity and compiled artifacts.
It writes raw logs and JSON into a new output directory. Per-check CPU seconds
sum work across translation units; they are not elapsed time. Investigate
reproducible runtime regressions above 10 percent on the same machine and scope;
wall-clock timing is a review threshold, not a CI pass/fail gate.

The [central exception policy](../.clang-tidy-exceptions.yaml) has schema
`version: 1`, with `exceptions` entries containing exactly `check`, `file`,
`symbol`, and `rationale`. Files are canonical repository-relative paths;
identities are `Suite.Test` or a qualified helper signature. Wildcards and
line-number identities are unsupported. Malformed and duplicate entries fail
before analysis; a full-scope report also rejects unused entries. Changed-file
runs do not reject entries outside their analyzed scope.

Result access reasoning is intraprocedural and bounded to structured control
flow, fatal state matchers, terminating guards, stable local/member identities,
and supported mutation/alias patterns. Functions containing source-level
`goto` or exception handlers receive no propagated state proofs;
`switch` bodies do not export new proofs, and possible mutations invalidate
prior facts. Fresh assertions after a switch can establish a new proof.
GoogleTest's internal macro control flow does not impose this restriction.
Lambda bodies are analyzed independently with empty incoming facts; caller,
captured-state and framework setup proofs are not imported across functions.
At loop increments and do-while conditions, continue paths conservatively
retain only mutation-free loop-entry proof, rather than assertions that a
continue could bypass.
Use a simpler local proof or a narrowly justified central exception where
needed. Unknown aliases and complex matcher compositions remain review
responsibilities. Single-use suggestions require an adjacent final block pair,
a void Result, and a pure `IsOk()` matcher to preserve cleanup ordering.
Uniform-buffer detection covers supported unsigned-byte vectors, spans and
arrays; custom projections and unrelated containers are permitted.

### SonarCloud

The `sonarcloud` job ([sonar.yml](../.github/workflows/sonar.yml)) gates the PR
against [sonar-project.properties](../sonar-project.properties). To reproduce
it locally, install `sonar-scanner`, then mirror CI:

```sh
scripts/coverage-local.sh
bazel run --config=coverage //bazel/compile_commands:refresh -- --config=coverage
python3 scripts/sonar_compdb_check.py compile_commands.json
sonar-scanner -Dsonar.token=$SONAR_TOKEN
```

The token is a personal one (SonarCloud → My Account → Security), not the CI
secret. Sonar reads its C++ compile commands from Hedron's
`compile_commands.json`, so the build stays cacheable (BuildBuddy in CI). The
check script fails if Qt's `qobjectdefs.h` does not resolve through the
database: without it Sonar misreads `emit` as a declaration and raises
`cpp:S1117`/`cpp:S824` false positives.

### CodeQL

[codeql.yml](../.github/workflows/codeql.yml) scans Actions, C/C++ (without a
build) and Python weekly, and on demand through `workflow_dispatch`. It does
not run on pull requests. GitHub's CodeQL default setup must stay disabled in
the repository's code security settings; while it is enabled, the workflow's
uploads are rejected.

## Mutation testing

The PR `mutation` job mutates changed portable lines and never fails a PR
([ADR 0022](adr/0022-advisory-mutation-testing-with-mull.md)). Locally, put
Mull's `mull-runner-<N>` and its LLVM's `clang` first on `PATH` (set
`MULL_IR_FRONTEND` if the plugin is not beside the runner):

```sh
python3 scripts/mutation_test.py --scope //src/algorithms/checksum/...
python3 scripts/mutation_test.py --diff-ref origin/master   # changed lines only
```

On macOS there is no Mull release for LLVM 22 yet (0.34.2 stops at LLVM 21;
LLVM 22 support is on Mull's `main`), so use Mull's nightly formula, built
against the same Homebrew LLVM:

```sh
brew install llvm@22 mull-project/mull/mull-nightly@22
export PATH="/opt/homebrew/opt/llvm@22/bin:$PATH"   # Homebrew's clang first, then run the commands above
```

`scripts/mutation_test.py` adds `--config=mutation-macos`
([`bazel/mutation.bazelrc`](../bazel/mutation.bazelrc)) on macOS. Bazel's Apple
toolchain otherwise compiles with Xcode's clang through `xcrun`, ignores `CC`,
and fails with `Wrong API version on plugin`. A nightly Mull can score a few
mutants differently from the 0.34.1 release CI pins, so compare runs made with
the same install.

A test-only change touches no mutable lines, so the PR job has nothing to
mutate. Run the workflow scoped on the branch and compare its survivors with
the same scope on `master`:

```sh
gh workflow run mutation -r <branch> -f scope=//src/algorithms/protocol/...
```

The `merged report` job's summary and `mutation-report` artifact hold the
result.

### Mutation performance loop

The target is **20 minutes from workflow start through the merged report**
for all portable tests. Record runner queue delay separately. Keep the
production revision, Mull/LLVM pins, mutant scope and runner class fixed when
comparing experiments. Surviving mutants remain advisory; incomplete runs
are failures, even if their partial reports look faster.

The workflow uses [duration estimates](../bazel/mutation-schedule.json) to
balance six jobs and GoogleTest slices of expensive binaries. Local runs
can use the identical selection and assignment. Use a fresh report/download
directory for each run so earlier artifacts cannot contaminate a comparison:

```sh
python3 scripts/mutation_test.py --schedule bazel/mutation-schedule.json \
  --shard 0/6 --report-dir /tmp/mutation-shard-0
```

1. Run one experiment on a feature branch, changing one performance variable.
   Start with a scoped run, then measure the full workflow:

   ```sh
   gh workflow run mutation -r <branch> -f baseline_run=<baseline-run-id>
   ```

2. Inspect the merged summary: build/query time, total Mull execution, the
   slowest work items, per-shard loads and baseline mutant comparison. The
   artifact's `_performance.json` files record revision, run ID, each slice's
   measured seconds and completion status, including the failing slice on a
   runner error. Setup/upload and final workflow duration come from Actions
   job/step timestamps. The runner-capacity step records the CPU model and
   logical CPU count; compare these too when execution speeds differ.

3. Compare identical production sources and scope. Investigate missing
   mutant identities, lost direct kills, additional survivors or new timeouts
   before retaining a speedup. Timeouts remain killed for the score but are
   shown separately. New mutant identities can expose coverage aggregation
   differences and also need review.

4. Download a complete candidate and refresh the estimates from measured
   slices. Review the resulting diff; a scoped refresh preserves other
   estimates. Partial binary slices and failed work cannot refresh estimates.

   ```sh
   gh run download <run-id> -n mutation-report -D /tmp/mutation-candidate
   python3 scripts/mutation_test.py --refresh-schedule /tmp/mutation-candidate \
     --schedule bazel/mutation-schedule.json
   ```

5. Repeat full runs after balancing. Qualify the target with three consecutive
   complete runs under 20 minutes and record their IDs, durations, cache
   conditions and quality differences. Aim for at most 17 minutes of Mull
   work on the slowest job to reserve time for setup, builds and merging.

The initial estimates come from
[run 39027359985](https://github.com/RcusStackwalker/FastECU/actions/runs/39027359985).
Its five slowest binaries account for 72% of execution, so their test slices
are the first optimization experiment. No test cases or production guards
are removed to meet the budget.
Experiment results and qualification evidence are recorded in
[PR 692](https://github.com/RcusStackwalker/FastECU/pull/692).
For executor-level profiling, see the
[flash executor performance investigation](reference/executor-test-performance.md).

## NLOC hill-climbing loop

The decision is [ADR 0023](adr/0023-hill-climb-nloc-with-kind-specific-guards.md).
An agent runs it with `/loop /hill-climbing-nloc`: each iteration of the
[project skill](../.claude/skills/hill-climbing-nloc/SKILL.md) settles the
open loop PR or produces the next one with the commands below.

```sh
python3 scripts/nloc_loop.py measure            # total, production and test NLOC
python3 scripts/nloc_loop.py candidates         # ranked duplicate-cluster queue
python3 scripts/nloc_loop.py guards snapshot    # test cases and coverage of HEAD
python3 scripts/nloc_loop.py guards check --kind test-only   # HEAD against its base
python3 scripts/nloc_loop.py ledger record <cluster> failed --reason mutation
python3 scripts/nloc_loop.py ledger list        # attempts and the streak of failed ones
python3 scripts/nloc_loop.py trend --pr <n> --kind test-only --before <base NLOC>
```

`measure` runs the manual-stage `nloc` prek hook, which shares the `lizard`
hook's pinned dependencies and excludes. A file is test code when it is under
`tests/`, `testing/` or `testdata/`, or its name starts with `tst_` or contains
`_test.` or `_test_`.

`candidates` runs the manual-stage `jscpd` prek hook (settings in
`.jscpd.json`; its ignore list mirrors `sonar.cpd.exclusions`) and a Bazel
`cquery` for the sources that build on the host. Duplicate pairs that share a
fragment, or overlap within a file, form one cluster; transitive links can
join fragments that are only similar to their neighbours, so read the
occurrences before choosing an abstraction. Occurrences in files that do not
build on the host are dropped, as are clusters saving fewer than 50 NLOC and
clusters already in the ledger. Test-only clusters are listed first, each kind
by estimated savings. The cluster ID is stable across line shifts.

`ledger record` takes `open`, `merged`, `closed`, `failed` (a guard failed after
the work was done), `unsuitable` (judged by reading, before any work) and the
older `rejected`, which counts as `failed`. The streak that stops the loop
counts trailing failed attempts; an unsuitable cluster costs no work, so it
neither counts nor breaks the streak.

`trend` measures the working tree and appends a row to
`docs/reference/nloc-trend.csv`; run it on the loop PR's branch before merge.

`guards snapshot` runs `scripts/coverage-local.sh` into `.nloc-loop/coverage`
and records the clean HEAD's executed test cases (from the `test.xml` files
that run wrote; skipped and disabled cases excluded) and per-file covered
lines in `.nloc-loop/snapshots/<commit>.json`. A failing test fails the
snapshot. Take one on the base (the merge-base with `origin/master`) before
branching and one on the loop PR's HEAD; `guards check` compares them and
exits 1 when a guard fails:

- Both kinds: executed test cases do not drop.
- Test-only: no production file changed, and every production line covered
  on the base is still covered.
- Production, judged on lines rather than per-file ratios (a ratio punishes
  moving well-covered code into a shared file; ADR 0023):
  - *unchanged lines stay covered*: a line a changed production file shares
    with the base (mapped through `git diff -U0`) and the base covered is still
    covered;
  - *uncovered lines not added*: the executable lines the PR adds that no test
    runs (new files included) do not outnumber the uncovered lines it deletes.
    Moving code keeps the count equal; adding code needs its own tests.

  Snapshots taken before the guards tracked uncovered lines are rejected with a
  request to re-run `guards snapshot`.

The mutation guards run in CI ([ADR 0023](adr/0023-hill-climb-nloc-with-kind-specific-guards.md))
on PRs labelled `nloc-loop`, through the [`nloc-loop` workflow](../.github/workflows/nloc-loop.yml).
`pr-kind` derives the kind from the changed files; the PR title does not
decide it. A test-only PR's base and head each run Mull in six shards over the
portable tests that depend on its changed test sources (`mutation-scope`, in
either checkout), and every mutant the base kills must still be killed. A
production PR's changed lines are mutated, and any survivor fails. The
`nloc-loop verdict` check reports the outcome; make it a required check for
it to block a merge. A test-only change to UI or platform tests has no
mutation scope, so only the local guards apply. Locally:

```sh
python3 scripts/nloc_loop.py pr-kind --base <sha> --head <sha>
python3 scripts/nloc_loop.py mutation-scope --base <sha> --head <sha>
python3 scripts/nloc_loop.py guards mutation --base-reports <dir> --reports <dir> --shards 6
```

- **NLOC**: Lizard's `Total nloc` over first-party C++ in `src/`, `apps/`
  and `tests/` with the prek excludes. Non-blank, non-comment lines on the
  clang-formatted tree. Avoid "line count", which is ambiguous.
- **Candidate cluster**: a group of duplicated blocks reported by jscpd,
  ranked by estimated savings.
- **Loop PR**: one iteration of the loop: a pull request that lowers NLOC by
  at least 50 and passes the guards for its kind.
- **Test-only PR**: a loop PR whose production sources are byte-identical to
  the base.
- **Production PR**: a loop PR that changes production sources.
- **Ledger**: the gitignored record of attempted clusters and their outcomes,
  which keeps rejected clusters from being retried.

## Fuzzing

A target named `*_fuzz_test` links FuzzTest, is tagged `fuzz`, and builds only
under `--config=asan` or `--config=fuzztest`
([ADR 0021](adr/0021-fuzz-portable-code-with-fuzztest.md)). Both need Clang
first on `PATH`: CI uses the pinned clang-tidy LLVM; on macOS Xcode's clang
works.

```sh
# The PR gate: portable tests and every committed seed under ASan + UBSan.
bazel test --config=asan //src/algorithms/... //src/backend/...
# Coverage-guided fuzzing of one property.
bazel run --config=fuzztest //src/algorithms/expression:expression_fuzz_test -- \
    --fuzz=ExpressionFuzz.EvaluatesAnyExpressionSafely --fuzz_for=10m
```
