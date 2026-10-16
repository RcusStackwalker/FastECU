# Bounded Flash Extractions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract five proven, bounded flash operations as five independently reviewable pull requests in the agreed order.

**Architecture:** Keep family state machines, geometry, progress, diagnostics outside each shared operation. Add four narrow operations to the Denso ISO-15765 common module and one dedicated CVT B6 exchange module; retain independent literal family transcripts as the behavioral oracle.

**Tech Stack:** C++23, portable backend ports and byte types, GoogleTest, Bazel and Gazelle.

**Spec:** [Bounded flash extraction design](../specs/2026-10-16-bounded-flash-extractions.md)

## Global Constraints

- Deliver in order P81 → P12 → P26 → P84 → P62, one branch and pull request per extraction. Start each branch from the current master after the previous PR merges; preserve unrelated working-tree files. Push only when authorized.
- Preserve existing wire bytes, reply policy, timeouts, error and cancellation behavior, logs, sleeps, progress, and caller-owned setup, close and checksum. No protocol correction or qualification claim belongs in these PRs.
- Backend operations use portable bytes and injected ports; no Qt, owned thread, direct filesystem I/O or exception across a port. Check `Result` and `Status` with `.has_value()`.
- Keep expected wire frames literal in executor tests. Do not derive expected bytes from the new helpers.
- `src/backend/flash/ecu/BUILD.bazel` is Gazelle managed: run `python3 scripts/gazelle_check.py --fix` after source changes and review its diff. Do not hand-edit it.
- For each PR, update the owning [flash architecture](../../design-notes.md#flash-architecture), close only that candidate's row in the [triage](../specs/2026-10-14-flash-duplication-triage.md), and keep unresolved correctness evidence visible. P12 also updates the [flash language](../../../src/backend/flash/CONTEXT.md).
- Before each PR's delivery, run the focused target(s), `bazel build --config=release //:fastecu`, `bazel test --config=release //...`, `prek run --all-files`, `python3 scripts/gazelle_check.py --fix`, and `bazel run //:clang_tidy_report_changed`; check applicable platform CI and packaging. Stage named paths only.

## Review Focus

The tests named in the tasks below must also cover these likely mistakes:

1. P81 seven encrypted response bytes yield four decrypted bytes, while caller progress still advances 256 and family padding remains intact.
2. P12 a framed but unexpected chunk reply succeeds, because this exchange does not inspect response content.
3. P26 a nonzero N83M image origin still sends the correct absolute address and matching encrypted bytes for the last chunk.
4. P84 a returned cancellation error inside a retry is swallowed, whereas a truly cancelled token prevents later I/O and can fail the caller's settle.
5. P62 a matching acknowledgement returned by the final receive is not checked after the loop; exhaustion logs and returns success.

---

### Task 1: P81 Denso encrypted page read

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`, `src/backend/flash/ecu/denso_iso15765_can_common.cpp`
- Test: `src/backend/flash/ecu/denso_iso15765_can_common_test.cpp`
- Modify callers and retain their literal tests: `src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.cpp`, `subaru_denso_1n83m_4m_can_executor.cpp`, `subaru_denso_sh72531_can_executor.cpp`, `subaru_denso_sh72543_can_diesel_executor.cpp` and their matching `*_test.cpp` files
- Modify: `docs/design-notes.md`, `docs/superpowers/specs/2026-10-14-flash-duplication-triage.md`; generated `src/backend/flash/ecu/BUILD.bazel` only if Gazelle changes it

**Interfaces:**
- Consumes: `CanExecutorContext`, `FatalRequest`, `DensoDecryptPage`, `uds::Payload`.
- Produces: `Result<bytes::Bytes> ReadDensoEncryptedPage(const CanExecutorContext& ctx, std::uint32_t address)` in the Denso common header.

- [ ] Add `DensoIso15765CanCommonTest.ReadPageUsesLiteralB7AddressAndWholeWordDecrypt`: script `B7 08 FA C0 00` on 0x7E0 and `F7 E0 D3 85 2B AA BB CC` on 0x7E8; assert output `00 01 02 03`, one 2000 ms initial read, and no helper-owned sleep or progress. Add `ReadPagePropagatesAbsentEmptyTransportAndCancellation` with each existing error kind and no extra write.
- [ ] Run `bazel test --config=release //src/backend/flash/ecu:denso_iso15765_can_common_test --test_filter=DensoIso15765CanCommonTest.ReadPageUsesLiteralB7AddressAndWholeWordDecrypt`; expect failure because the declared helper is missing.
- [ ] Implement the signature in `denso_iso15765_can_common.{h,cpp}` using B7 plus four-byte big-endian address, the existing 2000 ms `FatalRequest` path and address-bearing read diagnostic, then `DensoDecryptPage(uds::Payload(reply))`. Add no page-length check. Update the common header's scope comment to name this exact shared operation.
- [ ] Run the focused common test; expect pass.
- [ ] Replace only the repeated B7 request/decrypt expression in each of the four `ReadMemory` loops. Leave setup tolerance, top-of-loop cancellation, requested-size progress, SH 1 ms pace, stop behavior and padding in callers.
- [ ] Run all four family executor test targets, including each `ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` and `PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` script; expect pass with unchanged literal frames, I/O ordering and progress. Add a family test only if a required distinction is not already pinned.
- [ ] Update design notes and P81 triage status; run Gazelle and the global gates, review the scoped diff and commit named paths. Open the P81 PR when authorized and wait for its merge before Task 2.

### Task 2: P12 CVT B6 chunk exchange

**Files:**
- Create: `src/backend/flash/ecu/cvt_b6_chunk_exchange.h`, `src/backend/flash/ecu/cvt_b6_chunk_exchange.cpp`, `src/backend/flash/ecu/cvt_b6_chunk_exchange_test.cpp`
- Modify callers and retain their literal tests: `src/backend/flash/ecu/subaru_tcu_cvt_hitachi_m32r_can_executor.cpp`, `subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp` and matching `*_test.cpp` files
- Modify: `src/backend/flash/CONTEXT.md`, `docs/design-notes.md`, `docs/superpowers/specs/2026-10-14-flash-duplication-triage.md`; regenerate `src/backend/flash/ecu/BUILD.bazel`

**Interfaces:**
- Consumes: `uds::IUdsChannel::Send`, `uds::IUdsChannel::Receive`, `ICancellationToken`, `bytes::U24`, `bytes::ByteView`.
- Produces: `Status ExchangeCvtB6Chunk(uds::IUdsChannel& channel, const ICancellationToken& cancellation, std::uint32_t address, bytes::ByteView encrypted_chunk)` in a dedicated CVT module.

- [ ] Add `CvtB6ChunkExchangeTest.SendsLiteralFrameAndAcceptsSilentOrUninspectedReply`: send literal `B6 08 00 00 11 22` and assert one 2000 ms receive, success on silence and on a framed `7F B6 22` reply. Add `PropagatesSendReceiveAndCancellationErrors` with separate send-error, receive-error and pre-cancelled-token cases asserting error kind and I/O count.
- [ ] Run `bazel test --config=release //src/backend/flash/ecu:cvt_b6_chunk_exchange_test`; expect failure while the module is absent.
- [ ] Implement the exact interface in the new header/source: compose B6 plus `U24(address)` plus bytes, send once, receive once at 2000 ms, discard any successful optional reply. Add no address check, new cancellation checkpoint or SID check. Regenerate BUILD targets with Gazelle.
- [ ] Run the focused helper target; expect pass.
- [ ] Replace each CVT write loop's B6 channel send/receive pair with the helper. Keep each caller's loop cancellation, encryption, 128/256-byte chunk selection, progress, setup, one-versus-twenty close policy and checksum.
- [ ] Run `//src/backend/flash/ecu:subaru_tcu_cvt_hitachi_m32r_can_executor_test` and `//src/backend/flash/ecu:subaru_tcu_cvt_mitsu_mh8111_can_executor_test`; expect their `WriteAcceptsSilentChunkReplies`, `WritePropagatesChunkTransportError` and `WriteCloseExhaustionIsFatal` scripts to pass, plus MH8111's twentieth-attempt success. Add a literal unexpected-reply family case if these scripts do not already pin it.
- [ ] Define **CVT B6 chunk dialect** for only these two consumers in `CONTEXT.md`; update architecture and P12 triage. Run the global gates, review the scoped diff and commit named paths. Open the P12 PR when authorized and wait for its merge before Task 3.

### Task 3: P26 Denso B6 write request

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`, `denso_iso15765_can_common.cpp`, `denso_iso15765_can_common_test.cpp`
- Modify four Denso bootloader executor `.cpp` files and matching `*_test.cpp` files listed in Task 1
- Modify: `docs/design-notes.md`, `docs/superpowers/specs/2026-10-14-flash-duplication-triage.md`; generated `src/backend/flash/ecu/BUILD.bazel` only if Gazelle changes it

**Interfaces:**
- Consumes: `CanExecutorContext`, caller-selected absolute address and already-encrypted `bytes::ByteView`, existing `FatalRequest`.
- Produces: `Status DensoIso15765WriteChunk(const CanExecutorContext& ctx, std::uint32_t address, bytes::ByteView encrypted_chunk)`.

- [ ] Add `DensoIso15765CanCommonTest.WriteChunkSendsLiteralB6AbsoluteAddress`: script literal `B6 08 FA C0 00 11 22`, `F6` reply, and one 500 ms initial read. Add `WriteChunkPropagatesNegativeTransportAndCancellation` with each unchanged error kind and the address-bearing fatal diagnostic.
- [ ] Run the focused common test filter; expect failure while the helper declaration is missing.
- [ ] Implement the exact common helper: compose B6, four-byte big-endian address and supplied encrypted bytes; use the existing 500 ms fatal request policy; discard the successful response. Do not index, encrypt or validate the image inside it.
- [ ] Run the common helper target; expect pass.
- [ ] Replace only each loop's request/response operation with the helper. Retain whole-image encryption, image base subtraction, 256-byte slice selection, top-of-loop cancellation, progress, geometry, close and checksum in each caller.
- [ ] Run the four family executor targets. Confirm their existing first/last literal write frames, including N83M's nonzero image base, and their write failure/cancellation scripts still pass; add the missing last-chunk assertion if any script only pins the first chunk.
- [ ] Update architecture and P26 triage; run Gazelle and global gates, review the scoped diff and commit named paths. Open the P26 PR when authorized and wait for its merge before Task 4.

### Task 4: P84 Denso best-effort transfer exit

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`, `denso_iso15765_can_common.cpp`, `denso_iso15765_can_common_test.cpp`
- Modify four Denso bootloader executor `.cpp` files and matching `*_test.cpp` files listed in Task 1
- Modify: `docs/design-notes.md`, `docs/superpowers/specs/2026-10-14-flash-duplication-triage.md`; generated `src/backend/flash/ecu/BUILD.bazel` only if Gazelle changes it

**Interfaces:**
- Consumes: `CanExecutorContext`, its UDS client, cancellation and event sink.
- Produces: `void DensoIso15765BestEffortTransferExit(const CanExecutorContext& ctx)`.

- [ ] Add `DensoIso15765CanCommonTest.TransferExitRetriesSixTimesAndSwallowsReturnedErrors`: script six literal 37 writes and 200 ms reads, including negative, transport and returned-cancellation errors; assert a sixth `77` stops sends, exhaustion returns, and exact opening/success logs including `Closed succesfully: 77`. Add `TransferExitWithCancelledTokenDoesNoLaterIo` asserting no later wire I/O.
- [ ] Run the focused common test filter; expect failure while the helper is missing.
- [ ] Implement the exact common helper with six re-sent 37 requests and the existing 200 ms UDS policy. Swallow each failed result; keep responsePending re-reads within one request and preserve the current logs and spelling.
- [ ] Run the common helper target; expect pass.
- [ ] Replace each reflash block's close retry loop with the helper. Keep its caller-owned 100 ms cancellable settle and checksum request, timeout and result handling.
- [ ] Run all four family executor targets, especially `CloseRetriesSettleAndChecksumOutcomes`; assert sixth-attempt success, six-attempt exhaustion, returned error tolerance and the settle/checksum order. Add a cancellation-at-settle assertion if absent.
- [ ] Update architecture and P84 triage; run Gazelle and global gates, review the scoped diff and commit named paths. Open the P84 PR when authorized and wait for its merge before Task 5.

### Task 5: P62 two-family Denso jump continuation

**Files:**
- Modify: `src/backend/flash/ecu/denso_iso15765_can_common.h`, `denso_iso15765_can_common.cpp`, `denso_iso15765_can_common_test.cpp`
- Modify: `src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.cpp`, `subaru_denso_sh72531_can_executor.cpp` and their matching `*_test.cpp` files
- Modify: `docs/design-notes.md`, `docs/superpowers/specs/2026-10-14-flash-duplication-triage.md`; generated `src/backend/flash/ecu/BUILD.bazel` only if Gazelle changes it

**Interfaces:**
- Consumes: `CanExecutorContext`, caller-selected session, successfully read `std::optional<bytes::Bytes>` first reply, and caller-selected 50 bench or 10 in-car attempts.
- Produces: `Status DensoIso15765WaitForJumpAck(const CanExecutorContext& ctx, bytes::Byte session, std::optional<bytes::Bytes> first_reply, int attempt_count)`.

- [ ] Add `DensoIso15765CanCommonTest.JumpWaitChecksCurrentReplyBeforeEachSleep`: immediate `50 42` acknowledgement returns without sleep/read; a late `50 62` succeeds with exactly one 100 ms sleep and one 200 ms receive. Add `JumpWaitFinalReceiveIsNotChecked` for both 50 bench and 10 in-car attempts where the final receive returns a matching acknowledgement: assert the existing exhaustion error log and success result. Add `JumpWaitPropagatesSleepAndReceiveErrors` for each error kind.
- [ ] Run the focused common test filter; expect failure while the helper is missing.
- [ ] Implement the exact helper, preserving check → 100 ms cancellable sleep → 200 ms receive on every attempt and tolerated exhaustion. Add no initial read, attempt-count selection or timeout parameter.
- [ ] Run the common helper target; expect pass.
- [ ] In only N83M 4M and SH72531, pass the successfully read optional reply into the helper after the caller's jump request, first read and bench-only discarded read/50 ms wait. Leave N83M 1.5M and SH72543 diesel untouched.
- [ ] Run N83M 4M and SH72531 executor targets, particularly `BenchJumpExhaustionPreservesReadCountAndTiming` and `InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy`; assert exact 50/10 attempts, final read order, cancellation and log. Add any missing first/late-ack family case.
- [ ] Update architecture and P62 triage; run Gazelle and global gates, review the scoped diff and commit named paths. Open the P62 PR when authorized.

## Final series check

- [ ] After all five PRs merge, re-run the production jscpd 4.0.8 scan with the exact command in the [investigation](../specs/2026-10-14-flash-duplication-investigation.md#scope-and-evidence); report the measured result without a pass percentage or hardware qualification claim.
- [ ] Confirm P81, P12, P26, P84 and P62 triage rows point to the enduring contract and any unresolved oracle question remains explicit. Extract enduring knowledge from completed specs/plans before removing them; preserve original contents in delivered Git history as required by `AGENTS.md`.
