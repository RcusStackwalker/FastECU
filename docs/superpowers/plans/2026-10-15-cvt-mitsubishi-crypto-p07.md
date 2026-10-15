# Mitsubishi CVT Crypto P07 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the identical MH8104/MH8111 seed-key, ROM encryption and page decryption wrappers into the existing Mitsubishi CVT common module, preserving both family wire contracts.

**Architecture:** Extend `subaru_tcu_cvt_mitsu_can_common` with three named pure byte operations bound to its existing family tables and the canonical stock transformation. Keep the table accessors and lower-level SSM algorithms; executors delegate only these operations. Connection, transport, image addressing, checksum acceptance and programming flow remain family-owned.

**Tech Stack:** C++23, portable byte spans/vectors, GoogleTest/Google Mock, Bazel, Gazelle, prek.

**Spec:** P07 of the [production clone inventory](../specs/2026-10-14-flash-duplication-triage.md), the CVT common-module responsibility in the [confirmed investigation](../specs/2026-10-14-flash-duplication-investigation.md#module-responsibilities), and the [sharing contract](../../design-notes.md#share-only-proven-protocol-equivalence).

## Global Constraints

- “Complete SeedKey/EncryptRom/DecryptPage wrappers are identical, use already shared TcuCvtMitsu tables and stock index transformation; move named pure operations into existing CVT common module. No executor flow merger.”
- “Keep family executors as orchestration owners.”
- “Preserve current behavior during extraction; propose evidence-backed corrections separately with a reproducing test and qualification divergence note”.
- “Retain independent family transcript expectations; do not generate expected wire bytes through the extracted implementation.”
- “Qualification evidence and hardware status do not improve through refactoring.”
- Portable code only: no Qt, owned threads, filesystem I/O, new ErrorKind or widened visibility. Production Result/Status checks use `.has_value()`; tests use package-owned GoogleTest support. Warnings remain errors.
- Scope is P07 alone. Preserve MH8104 raw-channel/tolerant-checksum behavior, MH8111 UDS/fatal-checksum behavior, write geometry, image offset selection, retries, padding and timeout/cancellation policies.
- Keep `TcuCvtMitsuSeedKeyTable`, `TcuCvtMitsuEncryptTable` and `TcuCvtMitsuDecryptTable` signatures and exact values. Add no dependency, protocol dialect, general crypto engine or duplicated stock constant.

## Review Focus

- Seeds shorter than four bytes return empty; trailing seed bytes are ignored (Task 1).
- Empty or 1–3-byte payloads return empty; partial final words are dropped rather than padded/rejected (Task 1).
- Encrypt/decrypt tables and seed round direction must not be exchanged; literal asymmetric vectors pin direction (Task 1).
- A nonzero input subspan is transformed from its own beginning; the source remains unchanged (Task 1).
- Both executor families retain literal SecurityAccess bytes and family-local transport/checksum policies (Task 2).

---

## Source findings and file responsibilities

Planning baseline: merged PR #638, `1db8ca8d29a6e3af2e324f23c4e3698e5226efde`. Inspect current source before execution.

- Modify `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h` / `.cpp`: existing tables plus the operations below. Header currently says the stock transformation stays per-file, although both executors already use `ssm_protocol::kIndexTransformationStock`; replace that stale rationale when documenting the new seam. No new module.
- Modify `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common_test.cpp`: retain table tests; add fixed byte-vector/boundary tests.
- Modify `src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp` and `subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp`: remove only local `SeedKey`, `EncryptRom`, `DecryptPage`; replace their calls. Retain other SSM includes if still used.
- Modify both corresponding `_executor_test.cpp` files: strengthen independent transcripts with literal seed-key/payload expectations; never call the new common functions for expected bytes.
- Regenerate `src/backend/flash/ecu/BUILD.bazel`: common module gains an SSM implementation dependency. Preserve existing narrow visibility and review generated changes.
- Modify `docs/design-notes.md#share-only-proven-protocol-equivalence`: name the two consumers, three operations and retained policy differences. No qualification change.

This plan does not authorize C++ implementation. Execution follows plan review; the prior subagent-driven execution preference can be retained.

### Task 1: Add characterized pure CVT crypto operations

**Files:** Common header, implementation, test and generated BUILD file listed above.

**Interfaces:** Consume existing table accessors and `ssm_protocol::CalculateSeedKey` / `CalculatePayload`. Produce these signatures in `fastecu::flash`:

```cpp
bytes::Bytes TcuCvtMitsuSeedKey(bytes::ByteView seed);
bytes::Bytes TcuCvtMitsuEncryptRom(bytes::ByteView image);
bytes::Bytes TcuCvtMitsuDecryptPage(bytes::ByteView page);
```

- [ ] **Step 1: Add failing vector tests** to `SubaruTcuCvtMitsuCanCommonTest`, using `ElementsAreArray` with literal expected arrays (hex below). Compare contents, not round-trip success alone.

| Test | Exact assertions |
| --- | --- |
| `SeedKeyMatchesFixedVectors` | `00000000 → c261583a`; `11223344 → 12b6ceba`; `ffffffff → e3a06c00`. |
| `SeedKeyPreservesShortAndTrailingInputBehavior` | Lengths 0, 1, 2, 3 return empty; `11223344aabb → 12b6ceba`. |
| `EncryptRomMatchesFixedVectors` | `00000000 → f629a306`; `0011223344556677 → d78e41f97ab59e6d`; `000102030405060708090a0b0c0d0e0f → 76f9ec8b7d536fbf50e53f6116c7d55c`; `ffffffff → 68fc4e1c`. |
| `DecryptPageMatchesFixedVectors` | The literal inverse of each encryption vector above. Use ciphertext literals directly, not ciphertext generated in the test. |
| `PayloadOperationsPreservePartialWordBehavior` | Both operations return empty for lengths 0–3. Encrypt `00112233445566 → d78e41f9`; decrypt `d78e41f97ab59e → 00112233`; trailing bytes do not affect full words. |
| `TransformsOnlyTheInputViewWithoutMutation` | Backing bytes `aabb0011223344556677ccdd`, subspan `(2,8)` encrypts to `d78e41f97ab59e6d`; decrypt view of `aabbd78e41f97ab59e6dccdd` at `(2,8)` yields `0011223344556677`; both backing vectors equal saved copies afterward. |

Vector provenance: a separate Python Feistel calculation with explicit 16-bit truncation, 32-bit word swapping, literal CVT tables and stock substitution entries produced these literals during planning. They are independently calculated regression expectations, not external protocol/hardware proof. Existing table tests retain legacy table provenance. Never replace expected literals with runtime calls to shared or underlying production algorithms.

- [ ] **Step 2: Run the existing common target before implementation**: `bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_can_common_test`. Expected RED: new operation names undeclared.
- [ ] **Step 3: Declare and implement the three exact signatures.** Move the complete current wrapper bodies unchanged into the common `.cpp`, using the corresponding table accessor and `ssm_protocol::kIndexTransformationStock`. Payload operations retain `static_cast<std::uint32_t>(view.size())`; add no alignment/size guard or Result return. Header includes portable byte definitions, identifies both consumers, and replaces stale per-file transformation wording.
- [ ] **Step 4: Regenerate the graph**: `python3 scripts/gazelle_check.py --fix`; review the common module's SSM implementation dependency and unchanged visibility.
- [ ] **Step 5: Run the common test**, expected GREEN. Temporarily bind encrypt to the decrypt table and verify `EncryptRomMatchesFixedVectors` fails, then restore. Temporarily change seed-table entry `0x9E99` to `0x9E98`, run `SeedKeyMatchesFixedVectors` and verify failure, then restore. Rerun the complete common target, expected PASS; no mutation is committed.
- [ ] **Step 6: Commit** only the three common files and generated BUILD file: `refactor: add characterized Mitsubishi CVT crypto operations`.

### Task 2: Delegate both families with independent wire expectations

**Files:** Both executors and corresponding tests; design notes; regenerated BUILD file only if dependencies change.

**Interfaces:** Consume Task 1's exact three signatures. Public executor interfaces and table accessors remain unchanged.

- [ ] **Step 1: Strengthen both family transcripts before migration.** In each test file, change `ScriptFullConnect`'s key argument for existing seed `{0x11,0x22,0x33,0x44}` to literal `{0x12,0xb6,0xce,0xba}`. Keep separate test-local seed helpers needed for other seeds; never replace them with the production common function. Preserve MH8104's content-blind key acknowledgement and MH8111's required acknowledgement.
- [ ] **Step 2: Pin the first payload in each existing write transcript.** `WriteRom()` fills bytes with their index modulo 256; both selected write starts are multiples of 256. Assert the selected first sixteen bytes equal `000102030405060708090a0b0c0d0e0f`. At the first `0xB6` request, replace the first sixteen expected encrypted payload bytes with literal `76f9ec8b7d536fbf50e53f6116c7d55c`; the rest may use existing independent test-local `ToWire`. Apply this in MH8111 `ScriptWriteBlock` and MH8104 `WriteFlashesTheBlockToleratingEveryContentMismatch`. Preserve request addresses, chunk sizes (256 / 128), subsequent replies and checksum expectations. This makes the first wire payload independently specified even if lower-level production crypto changes.
- [ ] **Step 3: Run both family suites before migration**:

```sh
bazel test --config=release //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8104_can_executor_test //src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8111_can_executor_test
```

Expected PASS: these are preservation fixtures. Existing reads continue to exercise decryption and image assembly; Task 1 pins independent literal decrypt results.

- [ ] **Step 4: Remove the six local wrapper definitions and replace their consumers** with `TcuCvtMitsuSeedKey`, `TcuCvtMitsuEncryptRom`, `TcuCvtMitsuDecryptPage`. Preserve where each executor selects its image subspan before encryption, and decryption's `uds::Payload` input. No connect/read/write flow refactoring. Regenerate BUILD files and inspect the diff.
- [ ] **Step 5: Update the owning sharing contract** in design notes with the three operations, consumers and retained raw-channel versus UDS/checksum policies. Link the common header. Legacy line citations establish table provenance only, not qualification or the correctness of every current difference.
- [ ] **Step 6: Run the common and two family targets**, expected PASS. Review production diff for only wrapper movement/delegation. Existing cancellation, disconnect, wrong-content and pre-I/O image validation cases remain unchanged and passing.
- [ ] **Step 7: Run applicable final gates sequentially**:

```sh
python3 scripts/gazelle_check.py --fix
bazel build --config=release //:fastecu
bazel test --config=release //...
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Expected zero exit statuses, no graph drift or changed-code findings; fix causes without suppression. Use pinned Bazel/host setup from PR CI. Record compiler, platform skips and outstanding CI/packaging gates; never infer hardware qualification. A clone scan is optional measurement, with no percentage threshold.

- [ ] **Step 8: Commit** named executor/test/documentation paths and any generated graph changes: `refactor: share Mitsubishi CVT crypto wrappers`. Obtain task and whole-branch reviews before completion. Preserve this plan in delivered history; delete it in a separate cleanup after delivery. Keep the broader investigation/triage while their remaining work is unfinished.
