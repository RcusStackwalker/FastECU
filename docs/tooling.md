# Tooling

How to run the static-analysis, mutation and fuzzing checks locally and how CI
runs them. The rules they enforce live in the [coding style guide](coding-style.md).

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
bazel run //:clang_tidy_fix_changed               # apply fixes (macOS/Linux, system LLVM on PATH)
```

- **LLVM pin.** CI pins LLVM (`CLANG_TIDY_LLVM_*` in
  [pr.yml](../.github/workflows/pr.yml)) on Linux and Windows; a different local
  LLVM can report differently.
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

#### Custom checks

The repository checks are the `CustomChecks` entries in
[`.clang-tidy`](../.clang-tidy); each query's comment documents what it
matches and how to fix a finding. They run in a separate inline pass that also
covers headers under `apps/` and `tests/`. The
[adapter](../scripts/clang_tidy_adapter.py) adds LLVM's experimental flag; a
direct invocation needs it plus the registered name:

```sh
clang-tidy --experimental-custom-checks --checks='-*,custom-fastecu-linear-search-loop' \
    -p . src/backend/flash/ecu/subaru_denso_sh7058_can_plan.cpp
```

```sh
# Verify inline diagnostics and permitted counterexamples (run a report first
# to prepare the compilation database).
bazel run --config=release //:clang_tidy_check_fixtures
# Compare baseline and inline-check analysis time over the full local scope.
bazel run --config=release //:clang_tidy_benchmark -- --jobs 4 --output reports/clang-tidy-custom
```

The benchmark writes raw logs and JSON to a new output directory. Per-check
seconds sum work across translation units; they are not elapsed time.

### SonarCloud

The `sonarcloud` job ([sonar.yml](../.github/workflows/sonar.yml)) gates the PR
against [sonar-project.properties](../sonar-project.properties). To reproduce
it locally on Linux, install the SonarSource build wrapper from
`https://sonarcloud.io/static/cpp/` and `sonar-scanner`, then mirror CI:

```sh
build-wrapper-linux-x86-64 --out-dir bw-output \
    bazel build --config=sonar --spawn_strategy=local --strategy=Genrule=local //...
bazel shutdown   # the wrapper's interceptor stays loaded in the Bazel server
BAZEL_TEST_CONFIG=sonar scripts/coverage-local.sh
sonar-scanner -Dsonar.token=$SONAR_TOKEN
```

The token is a personal one (SonarCloud → My Account → Security), not the CI
secret. On macOS use the `build-wrapper-macosx-x86` package.

## Mutation testing

The PR `mutation` job mutates changed portable lines and never fails a PR
([ADR 0022](adr/0022-advisory-mutation-testing-with-mull.md)). Locally, put
Mull's `mull-runner-<N>` and its LLVM's `clang` first on `PATH` (set
`MULL_IR_FRONTEND` if the plugin is not beside the runner):

```sh
python3 scripts/mutation_test.py --scope //src/algorithms/checksum/...
python3 scripts/mutation_test.py --diff-ref origin/master   # changed lines only
```

A test-only change touches no mutable lines, so the PR job has nothing to
mutate. Run the workflow scoped on the branch and compare its survivors with
the same scope on `master`:

```sh
gh workflow run mutation -r <branch> -f scope=//src/algorithms/protocol/...
```

The `merged report` job's summary and `mutation-report` artifact hold the
result.

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
