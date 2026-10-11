# Repository-specific inline clang-tidy checks

Status: design agreed; implementation and validation in progress.

## Agreed scope

- Audit all first-party C++ code, including production code, tests, and
  application headers.
- Focus the check series on repository-specific conventions that cannot be
  expressed with built-in clang-tidy checks.
- Design checks defined inline in the clang-tidy configuration and a method
  for measuring their execution time.
- Continue through implementation and actual profiling after design agreement.
- Emit warnings for the new checks and explicitly exclude each from
  `WarningsAsErrors`; existing checks retain their current error policy.
- Restrict diagnostics to high-confidence violations. Ambiguous cases remain
  audit findings rather than warnings.
- Leave correction of existing violations to a separate effort.
- Measure both per-check execution costs and total analysis slowdown with
  repeated baseline-versus-enabled runs on identical translation units and
  fixed concurrency.
- Run performance measurements on the local machine only, recording the
  host, LLVM version, and measurement conditions.
- Use the full locally buildable scope, one warm-up, and three paired
  baseline/enabled measurements.
- Present measured costs without a slowdown acceptance threshold.
- Run custom checks in a separate advisory pass covering all first-party
  headers; retain the existing built-in enforcement scope. Measure the
  additional parsing cost as part of enabled-versus-baseline elapsed time.

The [coding guide](../../coding-style.md) owns the conventions and current
enforcement policy. Its existing-debt policy permits opportunistic conversion
rather than repository-wide cleanup, except for naming. It also explicitly
describes most conventions as review-enforced by design. Expanding mechanical
enforcement requires updating that description. Warning-only checks preserve
the existing opportunistic cleanup policy; they do not impose a cleanup sweep.

## Integration design

Use a small adapter around LLVM's installed parallel script to insert the
experimental activation flag in each clang-tidy invocation. Preserve upstream
scheduling, profile aggregation, nested configuration discovery, and exit
status. Reject an incompatible upstream invocation API with an actionable
error instead of silently omitting the advisory pass.

Enable only the registered custom-check names in the advisory pass. Keep the
root built-in check list and header filter unchanged, and explicitly exclude
each new check from warnings-as-errors. Show exported warnings on successful
analysis and distinguish findings from failures.

Fixture validation uses the actual configuration and pinned GoogleTest
dependency. It must cover direct versus nested assertion operands, optional
values, formatted versus concatenated messages, nested path construction,
user-file locations, advisory severity, and absence of fix-its. The runner
tests must cover Windows separator spellings and existing failure behavior.

## Agreed first series

Complete positive/negative fixtures must verify the following agreed series:

1. Boolean GoogleTest assertions on `Result`/`Status` `.has_value()`, excluding
   ordinary optional values and the low-level Result API tests.
2. Boolean GoogleTest assertions directly reducing container predicates through
   standard or ranges `all_of`, `any_of`, and `none_of`.
3. String concatenation in the message argument of specifically identified
   error factories or logging sinks, excluding path joins and Qt strings.

Scratch probes using installed LLVM 23.1.0 and the repository's GoogleTest
headers verified basic macro-site diagnostics and type distinctions. These
probes establish feasibility, not complete correctness or performance.

Minimum-scope analysis, string borrowing semantics, and active loop trace
coverage require reasoning beyond straightforward inline patterns and remain
deferred candidates. Integer duration parameters and GoogleTest suite suffixes
need additional exception analysis before inclusion.

## Integration findings

The [LLVM inline-check documentation](https://releases.llvm.org/23.1.0/tools/clang/tools/extra/docs/clang-tidy/QueryBasedCustomChecks.html)
describes experimental activation and diagnostic-only checks. YAML uses
`CustomChecks` with `Name`, `Query`, and `Diagnostic`; registered names receive
the `custom-` prefix. Enablement and explicit warnings-as-errors exclusions
must use the registered names.

There are two runners: FastECU's orchestration script
[clang_tidy_runner.py](../../../scripts/clang_tidy_runner.py) prepares Bazel
compilation commands and invokes LLVM's `run-clang-tidy` script. The LLVM script
runs the `clang-tidy` executable across translation units in parallel.

The installed LLVM 23.1.0 parallel runner does not forward the activation flag.
Its `-extra-arg` option appends compiler arguments, not clang-tidy options. A
minimal local probe using `-extra-arg=--experimental-custom-checks` failed with
`unknown argument: '--experimental-custom-checks'` and exit code 1. It cannot
activate the custom-check module.
The FastECU runner also suppresses successful analysis output and prints
zero findings even when exported warnings exist. Implementation must activate
the checks and make successful advisory findings visible without changing
existing failure behavior.

## Preliminary observations

These are sample findings, not a completed repository audit:

- [SSM identification](../../../src/backend/diagnostics/ssm_identify.cpp)
  constructs an error message using string concatenation.
- [SH7058 K-line tests](../../../src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor_test.cpp)
  use bare Result success assertions and contain an assertion loop without
  a trace.
- [M32R CAN tests](../../../src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp)
  reduce a container predicate to a boolean assertion.

The current [configuration](../../../.clang-tidy) treats warnings as errors.
The [runner](../../../scripts/clang_tidy_runner.py) expands configuration
changes to a full-scope analysis and already supports profiling. Its header
filter does not cover all first-party header locations. Inline custom-check
activation and reliable macro diagnostics need feasibility verification before
committing to any matcher design.
