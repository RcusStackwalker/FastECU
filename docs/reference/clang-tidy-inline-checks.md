# Inline clang-tidy check evidence

The [coding guide](../coding-style.md#static-analysis) owns analysis commands
and enforcement policy. The [configuration](../../.clang-tidy) owns the active
inline check definitions. These checks report advisory warnings and provide
no automatic replacements. Existing source violations remain separate cleanup
work.

## Scope and precision

The first series recognizes boolean GoogleTest assertions on FastECU Results,
boolean container-predicate assertions, and direct concatenation supplied to
specific message sinks (`fastecu::Fail` and `IEventSink::Log`). It does not infer
message intent for arbitrary functions, follow locals across statements, or
prove ownership, lifetime, minimum-scope, or loop-trace conventions.

Result assertions distinguish `std::expected<T, fastecu::Error>` from plain
`std::optional` and exclude the low-level Result API suite. Assertion checks
inspect GoogleTest's direct boolean operand, optionally negated once. They
exclude compound expressions and predicates nested in unrelated calls or
aggregates, while retaining assertions inside callbacks. Message checks follow
transparent temporary/conversion wrappers rather
than searching arbitrary nested function arguments; path construction nested
inside another call is outside their scope. Directly formatted messages are
permitted; appending text to a formatted result remains concatenation.

The advisory header filter includes first-party source, application, test, and
script headers, with both native and normalized separators. Analysis still
depends on the translation units buildable on the host and the headers they
include. A macOS scan does not establish coverage of Windows-exclusive code
or headers unused by any analyzed translation unit.

## Validation

The [fixture harness](../../scripts/clang_tidy_checks_test.py) compiles small
GoogleTest suites against the actual Bazel compilation flags and dependencies,
then compares exported diagnostic names and source locations to independently
marked expectations. It checks warning severity and absence of replacements.

The [adapter tests](../../scripts/clang_tidy_adapter_test.py) verify experimental
activation, preserved upstream arguments/exit status, and explicit rejection
of an incompatible upstream API. The [runner tests](../../scripts/clang_tidy_runner_test.py)
verify successful warning visibility, separate advisory scope, Windows path
spellings, and baseline preparation reuse.

## Additional audit samples, 2026-10-10

These source observations are outside the first check series. They remain
cleanup work and do not justify broad syntactic warnings without further
semantic evidence.

| Convention | Observed deviation | Why deferred |
| --- | --- | --- |
| Non-owning string parameters use `std::string_view` | `LineError` in [terminal scripts](../../src/backend/diagnostics/terminal_script.cpp) takes `const std::string&` solely for formatting | A reference parameter alone does not prove borrowing behavior |
| A local used only by one `if` belongs in its initializer | `ParseDelay` in [terminal scripts](../../src/backend/diagnostics/terminal_script.cpp) declares `all_digits` separately before its sole `if` use | General scope/lifetime proof exceeds the intended inline pattern checks |
| Duration units belong in types | `CanRawExchange` in [EEPROM CAN execution](../../src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp) accepts integer `delay_ms` and converts it to a duration | A name suffix alone cannot distinguish durations from serialized numeric fields |
| Assertion loops need a trace or parameterized suite | [SH7058 K-line tests](../../src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor_test.cpp) loop over bootloader states with assertions but no trace | Absence of a trace descendant does not prove absence of an active trace |

## Local measurement

On 2026-10-10, all 518 locally buildable translation units were measured on an
Apple M5 Pro with 24 GiB RAM, macOS 27.0.1, and Homebrew LLVM 23.1.0. Every
final run used 15 workers. This is local evidence; CI pins LLVM 23.1.2 and its
Linux/Windows results are separate qualification.

Both modes were warmed before three measured pairs. Pair order alternated;
every run used the same compilation database, source/configuration
fingerprints, and enabled profiling. Build and database preparation took
1.03 and 1.20 seconds in the initial warm-up and were not repeated in measured
analysis. Warm-up analysis took 890.40 seconds for baseline and 830.79 seconds
for enabled mode, including 72.02 seconds for the advisory pass.

| Pair | Order | Baseline analysis (s) | Enabled analysis (s) | Advisory pass (s) | Elapsed increase |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | Baseline, enabled | 726.42 | 789.42 | 65.72 | 8.67% |
| 2 | Enabled, baseline | 719.74 | 853.54 | 65.74 | 18.59% |
| 3 | Baseline, enabled | 720.02 | 801.02 | 71.83 | 11.25% |
| Median | | 720.02 | 801.02 | 65.74 | 11.25% |

Enabled elapsed time includes both passes. The percentage on the median row
compares the two mode medians. Built-in analysis varied from 723.70 to 787.80
seconds in enabled runs; the advisory pass varied from 65.72 to 71.83 seconds.
The difference between whole-run medians includes that host/run variation,
so it is not a precise attribution of all 81.00 added seconds to the new
matchers. No runtime acceptance threshold was applied.

| Check | Distinct advisory sites | Median check wall seconds, summed across translation units |
| --- | ---: | ---: |
| Result assertions | 893 | 1.8544 |
| Container assertions | 31 | 1.7678 |
| Message concatenation | 20 | 7.9478 |

All three enabled measurements reported the same 944 distinct sites and
exited successfully. Summed per-check timings are not elapsed pass time; the
separate pass also parses every translation unit and runs the tool for each.
The [machine-readable evidence](clang-tidy-inline-checks-2026-10-10.json)
retains individual runs, warm-ups, check costs, and provenance digests. Raw
logs, full input fingerprints, and the source-location list remain in the
ignored output directory recorded by that evidence.

Representative findings include the bare Result assertions in
[SH7058 K-line tests](../../src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor_test.cpp),
the boolean container predicate in
[M32R CAN tests](../../src/backend/flash/ecu/subaru_hitachi_m32r_can_executor_test.cpp),
and the concatenated error context in
[SSM identification](../../src/backend/diagnostics/ssm_identify.cpp).
No source cleanup was applied.

A preliminary four-worker baseline warm-up took 1280.85 seconds. That
configuration was abandoned to shorten the protocol; its timing is excluded
from the 15-worker comparison. Earlier interrupted warm-ups during fixture
development were also excluded.

Validation at delivery: the release application build passed, 269 Bazel
tests passed with seven Windows-only tests skipped, all five real compiler
fixture suites passed, and formatting, links, and Gazelle checks passed.
