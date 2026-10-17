# Flash executor test profiling

The 2026-10-17 investigation used
`//src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test` at baseline
`ca3ab375`. In the supplied [NLOC-loop job](https://github.com/RcusStackwalker/FastECU/actions/runs/39068581021/job/117501427238),
its slices 3 and 0 consumed 400.4 and 249.1 seconds of Mull execution.

## Method

Build with `--config=mutation`, without the Mull plugin, to retain the same
`-O0`, coverage instrumentation and standard-library hardening settings. Archive
the executable before changing sources, then compare fresh processes on the
same host with captured stdout and `LLVM_PROFILE_FILE` pointing outside the
checkout. Measure the full suite and each existing slice separately:

```sh
bazel test --config=mutation \
  //src/backend/flash/ecu:subaru_denso_1n83m_4m_can_executor_test \
  --nocache_test_results --test_output=all

GTEST_TOTAL_SHARDS=4 GTEST_SHARD_INDEX=0 LLVM_PROFILE_FILE=/tmp/executor.profraw \
  bazel-bin/src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor_test
```

Repeat each measurement three times without competing builds or tests. Use
median process wall time, including fixture setup. Measure all four slices;
changing case registration changes their membership.

On macOS, sample a repeated run with `sample <pid> 8 1 -file /tmp/executor.sample.txt`.
On Linux, use a CPU profiler against the same build settings. Separate fixture
generation, executor work and assertion costs before choosing an optimization.

## Findings and retained changes

CPU samples identified payload transformation, per-byte Google Mock `Each`
assertions, and scripted transfer construction/replay as the main costs.
Replacing the payload range pipeline with an explicit loop did not measurably
improve the suite and was discarded; production sources remain unchanged.

Uniform ROM assertions now use the full-range
[`BytesFilledWith` matcher](../../src/algorithms/protocol/testing/byte_matchers.h).
The shared dump helper builds its identical reply once; scripted CAN writes
compare the incoming view directly without allocating a copy. Mismatch
diagnostics and strict request matching remain intact.

The four Denso ISO-15765 suites expose each checksum outcome as a separate
parameterized test. This lets the existing GoogleTest sharding distribute
whole-image transfers and lets Mull's `--gtest_fail_fast` stop between outcomes.
Their immutable ROM/wire fixtures are reused across plain and parameterized
suites within a process. Full-size transfers, every checked byte, timing
assertions and frozen first/last write requests are retained.

## Local passing-test measurements

On the same Apple Silicon macOS host, using Apple clang 21.0.0 and Bazel
9.1.1, median process wall times over three fresh runs were:

| Execution | Baseline seconds | Candidate seconds |
| --- | ---: | ---: |
| Full suite | 7.587 | 5.730 |
| Slice 0/4 | 2.029 | 2.297 |
| Slice 1/4 | 0.680 | 0.745 |
| Slice 2/4 | 1.978 | 1.857 |
| Slice 3/4 | 3.598 | 1.550 |
| Slowest slice | 3.598 | 2.297 |

The full suite is 24% faster and the slowest passing slice is 36% faster.
Individual slice membership changes when the five checksum outcomes become
separate cases; compare the whole suite and the slowest slice, rather than
expecting each slice index to improve.

## Scoped Mull comparison

The [baseline run](https://github.com/RcusStackwalker/FastECU/actions/runs/39071197809)
at `ca3ab375` and [candidate run](https://github.com/RcusStackwalker/FastECU/actions/runs/39071413017)
at `ba74ebca` both completed successfully. Both used the single executor-test
target above, identical production sources, Mull 0.34.1/LLVM 22, `ubuntu-26.04`,
four GoogleTest slices and six workflow jobs.

| Measurement | Baseline | Candidate |
| --- | ---: | ---: |
| Workflow creation through merged-report completion | 8m 11s | 5m 45s |
| Initial runner queue delay | 3s | 3s |
| Aggregate Mull execution | 905.6s | 753.5s |
| Slowest Mull slice | 405.5s | 241.7s |
| Slice 0/4 | 250.4s | 180.6s |
| Slice 1/4 | 88.3s | 111.7s |
| Slice 2/4 | 161.4s | 241.7s |
| Slice 3/4 | 405.5s | 219.6s |

The merged verdict maps are identical: **136 mutant identities, 103 direct
kills, 3 timeouts, 30 survivors and no uncovered mutants**. There are no missing
or added identities, lost direct kills, additional survivors or new timeouts.
The target's [schedule estimates](../../bazel/mutation-schedule.json) were
refreshed from the complete candidate artifacts; other targets' estimates
remain unchanged.

This is one observed scoped comparison: 17% less aggregate Mull execution and
30% less workflow wall time. Runner CPU models varied. Slices 1 and 3 used AMD
EPYC 7763 in both runs; slice 2 used EPYC 9V74 in both. Slice 0 changed from
EPYC 7763 to EPYC 9V45, so its improvement cannot be attributed wholly to code.
The formerly slowest slice 3 improved 46% on the matching CPU model. Build/query
times are recorded in each artifact's `_performance.json`, and Bazel cache-hit
details remain in the job logs; these also contribute to workflow wall time.

The [mutation performance loop](../tooling.md#mutation-performance-loop) still
requires three complete all-portable runs to qualify the 20-minute workflow
target. This scoped pair does not establish that broader qualification or
hardware qualification.
