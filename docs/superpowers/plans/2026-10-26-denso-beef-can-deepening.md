# Denso BEEF CAN Deepening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the broad Denso BEEF CAN callback/session scaffolding with one three-operation warm-kernel module, then give the petrol and diesel ECU cold path one fixed-order owner while preserving the TCU's distinct wire order.

**Architecture:** PR 1 introduces `BeefCanKernel`, which owns payload transfer, reads, and writes for petrol, diesel, and TCU callers. PR 2 introduces overloaded, family-typed `RunDensoBeefCanEcu` entry points backed by a single compile-time-specialized implementation; the named petrol/diesel executors remain lifecycle adapters, while the TCU keeps its cold path. The final graph has no behavioural callback bundle, no public per-command BEEF session, and no forwarding copy of the old template header.

**Tech Stack:** C++23, Qt-free portable backend ports, `fastecu::Result`/`Status`, GoogleTest scripted transports, Bazel/Gazelle, prek, clang-tidy, Mull, SonarCloud, Lizard NLOC.

**Spec:** [docs/superpowers/specs/2026-10-26-denso-beef-can-deepening-design.md](../specs/2026-10-26-denso-beef-can-deepening-design.md), [ADR 0029](../../adr/0029-lower-nloc-with-sonar-nominated-right-sized-abstractions.md), [ADR 0024](../../adr/0024-wire-transcripts-and-goldens-are-the-test-contract.md), and [ADR 0015](../../adr/0015-caller-owns-flash-transport-lifetime.md).

## Global Constraints

- Deliver exactly two dependent implementation PRs after this design/plan documentation branch is reviewed and merged. PR 1 must merge before PR 2 branches from updated `master`; neither implementation PR may rely on an unmerged companion to pass.
- Apply the `sonar-dedup` label to each PR. Each independently lowers first-party NLOC by at least 10, has no surviving changed-line mutant, no new duplicated or uncovered lines, and passes a fresh ADR 0029 right-sizing review.
- The planning baseline on 2026-10-26 is `152246` total NLOC (`67548` production, `84698` test). Re-measure the actual merge base at the start of each PR; do not use this historical number as the verdict input after `master` moves.
- Preserve every existing test case and assertion in both PRs. Tests may be renamed, moved, or re-expressed only through the new public owner. Do not expose a test-only production hook or retain a shallow public helper merely to avoid retargeting a test.
- Do not update scripted expected bytes, ordering, timeouts, delays, retry counts, progress, or golden results to make the refactor pass. Any mismatch stops the task for a wire-equivalence review.
- Preserve the three family upload policies exactly:
  - petrol: download policy `50 ms / 2000 ms / 3000 ms / 10 pending`, B6 reply timeout `2000 ms`;
  - diesel: download policy `50 ms / 10 ms / 3000 ms / 10 pending`, B6 reply timeout `500 ms`;
  - TCU: download policy `50 ms / 2000 ms / 3000 ms / 10 pending`, B6 reply timeout `2000 ms`.
- `BeefCanKernel::TransferPayload` stops after the final address-only B6 reply. Transfer exit, routine start, delays, ECU kernel-ID verification, and TCU restart/probe stay in their family cold/handoff paths.
- The kernel public interface remains exactly three protocol operations: `TransferPayload`, `Read`, and `Write`. Constructor injection and the pure `BeefKernelTransferPolicy` do not count as protocol operations.
- Profiles remain pure data. Do not add callbacks, operation selectors, runtime family enums, security algorithms, or state-machine switches.
- The TCU still rejects `TestWrite` during plan admission before transport I/O. `BeefCanKernel::Write` supports `TestWrite` only because the admitted petrol/diesel plans can request it.
- Backend code owns no thread, performs no filesystem I/O, uses no Qt, and lets the caller own transport lifetime. The TCU's existing protocol-owned `RestartIso15765` remains inside its handoff.
- All new BUILD ownership is generated with `python3 scripts/gazelle_check.py --fix`; review the generated `src/backend/flash/ecu/BUILD.bazel` diff and never hand-maintain a conflicting target.
- Stage named paths only. Leave unrelated `.clwb/` and any other pre-existing changes untouched.

## Review Focus

1. Kernel payload transfer: preserve padding, checksum complement, encryption, `34 04 33`, all 128-byte B6 blocks, the final address-only B6, each family's exact pending/read timing, and cancellation/disconnection behavior.
2. Read geometry: every family requests `00 + U24 address + U16 0400`, uses a 2000 ms BEEF timeout, appends only the requested tail on a partial final page, and never reads past the plan's region.
3. Write semantics: preserve 200 ms stale CRC drain, profile-specific flash-init timeout/casing, voltage/erase/write/commit timeouts, 0x200/0x1000 windows, ECU opcode `23` TestWrite validation, TCU TestWrite rejection, final compare, and recovery warnings after erase.
4. Cold-path separation: petrol/diesel retain tolerant `03` then `43`, family security, then `02`/`42`; TCU retains required `03`, its own security, `02`, proprietary five-attempt probe/fragment drain, restart, 500 ms delay, and strict post-upload probe.
5. Petrol-only behavior: stock, EcuTek, RaceRom, RaceRom-alt, and Cobb calculations remain compile-time family behavior; RaceRom-alt RAM reads and CAL-ID lookup cannot become reachable from diesel or TCU code.

---

## PR 1 — Deepen the BEEF Kernel Module

### Task 1: Establish the PR 1 evidence baseline

**Files:**

- Inspect only: `src/backend/flash/ecu/BUILD.bazel`
- Inspect only: the five existing BEEF test sources listed below
- Record in the PR description: NLOC JSON and the current named-test inventory

- [ ] **Step 1: After the design/plan documentation PR merges, create a feature branch from updated `master`** (or an isolated worktree when the execution workflow requires it):

```bash
git switch master
git pull --ff-only
git switch -c refactor/denso-beef-can-kernel
```

- [ ] **Step 2: Record the merge-base NLOC**:

```bash
python3 scripts/dedup_loop.py measure --json
```

Expected: one JSON object with `total`, `production`, and `test`. Save it in the draft PR body; PR 1 must finish at least 10 total NLOC lower.

- [ ] **Step 3: Inventory every current BEEF test case**:

```bash
rg -n '^TEST(_F|_P)?\(' \
  src/backend/flash/ecu/beef_can_session_test.cpp \
  src/backend/flash/ecu/denso_beef_can_common_test.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp \
  src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp
```

Expected: the inventory includes all current session/common tests and all family transcript tests. Paste the output or an immutable commit link into the PR description so moved cases can be audited without adding a repository snapshot.

- [ ] **Step 4: Run the existing focused baseline**:

```bash
bazel test --config=release \
  //src/backend/flash/ecu:beef_can_session_test \
  //src/backend/flash/ecu:denso_beef_can_common_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
```

Expected: all five targets pass before production edits. Stop and diagnose any baseline failure rather than attributing it to this refactor.

### Task 2: Introduce `BeefCanKernel` and move payload transfer behind it

**Files:**

- Create: `src/backend/flash/ecu/beef_can_kernel.h`
- Create: `src/backend/flash/ecu/beef_can_kernel.cpp`
- Create: `src/backend/flash/ecu/beef_can_kernel_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`
- Modify: `src/backend/flash/ecu/denso_beef_can_common.h`
- Modify: `src/backend/flash/ecu/denso_beef_can_common_test.cpp`
- Generated: `src/backend/flash/ecu/BUILD.bazel`

**Interface:**

```cpp
struct BeefKernelTransferPolicy
{
    uds::ExchangePolicy download_policy;
    std::chrono::milliseconds block_reply_timeout;
};

class BeefCanKernel
{
  public:
    BeefCanKernel(ICanFlashTransport& transport, IClock& clock,
                  const ICancellationToken& cancellation, IEventSink& events,
                  uds::IUdsChannel& channel,
                  const DensoBeefCanProfile& profile);

    Status TransferPayload(const KernelImage& kernel,
                           const BeefKernelTransferPolicy& policy);
    Result<bytes::Bytes> Read(const MemoryRegion& region,
                              PhaseReporter& progress);
    Status Write(const FlashPlan& plan, PhaseSequence& phases,
                 PhaseReporter& compare_progress);
};
```

The header declares all three final operations now; Tasks 3 and 4 fill the latter two. The implementation privately owns a `uds::UdsClient` built from its injected channel, clock, and event sink for request-download only. This preserves the approved constructor and keeps strict UDS pending semantics out of callers; the client is stateless and does not own the channel.

- [ ] **Step 1: Write failing public-interface transfer tests** in `beef_can_kernel_test.cpp`. Re-express, without deleting assertions, the current cases `KernelDownloadRequestEncodesItsTwo24BitFields`, `BeefKernelUploadPayloadNamesTheFamilyInBothSizeErrors`, `BeefKernelPayloadReplacesThePaddedLastWordWithTheChecksum`, and `UploadBeefKernelBlocksEndsWithAnAddressOnlyBlock`. Drive `TransferPayload` through a scripted channel/transport and assert literal traffic rather than calling a production encoder.

Add one table-driven policy case with the three exact policies from Global Constraints. For each row assert the request-download read timing and the B6 reply timeout. Include cancellation and disconnect outcomes during B6 reply discard.

- [ ] **Step 2: Confirm the new test is red because the owner does not exist**:

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release //src/backend/flash/ecu:beef_can_kernel_test
```

Expected: compilation fails on the missing/incomplete `BeefCanKernel` implementation, not on malformed test setup.

- [ ] **Step 3: Implement only `TransferPayload`**. Move payload validation, padding, checksum complement, encryption, download framing, UDS response validation, B6 sends/reply discard, and final address-only B6 into `beef_can_kernel.cpp`. Keep pure framing/checksum helpers in the anonymous namespace of that `.cpp`; do not publish a second helper interface.

Implementation invariants:

```text
kernel bytes -> pad to 128-byte boundary
             -> replace final word with 0x5AA5A55A - sum(previous BE words)
             -> Denso encrypt
             -> UDS 34 04 33 + U24(load) + U24(size)
             -> B6(address + each 128-byte block)
             -> B6(final address only)
```

Return before transfer exit. Check cancellation at the same boundaries as `UploadBeefKernelBlocks`; ignore nonterminal B6 content while still propagating cancellation/disconnection.

- [ ] **Step 4: Replace only the payload-transfer portion of all three `UploadKernel` functions** with `context.kernel.TransferPayload(kernel, policy)`. Leave each family's exit/start/handoff immediately after it. Add `BeefCanKernel kernel;` to each context and keep the current `uds::UdsClient` for cold-path requests.

- [ ] **Step 5: Remove the absorbed transfer helpers and their directly-coupled template tests** from `denso_beef_can_common.h` / `_test.cpp` only after all assertions live in `beef_can_kernel_test.cpp`. Do not leave wrappers named `BeefKernelUploadPayload`, `UploadBeefKernelBlocks`, or `DiscardB6Reply`.

- [ ] **Step 6: Regenerate and test**:

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release \
  //src/backend/flash/ecu:beef_can_kernel_test \
  //src/backend/flash/ecu:denso_beef_can_common_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
```

Expected: PASS with unchanged family upload transcripts.

- [ ] **Step 7: Commit the transfer slice**:

```bash
git add \
  src/backend/flash/ecu/beef_can_kernel.h \
  src/backend/flash/ecu/beef_can_kernel.cpp \
  src/backend/flash/ecu/beef_can_kernel_test.cpp \
  src/backend/flash/ecu/denso_beef_can_common.h \
  src/backend/flash/ecu/denso_beef_can_common_test.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp \
  src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp \
  src/backend/flash/ecu/BUILD.bazel
git commit -m "refactor(flash): deepen BEEF kernel transfer"
```

### Task 3: Move the shared read operation into `BeefCanKernel`

**Files:**

- Modify: `src/backend/flash/ecu/beef_can_kernel.cpp`
- Modify: `src/backend/flash/ecu/beef_can_kernel_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`
- Modify: `src/backend/flash/ecu/denso_beef_can_common.h`
- Modify: `src/backend/flash/ecu/denso_beef_can_common_test.cpp`
- Generated: `src/backend/flash/ecu/BUILD.bazel`

- [ ] **Step 1: Retarget the existing `ReadBeefMemoryTruncatesTheLastPageAndStopsOnFailureOrCancellation` case** to `BeefCanKernel::Read`. Preserve every assertion and add literal checks for the two requests of a `0x401`-byte region: both encode U16 `0x0400`, while the returned buffer contains `0x400 + 1` bytes. Assert a 2000 ms read timeout and progress capped at the requested region length.

- [ ] **Step 2: Run the test and see the unimplemented operation fail**:

```bash
bazel test --config=release //src/backend/flash/ecu:beef_can_kernel_test \
  --test_filter='*Read*'
```

Expected: the new/retargeted case fails because `Read` has no behavior yet.

- [ ] **Step 3: Implement `Read`** with opcode `03`, payload `00 + U24(address) + U16(0x0400)`, fixed 0x400 page stride, 2000 ms BEEF timeout, existing cancellation points, transfer-rate log calculation, raw reply bytes, tail truncation, and progress updates.

- [ ] **Step 4: Migrate petrol and diesel read callbacks to `context.kernel.Read(plan.TransferRegion(), progress)`.** Replace the TCU's local `ReadMemory` with the same call and delete its `kKernelPageSize`, read loop, and duplicate progress logic when no longer used.

- [ ] **Step 5: Remove `ReadBeefMemory` and read-only helpers from `denso_beef_can_common.h`; move all corresponding assertions to the kernel test.** No forwarding function remains.

- [ ] **Step 6: Regenerate, run the focused kernel test, then all three transcript suites**:

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release \
  //src/backend/flash/ecu:beef_can_kernel_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
```

Expected: PASS; especially the three existing raw-boundary read tests and the TCU dual-geometry test.

- [ ] **Step 7: Commit the read slice**:

```bash
git add \
  src/backend/flash/ecu/beef_can_kernel.cpp \
  src/backend/flash/ecu/beef_can_kernel_test.cpp \
  src/backend/flash/ecu/denso_beef_can_common.h \
  src/backend/flash/ecu/denso_beef_can_common_test.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp \
  src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp \
  src/backend/flash/ecu/BUILD.bazel
git commit -m "refactor(flash): centralize BEEF kernel reads"
```

### Task 4: Move compare and write into `BeefCanKernel`

**Files:**

- Modify: `src/backend/flash/ecu/beef_can_kernel.cpp`
- Modify: `src/backend/flash/ecu/beef_can_kernel_test.cpp`
- Modify: `src/backend/flash/ecu/beef_can_session.h`
- Modify: `src/backend/flash/ecu/beef_can_session.cpp`
- Modify: `src/backend/flash/ecu/beef_can_session_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`
- Modify: `src/backend/flash/ecu/denso_beef_can_common.h`
- Modify: `src/backend/flash/ecu/denso_beef_can_common_test.cpp`
- Generated: `src/backend/flash/ecu/BUILD.bazel`

- [ ] **Step 1: Move all existing warm write cases to the public kernel owner before implementation.** This includes the `QueryCrc`, `CompareBlocks`, `InitializeFlash`, and `WriteMemory` cases from `beef_can_session_test.cpp`, plus `CompareBeefBlocks*`, `InitializeBeefFlash*`, `FlashBeefBlock*`, and `WriteBeefMemory*` from `denso_beef_can_common_test.cpp`. Preserve their assertions and case intent; drive only `Write` (and a read/write setup fixture), not private helpers.

Ensure the public tests cover:

- no-difference early exit with ordered four-phase progress;
- one changed 0x1000 region with literal `04`, `25`, eight `22` chunks, `24`, and final CRC compare;
- `TestWrite` selecting `21` and `23`, never `20`/`24`;
- the TCU profile's 2000 ms init timeout and uppercase size logs versus ECU 500 ms;
- a 200 ms stale CRC drain that ignores content but propagates cancellation/disconnection;
- image/range/alignment errors before unsafe subspans;
- recovery warning after erase on write/commit/cancellation failure.

- [ ] **Step 2: Run the write-focused test and confirm it is red**:

```bash
bazel test --config=release //src/backend/flash/ecu:beef_can_kernel_test \
  --test_filter='*Crc*:*Compare*:*Initialize*:*Write*:*Flash*'
```

Expected: failures identify missing `Write` behavior, not changed expected wire bytes.

- [ ] **Step 3: Implement `Write` and its private helpers in `beef_can_kernel.cpp`**. Move BEEF exchange/framing/parsing, CRC query/compare, stale drain, flash initialization, voltage read, erase, 0x200 buffer writes, 0x1000 validate/commit, progress/rate reporting, recovery diagnostics, and final comparison into the implementation file.

Keep these timeouts literal or named private constants: CRC `3000 ms` plus stale drain `200 ms`; voltage `500 ms`; erase `3000 ms`; buffer `800 ms`; validate/commit `3000 ms`; profile-controlled init `500/2000 ms`.

- [ ] **Step 4: Migrate petrol and diesel write callbacks to `context.kernel.Write`.** Replace the TCU's `FlashBlock`, `ReflashBlock`, and `WriteMemory` path with `context.kernel.Write`, then delete the duplicate TCU opcode/constants/checksum/progress code that becomes unused.

- [ ] **Step 5: Shrink `BeefCanSession` to cold-path operations only.** Remove `Exchange`, `QueryCrc`, `CompareBlocks`, `InitializeFlash`, and `WriteMemory` from its public API and implementation. Move every removed test case to `beef_can_kernel_test.cpp` first.

- [ ] **Step 6: Delete all absorbed warm helpers from `denso_beef_can_common.h` and retarget all remaining common tests.** At this PR boundary the header may temporarily retain only cold/outer-runner helpers still used by PR 2. It must not contain a duplicate implementation or wrapper of transfer/read/write.

- [ ] **Step 7: Regenerate and run all shared/family tests**:

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release \
  //src/backend/flash/ecu:beef_can_kernel_test \
  //src/backend/flash/ecu:beef_can_session_test \
  //src/backend/flash/ecu:denso_beef_can_common_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
```

Expected: PASS, including the existing TCU real-write, repeated-window, stale-drain, disconnect, cancellation, and recovery-warning cases.

- [ ] **Step 8: Commit the write slice**:

```bash
git add \
  src/backend/flash/ecu/beef_can_kernel.cpp \
  src/backend/flash/ecu/beef_can_kernel_test.cpp \
  src/backend/flash/ecu/beef_can_session.h \
  src/backend/flash/ecu/beef_can_session.cpp \
  src/backend/flash/ecu/beef_can_session_test.cpp \
  src/backend/flash/ecu/denso_beef_can_common.h \
  src/backend/flash/ecu/denso_beef_can_common_test.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp \
  src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp \
  src/backend/flash/ecu/BUILD.bazel
git commit -m "refactor(flash): centralize BEEF kernel writes"
```

### Task 5: Close and qualify PR 1

**Files:**

- Modify: `docs/reference/flash-contracts.md`
- Modify if needed: source comments in the three family executor `.cpp` files
- Modify: `docs/reference/sonar-dedup-ledger.json` only through the dedup-loop workflow when it requires an attempt update

- [ ] **Step 1: Update the durable sharing contract** to say petrol, diesel, and TCU share kernel payload transfer/read/write, while the ECU and TCU cold paths and post-upload handoffs remain distinct. Remove stale comments claiming TCU read/write wire behavior differs. Do not claim hardware qualification.

- [ ] **Step 2: Audit test preservation** by rerunning the Task 1 `rg` inventory against the new test files. Account for every old case by same name or an explicit old-name → new-name note in the PR description. Verify no assertion disappeared in the moves with a scoped diff review.

- [ ] **Step 3: Run generated-file, focused, full, formatting, and static-analysis gates**:

```bash
python3 scripts/gazelle_check.py --fix
git diff --check
bazel test --config=release \
  //src/backend/flash/ecu:beef_can_kernel_test \
  //src/backend/flash/ecu:beef_can_session_test \
  //src/backend/flash/ecu:denso_beef_can_common_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy
```

Expected: every command passes. If local clang-tidy prerequisites are unavailable, do not claim that gate; rely on and report the required CI job.

- [ ] **Step 4: Measure the PR head**:

```bash
python3 scripts/dedup_loop.py measure --json
```

Expected: `total` is at least 10 lower than Task 1's actual merge-base measurement. If not, stop and revise the seam; do not delete tests or unrelated code to manufacture the drop.

- [ ] **Step 5: Run local changed-line mutation evidence when the pinned Mull toolchain is available**:

```bash
git merge-base origin/master HEAD
python3 scripts/mutation_test.py \
  --diff-ref <merge-base-sha-from-previous-command> \
  --scope //src/backend/flash/ecu/... \
  --report-dir /tmp/fastecu-beef-kernel-mutation
```

Expected: no surviving mutant on changed lines. The CI `mutation` report and `sonar-dedup verdict` remain authoritative; do not waive them because a local tool is absent.

- [ ] **Step 6: Request a fresh ADR 0029 right-sizing review** focused on the five Review Focus items. The reviewer must confirm the kernel has three operations, all three callers use it, no forwarding layer remains for warm work, and the TCU cold path was not generalized.

- [ ] **Step 7: Commit docs/cleanup, open PR 1, apply `sonar-dedup`, and wait for all required checks**:

```bash
git add \
  docs/reference/flash-contracts.md \
  docs/reference/sonar-dedup-ledger.json \
  src/backend/flash/ecu/BUILD.bazel \
  src/backend/flash/ecu/beef_can_kernel.h \
  src/backend/flash/ecu/beef_can_kernel.cpp \
  src/backend/flash/ecu/beef_can_kernel_test.cpp \
  src/backend/flash/ecu/beef_can_session.h \
  src/backend/flash/ecu/beef_can_session.cpp \
  src/backend/flash/ecu/beef_can_session_test.cpp \
  src/backend/flash/ecu/denso_beef_can_common.h \
  src/backend/flash/ecu/denso_beef_can_common_test.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp \
  src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp
git commit -m "docs(flash): record BEEF kernel sharing contract"
```

PR 1 is not complete until `sonar-dedup verdict` passes NLOC, mutation, changed-line coverage, and new-duplication checks and the right-sizing review is recorded. Merge PR 1 before starting PR 2.

---

## PR 2 — Concentrate the Petrol/Diesel ECU Sequence

### Task 6: Establish the PR 2 baseline and introduce the typed ECU entry points

**Files:**

- Create: `src/backend/flash/ecu/denso_beef_can_ecu_execution.h`
- Create: `src/backend/flash/ecu/denso_beef_can_ecu_execution.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp`
- Generated: `src/backend/flash/ecu/BUILD.bazel`

**Public internal-package interface:**

```cpp
Result<FlashExecutionResult>
RunDensoBeefCanEcu(const SubaruDensoSh7058CanPlan& family,
                   const FlashPlan& plan, ICanFlashTransport& transport,
                   IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events);

Result<FlashExecutionResult>
RunDensoBeefCanEcu(const SubaruDensoSh7058CanDieselPlan& family,
                   const FlashPlan& plan, ICanFlashTransport& transport,
                   IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events);
```

These overloads are compile-time admission, not a runtime family selector. The `.cpp` implements one private function template and exactly two private `EcuFamilyTraits` specializations. Traits provide pure family facts/security behavior; they do not provide steps as callbacks.

- [ ] **Step 1: After PR 1 merges, branch from updated `master`, measure NLOC, inventory tests, and run the same focused baseline** as Task 1. Use branch `refactor/denso-beef-can-ecu-execution`.

- [ ] **Step 2: Make the diesel adapter delegate to the declared overload before defining it.** Replace its `Run` body with the call shown in Step 5, leaving the existing diesel transcript suite unchanged. This makes the established external seam test the new deep owner without copying a second full protocol transcript.

- [ ] **Step 3: Confirm it is red because the new entry point is missing**:

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test
```

Expected: link or compilation failure because the new diesel execution overload has no definition. The existing test source and its literal wire expectations remain unchanged.

- [ ] **Step 4: Implement the fixed outer operation lifecycle and diesel traits** in `denso_beef_can_ecu_execution.cpp`: Kernel phase; tolerant running-kernel probe; identity; session discovery; diesel security; programming-session choice; `BeefCanKernel::TransferPayload`; ECU transfer exit/delay/routine start/strict kernel ID; then kernel read/write and final result/progress.

Do not call `RunBeefKernelOperation`; the new implementation owns the order directly. Do not add probe/connect/upload/read/write function parameters.

- [ ] **Step 5: Keep `SubaruDensoSh7058CanDieselExecutor::Run` as the thin delegation introduced in Step 2**:

```cpp
return RunDensoBeefCanEcu(family, plan, transport, clock,
                          cancellation, events);
```

Keep `Setup` and `BeforeTransportConfigure` in the named adapter.

- [ ] **Step 6: Run the full diesel suite**:

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test
```

Expected: PASS with all diesel generations, strict pending behavior, TestWrite, cancellation, stale-drain, and recovery cases unchanged.

- [ ] **Step 7: Commit the diesel tracer bullet**:

```bash
git add \
  src/backend/flash/ecu/denso_beef_can_ecu_execution.h \
  src/backend/flash/ecu/denso_beef_can_ecu_execution.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.cpp \
  src/backend/flash/ecu/BUILD.bazel
git commit -m "refactor(flash): own diesel BEEF ECU sequence"
```

### Task 7: Add petrol specialization and migrate the petrol adapter

**Files:**

- Modify: `src/backend/flash/ecu/denso_beef_can_ecu_execution.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp`
- Generated: `src/backend/flash/ecu/BUILD.bazel`

- [ ] **Step 1: Make the petrol adapter delegate to its declared overload before defining the petrol traits.** Leave `AllFiveValidatedEnumsProduceTheirFixedSecurityVector`, the full cold-upload cases, and all RaceRom-alt transcript expectations unchanged in the petrol executor suite.

- [ ] **Step 2: Run the petrol security/cold-path filters and see the missing overload fail**:

```bash
bazel test --config=release //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  --test_filter='*AllFiveValidatedEnums*:*ProbeTimeoutUploads*:*IdentityAndVendorFailures*'
```

- [ ] **Step 3: Add the petrol `EcuFamilyTraits` specialization**. Move the five key calculations, modular exponentiation/table facts, CAL-ID lookup, and RaceRom-alt RAM preparation into the ECU execution implementation. The common fixed-order template invokes trait operations at named fixed points; traits may calculate data but may not select/reorder phases.

- [ ] **Step 4: Keep `SubaruDensoSh7058CanExecutor::Run` as the same thin `RunDensoBeefCanEcu(...)` delegation introduced in Step 1.** Keep petrol reset/configure/open lifecycle hooks in the named adapter.

- [ ] **Step 5: Delete migrated petrol/diesel cold implementations from the two adapter `.cpp` files** only after both full suites pass. The adapters should retain setup/lifecycle and one Run delegation; they must not retain alternate dormant session/security/upload code.

- [ ] **Step 6: Regenerate and run both ECU family suites**:

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test
```

Expected: PASS with byte-for-byte existing family transcripts and unchanged lifecycle tests.

- [ ] **Step 7: Commit the petrol migration**:

```bash
git add \
  src/backend/flash/ecu/denso_beef_can_ecu_execution.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_executor.cpp \
  src/backend/flash/ecu/BUILD.bazel
git commit -m "refactor(flash): share Denso BEEF ECU execution"
```

### Task 8: Remove the callback runner and absorb the remaining session

**Files:**

- Modify: `src/backend/flash/ecu/denso_beef_can_ecu_execution.cpp`
- Modify: `src/backend/flash/ecu/beef_can_kernel_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp`
- Modify: `src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp`
- Delete: `src/backend/flash/ecu/beef_can_session.h`
- Delete: `src/backend/flash/ecu/beef_can_session.cpp`
- Delete: `src/backend/flash/ecu/beef_can_session_test.cpp`
- Delete: `src/backend/flash/ecu/denso_beef_can_common.h`
- Delete: `src/backend/flash/ecu/denso_beef_can_common_test.cpp`
- Generated: `src/backend/flash/ecu/BUILD.bazel`

- [ ] **Step 1: Retarget every remaining `BeefCanSessionTest` and `DensoBeefCanCommonTest` case before deleting their sources.** ECU probe/identity/session/logging cases move to the petrol or diesel executor suite and continue through the stable family seam. TCU wording/identity behavior moves to its existing executor suite where appropriate. Profile-only and kernel-observable assertions move to `beef_can_kernel_test.cpp`. Preserve case names where practical and document every rename.

- [ ] **Step 2: Run the retargeted tests while the old implementation still exists** to validate the scripted setup:

```bash
bazel test --config=release \
  //src/backend/flash/ecu:beef_can_kernel_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
```

- [ ] **Step 3: Move the remaining ECU cold helpers**—tolerant query/logging, kernel-ID probe, identity parsing, accepted-session tracking, strict exchange validation—into the anonymous namespace/private traits in `denso_beef_can_ecu_execution.cpp`.

- [ ] **Step 4: Give the TCU its own explicit outer lifecycle and local cold helpers.** Replace `RunBeefKernelOperation` with locally readable code preserving: phases/results, `7A A0` five-attempt probe and bounded fragment drain, ECU/CAL identity, required session `03`, TCU seed/key, programming session `02`, optional payload transfer, exit/start, ISO-15765 restart, 500 ms sleep, strict proprietary re-probe, and kernel read/write.

Do not copy the ECU runner or introduce a generalized runner. Small local channel/parsing helpers are private to the TCU translation unit; its warm operations remain calls to `BeefCanKernel`.

- [ ] **Step 5: Delete `RunBeefKernelOperation`, the remaining common-header helpers, and all `BeefCanSession` files/targets.** Move any required pure BEEF framing/parsing into the private implementation of the owner that uses it. Verify no include or symbol remains:

```bash
rg -n 'BeefCanSession|RunBeefKernelOperation|denso_beef_can_common' src tests
```

Expected: no matches.

- [ ] **Step 6: Regenerate and run every BEEF suite**:

```bash
python3 scripts/gazelle_check.py --fix
bazel test --config=release \
  //src/backend/flash/ecu:beef_can_kernel_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
```

Expected: PASS. Compare the final named-test inventory with Task 6's baseline and account for every moved case.

- [ ] **Step 7: Commit the removal**:

```bash
git add \
  src/backend/flash/ecu/beef_can_kernel_test.cpp \
  src/backend/flash/ecu/denso_beef_can_ecu_execution.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp \
  src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp \
  src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp \
  src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp \
  src/backend/flash/ecu/BUILD.bazel
git add -u \
  src/backend/flash/ecu/beef_can_session.h \
  src/backend/flash/ecu/beef_can_session.cpp \
  src/backend/flash/ecu/beef_can_session_test.cpp \
  src/backend/flash/ecu/denso_beef_can_common.h \
  src/backend/flash/ecu/denso_beef_can_common_test.cpp
git commit -m "refactor(flash): remove shallow BEEF session seams"
```

### Task 9: Close and qualify PR 2

**Files:**

- Modify: `src/backend/flash/GLOSSARY.md`
- Modify: `docs/reference/flash-contracts.md`
- Modify: `docs/reference/sonar-dedup-ledger.json` only through the dedup-loop workflow when required
- Delete after enduring knowledge is extracted: `docs/superpowers/specs/2026-10-26-denso-beef-can-deepening-design.md`
- Delete after enduring knowledge is extracted: `docs/superpowers/plans/2026-10-26-denso-beef-can-deepening.md`

- [ ] **Step 1: Update enduring documentation**. The glossary names one Denso BEEF CAN dialect, the shared warm-kernel owner, the shared ECU cold-path owner, and the separate TCU cold path. The flash contract records the exact shared operations/deviations and preserved transcript evidence. Do not duplicate the wire inventory in both documents; link to the contract owner.

- [ ] **Step 2: Verify the design/plan documentation PR is present in delivered `master` history.** The files are now completed temporary documents; delete both in Step 7 after their enduring contract has moved to the glossary and flash contract. If that documentation PR was not preserved separately, stop and preserve the documents in delivered history before deleting them.

- [ ] **Step 3: Perform the final dependency/surface audit**:

```bash
rg -n 'BeefCanSession|RunBeefKernelOperation|denso_beef_can_common' src tests
rg -n 'class BeefCanKernel|TransferPayload|Result<bytes::Bytes> Read|Status Write' \
  src/backend/flash/ecu/beef_can_kernel.h
rg -n 'RunDensoBeefCanEcu' src/backend/flash/ecu
```

Expected: the removed seams have no matches; the kernel header has only the three operations; exactly two ECU overloads and two adapter calls exist; TCU does not call the ECU entry point.

- [ ] **Step 4: Run the full gates**:

```bash
python3 scripts/gazelle_check.py --fix
git diff --check
bazel test --config=release \
  //src/backend/flash/ecu:beef_can_kernel_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_executor_test \
  //src/backend/flash/ecu:subaru_denso_sh7058_can_diesel_executor_test \
  //src/backend/flash/ecu:subaru_tcu_denso_sh705x_can_executor_test
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy
```

- [ ] **Step 5: Measure NLOC and run local mutation evidence** as in Task 5, using PR 2's actual merge base. Expected: at least 10 total NLOC removed and no surviving changed-line mutant. Stop if the independent PR fails either condition.

- [ ] **Step 6: Request the fresh ADR 0029 right-sizing review.** Require an explicit answer to: Is the ECU module one fixed algorithm rather than a configurable framework? Are family differences compile-time and limited to facts/security? Is the TCU order visibly separate? Are the kernel's three operations cohesive? Can the old session/common seams be deleted without reconstructing them elsewhere?

- [ ] **Step 7: Remove the completed design/plan, commit the enduring docs, open PR 2, apply `sonar-dedup`, and wait for every gate**:

```bash
git add \
  src/backend/flash/GLOSSARY.md \
  docs/reference/flash-contracts.md \
  docs/reference/sonar-dedup-ledger.json
git add -u \
  docs/superpowers/specs/2026-10-26-denso-beef-can-deepening-design.md \
  docs/superpowers/plans/2026-10-26-denso-beef-can-deepening.md
git commit -m "docs(flash): record Denso BEEF execution owners"
```

PR 2 is complete only when all repository CI, `sonar-dedup verdict`, and the right-sizing review pass. Do not claim hardware qualification; the qualification matrix is unchanged.
