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

## Qualification boundary

These local process timings measure passing tests, not a full Mull run. The
host has no compatible pinned Mull installation. They do not establish an
end-to-end CI speedup, unchanged mutant verdicts, or the 20-minute workflow
target. Before refreshing the schedule, run the identical mutation scope at
base and head and compare mutant identities, direct kills, survivors and
timeouts using the [mutation performance loop](../tooling.md#mutation-performance-loop).
