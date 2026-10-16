# Remaining Flash Duplication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve the remaining clone candidates through small, independently tested extractions and explicit investigation outcomes.

**Architecture:** Family executors retain orchestration, timing, response tolerance,
address translation, and erase policy. Extend package-owned primitives for proven
pure or bounded operations. Investigations produce decisions before any dependent
extraction; they do not authorize behavioral normalization.

**Tech Stack:** C++23, GoogleTest, Bazel, Gazelle, scripted flash transports.

**Spec:** [Remaining flash duplication triage](../specs/2026-10-14-flash-duplication-triage.md).

## Global Constraints

- Backend and algorithms are portable: no Qt, owned threads, direct filesystem I/O, or dependencies on platform code.
- Execution context and lifetimes belong to callers.
- Backend exceptions never cross ports. Check `Result`/`Status` with `.has_value()`; adding an `ErrorKind` requires an ADR.
- Tests and mocks are package-owned. Every C++ suite uses GoogleTest.
- Never relax an address-window guard or claim qualification without recorded evidence.
- Preserve family sequencing, timeouts, response tolerance, retry policy, address translation, and erase rules.
- Keep catalog identities, validation precedence, diagnostics, and explicit TestWrite guards family-owned.
- Use feature branches and reviewable commits. Implementation requires separate authorization; this change supplies documentation only.

## Review Focus

- Absent versus present-empty replies must retain distinct results and diagnostics (Tasks 2, 4, 5).
- Cancellation concurrent with a transport error must retain the current error precedence and I/O count (Tasks 2, 4, 5).
- Invalid image/kernel plus unsupported TCU operation must still reject the unsupported operation first (Task 1).
- Unchanged ROM must still enable programming voltage before the no-change return (Task 3).
- Nonzero image bases, final chunks, and mismatched post-write CRC must retain family addressing and current returned status (Tasks 3, 7).

## Work groups and order

| Group | Pair IDs | Deliverable | Dependency |
|---|---|---|---|
| Pure plan validation | P32–P36, P38–P39, P42 | Task 1 | None |
| Denso K-Line operations | P40, P44–P46, P52–P55, P57 | Tasks 2–3 | Task 3 follows Task 2 |
| Denso CAN read operations | P23, P25, P81 | Task 4 | Evidence gate within task |
| K-Line protocol evidence | P06, P19, P48–P49 | Task 5 | None |
| CVT protocol evidence | P11–P12 | Task 6 | None |
| Denso CAN connection/programming evidence | P22, P24, P26, P61–P62, P64, P67, P73–P74, P84–P85 | Task 7 | None |

Tasks 1–3 are the first implementation series. Task 4 requires a sharing-contract
decision because the current Denso common header deliberately leaves those flows
local. Tasks 5–7 are separate evidence workstreams: each may close with a retain
decision. No universal executor or transfer state machine is planned.

All source paths below are relative to `src/backend/flash/ecu/`. Each task also
owns its generated `BUILD.bazel` changes. Preserve literal expectations in family
suites; never generate expected wire bytes with the production helper under test.

### Task 1: Extract pure plan checks

**Files:** Modify `plan_primitives.{h,cpp}`, `plan_primitives_test.cpp`,
`subaru_denso_sh705x_densocan_plan.cpp`, `subaru_tcu_denso_sh705x_can_plan.cpp`,
`subaru_denso_sh7058_can_plan.cpp`, `subaru_denso_sh7058_can_diesel_plan.cpp`,
`subaru_denso_mc68hc16y5_02_plan.cpp`, `subaru_denso_sh7055_02_plan.cpp`, and
those families' existing `*_plan_test.cpp` files.

**Interfaces:** Add pure predicates in `fastecu::flash::detail`:
`bool FixedShapeDeviceMatches(const FlashDevice*, std::uint32_t rom_size, unsigned block_count)`,
`bool OperationImageShapeMatches(const FlashPlan&, std::uint32_t rom_size)`, and
`bool TransferMatchesDevice(const MemoryRegion&, const FlashDevice&)`.
The first rejects null device/flash/kernel tables and mismatched size/count.
The second requires no image for Read and an exact-size image otherwise.
The third compares transfer start/length with the device's flash start/ROM size.
Callers retain error construction and check order. Existing
`ValidateKernelUpload<BlockSize>` and `MakeEraseRegions` remain the arithmetic owners.

- [ ] Add predicate cases for null/missing tables, size/count mismatch, Read with image, missing/wrong-sized write image, and wrong transfer start/length; assert false for each and true for valid inputs.
- [ ] Add family regressions combining unsupported TCU TestWrite/SH7055 Write with invalid kernel/image, asserting the existing Unsupported error and detail. Pin upload padding of 6 versus 128 bytes and exact-window/overflow rejection.
- [ ] Run `bazel test --config=release //src/backend/flash/ecu:plan_primitives_test`; new predicate tests must fail to compile before implementation.
- [ ] Implement the predicates and migrate only matching subchecks; retain catalog lookup, confirmations, transport/security checks, and diagnostics locally. Do not manufacture a generic builder for the remaining skeleton.
- [ ] Run `bazel test --config=release //src/backend/flash/ecu/...`; all tests must pass. Review rejection precedence and generated BUILD changes, then commit named paths as `refactor: share pure flash plan checks`.

### Task 2: Extract Denso K-Line framing and optional exchange

**Files:** Create `denso_beef_kline_common.{h,cpp}` and
`denso_beef_kline_common_test.cpp`; modify
`subaru_denso_mc68hc16y5_02_executor.cpp`, `subaru_denso_sh7055_02_executor.cpp`,
and their existing executor tests.

**Interfaces:** In `fastecu::flash`, add
`bytes::Bytes DensoBeefKlineFrame(bytes::Byte opcode, bytes::ByteView payload = {})`,
`bool DensoBeefKlineResponseOk(bytes::ByteView response, bytes::Byte ack)`, and
`Result<IKlineFlashTransport::OptionalBytes> DensoBeefKlineExchangeOptional(IKlineFlashTransport&, IClock*, const ICancellationToken&, bytes::ByteView request, std::chrono::milliseconds settle, std::chrono::milliseconds timeout)`.
Move byte-identical opcodes into this module with Denso K-Line names. Keep the
BEEF CAN module separate. Required-response diagnostics and CRC accumulation stay local.

- [ ] Add literal frame vectors: opcode `01`, empty payload produces `BE EF 00 01 01 AF`; opcode `22`, payload `01 02` produces `BE EF 00 03 22 01 02 D5`. Pin current 16-bit length conversion and permissive ACK check (minimum six bytes, BE EF, opcode at offset four; no added checksum validation).
- [ ] Add scripted cases for short write, write/read error concurrent with cancellation, cancellation at every existing checkpoint, null clock, positive settle, failed sleep, silence, present-empty, and exact timeout. Assert the current error/detail, order, and counts independently in both family suites.
- [ ] Run the new target before adding implementations; expect compilation failure for missing interfaces.
- [ ] Move the matching Frame/ResponseOk/optional exchange code. Preserve before-write, after-write, before-read, after-read checkpoints and transport-error precedence. Share the CRC initial checked-write leg only if its checkpoints match; leave its continuation reads unchanged (P40).
- [ ] Run both family executor targets and the common target with `bazel test --config=release`; all must pass. Regenerate BUILD files, review, and commit as `refactor: share Denso BEEF K-Line primitives`.

### Task 3: Extract K-Line encoding, comparison, and programming-line operations

**Files:** Extend Task 2's common files/tests and both executor files/tests.

**Interfaces:** Add `bytes::Bytes DensoBeefKlineWritePayload(std::uint32_t address, bytes::ByteView chunk)` for the existing four-byte big-endian address-plus-chunk encoding, and
`Status DensoBeefKlineEnableProgrammingVoltage(IKlineFlashTransport&, const ICancellationToken&)`.
Add a package-owned comparison callable in the common header:
`template<class ReadCrc> Result<unsigned> DensoBeefKlineCompareBlocks(std::span<const MemoryRegion> blocks, bytes::ByteView addressed_image, std::vector<bool>& modified, const ICancellationToken&, ReadCrc&& read_crc)`;
`read_crc(const MemoryRegion&)` returns `Result<std::uint32_t>`.
Callers supply valid absolute-addressed spans and equally sized modified flags;
image rendering and family CRC decoders remain local.

- [ ] Add literal first/last chunk address tests and ordered multi-block comparison cases: all equal, one mismatch, CRC failure, and cancellation before the next block. Assert flags/count, addressed slices, and no subsequent CRC call after failure.
- [ ] Pin no-change programming-voltage invocation, before/after line cancellation, line error precedence, and final mismatch logs/current return status in both family suites. Preserve MC68 no settle/3000 ms versus SH 50 ms/2000 ms chunk exchanges.
- [ ] Run new helper tests before implementation and confirm they fail for missing interfaces.
- [ ] Implement and integrate the helpers for P52–P54/P57. P55 uses Task 2 framing only: keep capability response decoding, offset-six logs, and no-change return local. Share comparison before and after writing without changing success-on-mismatch behavior.
- [ ] Run the common and both family executor targets; expect PASS. Review addressed-image construction and named-path diff, then commit as `refactor: share bounded Denso K-Line write operations`.

### Task 4: Gate and characterize Denso CAN read operations

**Files:** Inspect `denso_iso15765_can_common.{h,cpp}`, all four
`subaru_denso_{1n83m_1_5m,1n83m_4m,sh72531}_can_executor.cpp` /
`subaru_denso_sh72543_can_diesel_executor.cpp` families and their tests;
update [flash architecture](../../design-notes.md#flash-architecture) and the
triage. The brace notation identifies the three literal family filenames.

**Interfaces:** No new production interface is approved here. Candidate boundaries
are one OBK probe, one six-attempt best-effort read-stop operation, and one encrypted
page request/decrypt operation. A follow-up extraction plan must specify exact
signatures after characterization and the sharing decision; complete read loops
remain family-owned.

- [ ] Add independent family scripts asserting probe hit/mismatch/silence/error/cancellation, six stop attempts with request `37`, tolerated stop errors/exhaustion, and exact B7 page requests/decrypted bytes/progress.
- [ ] Run the four family executor targets; characterization must pass against current code. Pin current short-page behavior, absent versus empty responses, and cancellation/error precedence without tightening validation.
- [ ] Compare complete candidate operations and record the contract disagreement: the triage proposes sharing while the common header intentionally retains stop/probe/read flows locally. Record a narrow evidence-backed decision in design notes before reconciling it.
- [ ] Preserve diesel's 1 ms page wait, family setup timeouts, 4M tolerance, image padding, and family logs. If equality is unproven, retain the operation and close its candidate with the evidence; otherwise write a bounded extraction follow-up with exact interfaces and tests.
- [ ] Commit the scripts and decision as `test: characterize Denso bootloader read boundaries`. This task completes with evidence and disposition, not a promised extraction.

### Task 5: Resolve K-Line pacing, address, upload, and CRC questions

**Files:** Existing executor/test pairs for
`subaru_tcu_hitachi_m32r_kline`, `subaru_hitachi_m32r_kline`,
`subaru_mitsu_m32r_kline`, `subaru_denso_mc68hc16y5_02`, and
`subaru_denso_sh7055_02`; update triage and the relevant enduring contract.

**Interfaces:** Existing executor APIs only; no shared read/upload/CRC state machine.

- [ ] Add scripts for TCU 100 ms before/after block reads (P06), Hitachi versus Mitsubishi request `00 00`/`00 20`, address bias/full-image FF padding (P19), upload silence versus present-empty (P48), and fragmented CRC/truncated/checksum/prefix replies (P49). Assert actual output, retry/read counts, 50 ms reads and 100 ms settles where currently used.
- [ ] Run the five family executor targets and confirm characterization passes unchanged.
- [ ] Locate independent family-specific protocol functions or traces. For each pair, record source revision/function mapping, observed behavior, local test, and verdict; mark unavailable evidence explicitly rather than guessing from strings or comments.
- [ ] Close each candidate as retain, proven extraction follow-up, or separately scoped defect with reproduction. Record unresolved evidence requirements in the triage; commit as `docs: resolve K-Line duplication evidence` with any characterization tests.

### Task 6: Resolve CVT security and transfer questions

**Files:** Existing executor/test pairs for `subaru_tcu_cvt_hitachi_m32r_can`
and `subaru_tcu_cvt_mitsu_mh8111_can`; update triage and enduring contract.

**Interfaces:** Existing executor APIs; retain local security/table bindings and close policy.

- [ ] Add literal seed/key/addressing cases covering Hitachi other-UDS `0x7E0` versus Mitsubishi primary UDS (P11). Add chunk silence/error and transfer-close scripts asserting Hitachi one fatal close versus MH8111 up to twenty fatal attempts (P12).
- [ ] Run both family executor targets; expect current behavior to pass.
- [ ] Map independent family security/reflash evidence and record the same source/function/test/verdict fields as Task 5. Do not treat different key tables as algorithm equivalence.
- [ ] Record retain/extraction-follow-up/separate-defect outcomes, leave unavailable evidence open, and commit as `docs: resolve CVT duplication evidence` with any characterization tests.

### Task 7: Resolve Denso CAN connection and programming questions

**Files:** The four bootloader family executors/tests from Task 4;
`uds_client_exchange_common_test.cpp`; triage and enduring flash contract.

**Interfaces:** Existing executor APIs; existing UDS shared exchange behavior stays intact.

- [ ] Add timed scripts for one versus two bench pre-loop reads and discarded first reply/50 ms wait, short versus long initial/continuation timeouts, tolerated jump exhaustion, OBK probe miss/error, and identity query failure/cancellation (P22/P24/P61/P62/P64/P73/P74).
- [ ] Add first/last 256-byte transfer and nonzero image-base cases that pin caller pre-offset plus callee indexing (P26). Pin close retry counts, 100 ms settle, checksum immediate-positive versus pending/final responses, and timeout/error outcomes (P67/P84/P85).
- [ ] Run all four family executor targets and the shared exchange target; current behavior must pass.
- [ ] Map independent ECUFlash family functions, including callers, before deciding address or discarded-read correctness. Record exact source/function/test/verdict fields for each pair. Keep retrying transfer exit distinct from erase re-reads.
- [ ] Update triage with retain/extraction-follow-up/separate-defect outcomes; unresolved source mapping stays explicitly open. Commit as `docs: resolve Denso CAN duplication evidence` with characterization tests. Any behavioral correction needs a separate plan and applicable qualification checklist.

## Verification and completion

For each implementation task, regenerate with `python3 scripts/gazelle_check.py --fix`
and review managed BUILD changes. Before delivery of application changes, run:

```sh
bazel build --config=release //:fastecu
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Require passing applicable platform CI/packaging gates. Preserve hardware status;
automated transcripts do not qualify a flash family. Update owning contracts and
qualification evidence when behavior changes. Remove resolved triage rows as tasks
land. Once the plan is complete, extract enduring knowledge and remove completed
spec/plan artifacts only after their originals are preserved in delivered history.

This documentation-only preparation uses `prek run lychee --all-files` and scoped
diff review; it does not require building the application.
