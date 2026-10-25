# Tooling

How to run the static-analysis, mutation and fuzzing checks and the Sonar
dedup loop locally, and how CI runs them. The rules they enforce live in the [coding style guide](coding-style.md).

## Build code generation

[Bazel configuration](../.bazelrc) enables `--force_pic` only in the Linux and
macOS configurations. A global build option overrides platform options expanded
from common configuration, so a global flag with a Windows opt-out is insufficient.
On Linux this replaces the raw `-fPIC` flag: Bazel
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
bazel run //:clang_tidy                         # all host-eligible configured sources
bazel run //:clang_tidy -- //src/algorithms/...  # selected Bazel targets and dependencies
bazel run //:clang_tidy -- --profile             # one recorded per-check timing table
bazel run //:clang_tidy -- --fix                 # apply and verify fixes (macOS/Linux)
```

The Bazel aspect runs built-in and repository checks together, once per
configured source. Source files, transitive headers, the governing configuration
chain, selected executable, repository-check artifact and LLVM resource headers
are action inputs. Bazel reuses unchanged results; editing a child configuration
invalidates only the sources that inherit it. Generated sources and external
repositories are excluded, while generated headers remain available through the
configured C++ graph. The aspect follows C++ dependencies and forwarding rules,
including the Windows x86 transition, and preserves distinct configurations.

The wrapper reads declared human reports, exit statuses and replacements from
Bazel's build events, downloading those outputs when a remote cache is used.
It deduplicates findings across translation units and prints their snippets and
notes. `--annotate` adds GitHub annotations; CI enables it. GitHub displays at
most ten error annotations per step, so the job log carries the full list.
Findings, missing outputs, empty scope, failed tools and failed builds return
nonzero status.

`--profile` displays one table containing every recorded check, slowest first.
CI always displays it and adds it to the job summary. Values are recorded wall
seconds summed across the selected translation units, including cached results;
samples can span runs and machines. They measure recorded check work, rather
than the current invocation's elapsed time. Missing profiles are reported
explicitly. Requesting the table does not force analysis to rerun.

`--fix` applies at most three batches of exported replacements, reanalyzing
after every batch, including the last. After successful applications, an ordinary
build verifies the selected targets with the same configuration, even when
findings remain. Residual findings and compilation failures fail the command.
Replacement paths are localized to the checkout; replacement text is preserved.
Windows fix mode is unsupported.

Nested builds default to `--config=release`. Pass additional build flags with
`--bazel-arg`, for example `--bazel-arg=--compilation_mode=dbg`; the same flags govern
linting and post-fix compilation. Compilation-database generation remains an
independent SonarCloud/editor workflow.

The existing blocking `clang-tidy` CI jobs run on Linux and Windows, with the
pinned LLVM installation and repository-check fixtures. Linux analyzes its
host-eligible graph. Windows reports the source prefixes selected by
`--scope-os windows` from the [scope manifest](../.clang-tidy-scope.toml).
Every source-owning package with platform selection must be covered by that
manifest. The scope scanner excludes hidden checkouts, symlinks and junctions.
Local macOS is supported without a separate macOS analysis CI job.

The [qualification record](reference/clang-tidy-aspect-qualification.md) owns
source/configuration and seeded-finding parity, cache and fix evidence, and
installation boundaries. Before delivering a check or configuration change,
run the full analysis and fix its findings; fixtures alone do not establish a
clean production scope. The default graph's fuzz-harness coverage gap remains
[separate work](tech-debt.md#p2-analyze-fuzz-harnesses-with-clang-tidy).

#### Repository checks

Repository checks are compiled in
[`tools/clang-tidy/plugin/`](../tools/clang-tidy/plugin/); the enabled
`fastecu-*` names live in [`.clang-tidy`](../.clang-tidy). Bazel builds the
analysis artifact automatically for analysis, fixes and fixture
commands. The wrapper validates the selected LLVM installation, release and
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
bazel run --config=release //:clang_tidy
```

On Windows, also set `BAZEL_LLVM` to the same root and `USE_CLANG_CL=1`
for analysis builds. The pinned compiler builds against its own development
headers while retaining warnings as errors in first-party code.

macOS/Linux load a shared extension. The pinned Windows executable does not
export the required symbols, so Bazel builds an analysis host with the checks
linked into it and supplies its missing compression dependencies. The
[packaging smoke test](../tools/clang-tidy/plugin/smoke/README.md)
checks that a host loads the extension. Ordinary application builds do not
need LLVM development headers.

Built-in and repository checks run together with the wider first-party header
scope under `src/`, `apps/`, `tests/`, `scripts/` and `tools/`, including `.hpp`.
Generated headers retain their exclusions; vendor headers stay outside the scope.

```sh
# Required real-dependency positive and permitted-counterexample fixtures.
# No application build or compilation database is needed.
bazel test --config=release //tools/clang-tidy/scripts:clang_tidy_check_fixtures
# Select individual fixtures with lit's filter.
bazel test --config=release //tools/clang-tidy/scripts:clang_tidy_check_fixtures \
  --test_arg=--filter --test_arg=template-type
```

The [module test suite](../tools/clang-tidy/plugin/test/) follows
[LLVM's in-tree test conventions](https://clang.llvm.org/extra/clang-tidy/Contributing.html#testing-checks):
standalone source fixtures carry `RUN`, `CHECK-MESSAGES` and `CHECK-FIXES`
directives, interpreted by lit, FileCheck and the unmodified upstream
`check_clang_tidy.py`. Diagnostic expectations also reject unexpected warnings
and errors; checks without fixes must leave their source unchanged. Exception
usage and first-party/generated-header filtering are exercised by explicit
`RUN` commands. The C++ standard-library and GoogleTest headers are real,
with FastECU's transitive headers supplied by Bazel rather than a compilation
database. The lit configuration discovers the selected compiler's standard
include paths because LLVM's helper disables automatic C++ header lookup.
Use a temporary source name ending in `_test.cpp` (the fixtures use `%t_test`)
for checks restricted to test code. Keep helper options such as `-std` before
its positional arguments so its argument parser separates compiler flags correctly.

The lit package, helper and FileCheck driver are pinned to the analysis LLVM
release in [requirements.txt](../requirements.txt) and [MODULE.bazel](../MODULE.bazel).
Bazel compiles LLVM's FileCheck driver against the selected development
libraries because the official archives omit that executable. The macOS
archives contain LLVM bitcode, so linking FileCheck uses their matching LTO
library; Windows uses their static release CRT, as does the analysis host.
Fixture commands require Unix tools such as `cp`, and the upstream helper
requires `diff`. LLVM's lit initialization discovers these on Windows,
including Git for Windows installations registered outside `PATH`.

Fixtures require the registered checks and do not silently skip when an
extension is missing. They are a manual, local Bazel test because they require
the selected host LLVM installation; the analysis CI job runs them explicitly
with `bazel test` so unchanged successful results can be cached.
The analysis job also runs
`bazel test --config=release //tools/clang-tidy/aspect/tests:transition_outputs_test`.
This manual regression executes linting through a configured forwarding rule
and C++ dependency, reads its real status/profile/replacement outputs, and checks
32-bit pointer width on Windows. It catches configured sources omitted by
aspect traversal. Windows analysis uses Bazel's configured Git Bash and its
Unix utilities.

The `fastecu-*` checks have no exception list of their own: suppress a finding
with a `NOLINTBEGIN(check)`/`NOLINTEND(check)` block around the test, with the
reason after the colon, like any other clang-tidy finding. Name the check; a
bare `NOLINT` hides every check.

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
Use a simpler local proof or a `NOLINTBEGIN/NOLINTEND` block that says why where
needed. Unknown aliases and complex matcher compositions remain review
responsibilities. Single-use suggestions require an adjacent final block pair,
a void Result, and a pure `IsOk()` matcher to preserve cleanup ordering.
Uniform-buffer detection covers supported unsigned-byte vectors, spans and
arrays; custom projections and unrelated containers are permitted.

### SonarCloud

Python tooling follows the [test directory convention](coding-style.md#python-tooling-tests).
Sonar analyzes implementations as production sources and suites, test runners,
and plugin fixtures as tests. `//:sonar_scope_test` checks the declared scope
against package inventories and Python test entry points; run it with
`bazel test --config=release //:sonar_scope_test` after changing directories or
analysis settings. Python tooling coverage is not imported by the C++ coverage
pipeline, so `scripts/`, `tools/`, and `packaging/` are excluded from coverage
metrics while remaining in source analysis.

The `sonarcloud` job ([sonar.yml](../.github/workflows/sonar.yml)) gates the PR
against [sonar-project.properties](../sonar-project.properties). To reproduce
it locally, mirror CI. CI runs the scanner pinned in `MODULE.bazel` through Bazel
(`bazel run --config=coverage //tools/sonar:scan`), but that fetches the linux-x64
scanner and works on Linux only; on macOS use a locally installed `sonar-scanner`
for the last step:

```sh
# macOS only: Xcode's LLVM tools are not on PATH.
MAC="--repo_env=GCOV=$(xcrun -f llvm-profdata) --repo_env=BAZEL_LLVM_COV=$(xcrun -f llvm-cov)"
bazel coverage --config=coverage $MAC //...
bazel run --config=coverage $MAC //tools/sonar:coverage_report -- \
  --lcov "$(bazel info --config=coverage $MAC output_path)/_coverage/_coverage_report.dat" \
  --root "$PWD" --output-dir coverage
bazel run --config=sonar_inputs //:sonar_inputs
# Linux:
bazel run --config=coverage //tools/sonar:scan -- -Dsonar.token=$SONAR_TOKEN
# macOS:
sonar-scanner -Dsonar.token=$SONAR_TOKEN
```

The token is a personal one (SonarCloud → My Account → Security), not the CI
secret. Sonar reads its C++ compile commands from `sonar-inputs/compile_commands.json`,
which a Bazel aspect writes from the real configured compiler actions and the
generated headers they need, without compiling application code. Preparation fails
if any input is missing or if Qt's `qobjectdefs.h` does not resolve through the
database: without it Sonar misreads `emit` as a declaration and raises
`cpp:S1117`/`cpp:S824` false positives. Hedron's `compile_commands.json` remains
for editors and clangd only.

In CI one job runs `bazel coverage`, converts the combined lcov report to Sonar's
generic coverage format, prepares the inputs and scans; a transient scanner
failure is retried once. Test results and coverage come from the BuildBuddy
cache, so a rerun or an unrelated commit reruns only the affected tests.
[ADR 0027](adr/0027-use-native-bazel-coverage-for-sonar.md) records why; the
[qualification record](reference/sonar-aspect-qualification.md) holds the evidence
behind the aspect-based scanner inputs.

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

## Sonar dedup loop

The decision is [ADR 0029](adr/0029-lower-nloc-with-sonar-nominated-right-sized-abstractions.md);
the design is the [spec](superpowers/specs/2026-10-25-sonar-dedup-loop-design.md).
SonarCloud's duplication groups nominate where to look, and first-party NLOC
measures each step.

An agent runs it with `/goal run /sonar-dedup-loop iterations until it prints
DONE` (or `/loop /sonar-dedup-loop`): each iteration of the
[project skill](../.claude/skills/sonar-dedup-loop/SKILL.md) settles the open
`sonar-dedup` PR or opens the next one.

```bash
python3 scripts/dedup_loop.py measure            # total, production and test NLOC
python3 scripts/dedup_loop.py measure --json
python3 scripts/dedup_loop.py candidates         # ranked duplication groups from SonarCloud
python3 scripts/dedup_loop.py fresh              # exit 0 when master's analysis is of origin/master
python3 scripts/dedup_loop.py ledger record <id> unsuitable --members-json '[["path", "fn"]]' --reason "..."
python3 scripts/dedup_loop.py ledger list        # judgments, attempts, and the failure streak
python3 scripts/dedup_loop.py trend add --pr <n> --before <NLOC> --dup-before <lines>
python3 scripts/dedup_loop.py trend fill-after   # master's duplicated_lines after the last merge
python3 scripts/dedup_loop.py verdict --pr <n> --nloc-before <N> --nloc-after <N> \
    --changed-file <names> --mutation-reports <dir>
```

NLOC is Lizard's count over `src/`, `apps/` and `tests/`, from the manual-stage
`nloc` prek hook. `candidates` reads master's last SonarCloud analysis (public
API, no token), keys each group by the functions it occupies (the manual-stage
`nloc-functions` hook), drops blocks the checkout no longer has, and skips
groups the [ledger](reference/sonar-dedup-ledger.json) has settled or whose
files an open non-loop PR touches. Master is analysed only on release, so the
list can be stale. The [trend](reference/dedup-trend.csv) records NLOC and
master's duplicated lines per loop PR.

The `sonar-dedup verdict` job in `pr.yml` runs on PRs labelled `sonar-dedup`
after the SonarCloud and mutation jobs. It fails unless NLOC drops by at least
10 (a PR changing only the ledger and trend is exempt), the PR's SonarCloud
analysis reports no new duplicated and no new uncovered lines, and the
mutation job's reports hold no surviving mutant. It reuses those reports and
does not run Mull again. Mutation testing stays advisory for other PRs.
It judges with the base's copy of the script, refuses a SonarCloud analysis of
an older push, fails when either upstream job did not succeed, and fails a loop
PR that changes the loop's own tooling.

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
