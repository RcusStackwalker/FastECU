# ADR 0022: Advisory Mutation Testing with Mull

## Status

Accepted and implemented by `scripts/mutation_test.py`, `mull.yml`,
`bazel/mutation.bazelrc`, the weekly `mutation` workflow, and the PR
workflow's `mutation` job.

## Context

Coverage shows which lines the tests execute, not whether a test would notice
those lines being wrong. Address-window guards, checksums and protocol framing
are where an unnoticed wrong change can brick an ECU, and the
[coding style](../coding-style.md#tests) already asks for a manual mutation
check when correcting them without hardware. A run over `algorithms/checksum`
(scores of 39–61% per test binary, 140 surviving mutants) showed the gap is
real.

## Decision

- Mull mutates the portable code, `src/algorithms` and `src/backend`,
  excluding tests, `testing/` helpers and external code (`mull.yml`).
- Mull's plugin must match the compiler that loads it, so mutation builds use
  the LLVM that Mull's release packages: Ubuntu's `clang-22` with Mull 0.34.1
  in CI (`.github/actions/mull-linux`). This is a second LLVM, separate from
  the pinned LLVM 23 used for clang-tidy, sanitizers and fuzzing.
- `scripts/mutation_test.py` builds the selected tests once
  (`--config=mutation`, with coverage so a binary's unexecuted code is reported
  as not covered rather than surviving), runs `mull-runner` on each, and merges
  the per-binary reports: a mutant is killed when any test binary kills it.
  GoogleTest stops after the first failing case for a mutant: subsequent
  cases cannot strengthen that verdict. Passing baseline runs execute every
  selected case.
- It is advisory. A weekly run covers all portable code and publishes its
  reports; a PR job mutates only the changed lines (`gitDiffRef`, applied by
  the runner so the build stays the same in every mode) and reports survivors
  as annotations on the diff and a job summary. Surviving mutants never fail
  either; a tool or build failure does.
- The weekly and manual runs use four jobs, balancing work longest-first
  from the versioned [duration estimates](../../bazel/mutation-schedule.json).
  Expensive binaries use GoogleTest's disjoint test shards: every test case
  still runs, while Mull's coverage filter limits each slice's mutants.
  Each job computes the same assignment without a planning job and builds
  only the binaries it needs. Missing estimates use a deterministic default;
  estimates never define which Bazel tests are selected.
  Each shard writes a manifest after its last binary, and a merge job that
  runs even when a shard fails merges every report it finds, names the shards
  that left no manifest in the summary, and fails the run if there were any.
  Completed manifests must also agree on the complete selection and account
  for every work item exactly once. Per-work-item report directories prevent
  slices or identically named binaries from overwriting one another.
  The PR job, usually a few minutes, stays one job.

## Alternatives

- Dextool mutate or mutate_cpp: source-level mutators that rebuild per mutant,
  far slower on a Bazel tree, and Dextool is no longer actively maintained.
- Mull on the project's LLVM 23: Mull ships no release for it, and building
  Mull from source would make the project maintain its toolchain.
- A mutation-score gate: premature until the baseline is known and the
  operator noise is understood. It may follow as a ratchet on chosen packages.

## Consequences

- CI installs a second LLVM for these jobs, and a Mull bump may move it; the
  pin lives in one composite action.
- Mutation runs build every selected test a second time with the plugin and
  coverage. Mull reruns a whole test binary for every mutant it covers, so a
  binary with slow cases costs that time per mutant, and the slowest binary
  bounds a sharded run's wall time however many shards there are. Keep each
  binary's unmutated run short: `subaru_key_recovery_test` took about 33
  minutes until its attack stopped re-evaluating the f-function per pair and
  candidate.
- The [2026-10-17 baseline](https://github.com/RcusStackwalker/FastECU/actions/runs/39027359985)
  took 54m27s, with shard durations from 3m22s to 54m11s and one binary
  taking 26m20s. This supersedes the original choice to avoid timing-based
  balancing. The performance target is 20 minutes for the complete workflow;
  its diagnostic timeout stays longer so regressions retain complete evidence.
  The [tooling loop](../tooling.md#mutation-performance-loop) owns measurement,
  estimate refresh and quality comparison instructions.
- macOS runs are local and best-effort with Mull's macOS release and the
  LLVM it was built against.
