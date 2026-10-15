# Fuzz and mutation testing

Proposed design, agreed in a design review on 2026-10-15. This is context for
the delivery PRs below, not authorization to implement them out of order.
Conventions that endure move to [coding style](../../coding-style.md#tests) and
the two ADRs named below as each PR lands; this spec then leaves the checkout.

## Goals

- **Fuzzing** hardens parsing of untrusted input against crashes, undefined
  behavior, out-of-bounds access and hangs: ECU replies on the wire, ROM files,
  definition XML, and user- or definition-supplied conversion expressions and
  terminal scripts.
- **Mutation testing** measures whether the tests guarding hardware-facing
  policy (address-window guards, checksums, protocol framing) would notice a
  wrong change. It is advisory; it never blocks a merge.

## Scope

Both techniques cover portable code only: `src/algorithms` and `src/backend`.
Desktop and platform adapters (serial, the J2534 bridge protocol) are later
candidates and need their own decision.

## Fuzzing

**Engine.** FuzzTest (`bazel_dep(name = "fuzztest")`). Harnesses are
`FUZZ_TEST` properties in GoogleTest style, using typed domains rather than raw
byte buffers where the input has structure. Recorded in ADR 0021.

**Harness layout.** One `<unit>_fuzz_test.cpp` beside the unit under test, owned
by the same package. A `fastecu_portable_fuzz_test` macro in
`//bazel:gtest_targets.bzl` adds the FuzzTest dependencies and main, tags the
target `fuzz`, and restricts it to Clang builds; Windows never builds it.
Gazelle emits the macro for the `_fuzz_test.cpp` suffix. If `gazelle_cc` cannot
select a kind by suffix, settle the workaround in PR 1 and record it in coding
style.

**Seeds.** Synthetic inputs only, derived from existing test fixtures, stored as
files under `<package>/testdata/fuzz/<harness>/` and loaded with
`fuzztest::ReadFilesFromDirectory`. Stock ROM bytes and bench reads are never
committed. A prek hook rejects seed files over 4 KiB; a legitimately larger
synthetic seed needs an explicit allowlist entry in that hook. Real ROMs may be
used for local fuzzing from a gitignored directory.

**PR gate.** A Linux PR job using the clang-tidy job's pinned LLVM (currently
23.1.2) runs `--config=asan` (ASan + UBSan) over all portable tests and the
`fuzz`-tagged harnesses. Harnesses run in FuzzTest's unit-test mode, so every
committed seed and minimized crash input is replayed under sanitizers on every
PR. The main release jobs skip `fuzz` targets.

**Long runs.** A workflow on a weekly schedule and `workflow_dispatch` runs each
harness under `--config=fuzztest` for about 10 minutes. Crashing inputs are
uploaded as artifacts. A maintainer minimizes each one, commits it as a seed
(which makes it a regression test through the PR gate) and fixes the defect.

**Harness order.**

1. ECU reply decoders: SSM, UDS, MUT/DMA, BIU, Colt (`algorithms/protocol`).
2. ROM file to memory map loading (`algorithms/memory`, the memory map spec in
   `backend/config`).
3. Conversion expressions (`algorithms/expression`).
4. Definition and catalog XML (`backend/definition`, `backend/config`).
5. DTC and NRC parsers; the terminal script parser.
6. Checksum and crypto routines over arbitrary-length images.

## Mutation testing

**Tool.** Mull with its matching LLVM 19 toolchain (`mull-19` from apt in CI,
`mull-project/mull/mull@19` and `llvm@19` from Homebrew locally). This is a
second Clang pin, separate from LLVM 23 used for fuzzing, sanitizers and
clang-tidy. Recorded in ADR 0022. Viability depends on Clang 19 building the
C++23 portable tree against the CI runner's libstdc++; if it cannot, revisit
the tool choice rather than lowering the language standard.

**Mutants.** Mull's default operator set over `src/algorithms` and
`src/backend`, excluding `*_test.cpp`, `testing/` helper packages and generated
sources. Widen the operator set only after the baseline report shows where the
noise is.

**Runs.**

- Weekly full run, publishing the report as an artifact.
- Advisory PR job in incremental mode (`gitDiffRef` against `origin/master`)
  when portable code changes. Surviving mutants appear in the job summary and as
  `::warning` annotations on the changed lines. It needs no write permissions
  and never fails the build.
- A local wrapper script for either mode.

A mutation-score ratchet on these packages is a possible later step once the
baseline is stable; it is not part of this design.

## Delivery

1. FuzzTest and sanitizer infrastructure: module dependency, `.bazelrc`
   configs, the macro and Gazelle mapping, the seed-size hook, the asan PR job,
   the first two harnesses (one SSM or UDS reply decoder and the expression
   parser), ADR 0021 and the coding-style section.
2. Weekly and manually dispatched fuzz workflow.
3. Mull spike on `algorithms/checksum` with Clang 19: a go/no-go decision. Runs
   in parallel with PR 1.
4. Mull configs, local wrapper, weekly and PR-diff jobs, ADR 0022.
5. Remaining harnesses in the order above, one PR per package.

On macOS (Apple Silicon), fuzzing with Homebrew `llvm` and Mull with `llvm@19`
are best-effort local workflows, not CI gates.
