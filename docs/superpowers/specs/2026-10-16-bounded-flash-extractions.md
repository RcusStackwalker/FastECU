# Bounded flash extraction follow-ups

Status: active, partially implemented series. The design was approved by the
user on 2026-10-16. P81 is implemented and reviewed; the remaining four
extractions are tracked in the
[implementation plan](../plans/2026-10-16-bounded-flash-extractions.md).
This series does not authorize flash-behavior corrections.

## Purpose and scope

Design the five bounded extraction follow-ups left by the
[remaining flash duplication triage](2026-10-14-flash-duplication-triage.md)
after PR #654: P81 encrypted Denso page read, P12 CVT chunk exchange, P26 Denso
B6/address/chunk request, P62 Denso jump continuation wait, and P84 Denso
transfer-exit retry. Each boundary must be independently reviewable. A candidate
can be retained locally if its complete operation is not equivalent; the goal
is maintainable responsibility boundaries, not a duplication percentage.

The [current sharing contract](../../design-notes.md#share-only-proven-protocol-equivalence)
and [flash language](../../../src/backend/flash/CONTEXT.md) govern this design.
Family executors retain their plans, state transitions, address translation,
timing, diagnostics, and result policy outside each proven shared operation.
Automated transcripts do not change the [qualification status](../../flash-qualification-matrix.md).

## Evidence boundary for P81

The four FastECU family implementations and their independent executor scripts
establish the same bounded B7 request, UDS handling, and whole-word decryption.
The checked RomRaider revision has no mapped flash implementation for these
families. The examined ECUFlash binary has not been mapped to a family-specific
page-read function or caller. Neither source independently establishes the
timeout, response policy, or short-page acceptance. This limits P81 to a
behavior-preserving extraction; it does not justify changing the protocol or
claiming qualification. See the
[investigation's reference-source record](2026-10-14-flash-duplication-investigation.md#reference-source-applicability)
and [remaining evidence limits](2026-10-14-flash-duplication-triage.md#evidence-limits).

## Confirmed decisions

1. Cover all five follow-ups in this design interview. Describe each as an
   independent, reviewable change rather than one combined extraction.
2. For P81, extract only the common B7 page request, response handling, and
   whole-word decryption. Preserve each family's setup acceptance, read loop,
   progress, 1 ms pacing where present, stop diagnostics, padding, and current
   short-page behavior. Treat any setup or pacing correction as separate
   evidence-backed work.
3. For P12, share one CVT B6 chunk send/receive exchange. Preserve acceptance
   of an absent reply or any validly framed reply. Callers retain their
   cancellation check, encryption, geometry, progress, setup, close, and
   checksum policies.
4. For P26, share the bounded Denso B6 request and response operation with its
   500 ms read policy and current fatal diagnostics. Callers choose already
   encrypted bytes and calculate the absolute address from their image base.
5. For P62, first serve only the N83M 4M and SH72531 matching jump
   continuation tails. Callers retain initial reads and attempt counts. Do not
   parameterize this extraction to cover the other two families yet.
6. For P84, share the six-attempt transfer-exit retry only. Keep the 100 ms
   settle and each family's checksum handling with callers. Preserve swallowed
   retry errors and current cancellation behavior.
7. Deliver each accepted extraction as a separate pull request with independent
   family transcript expectations.
8. Once P12 is implemented and independently tested, name its two consumers
   the **CVT B6 chunk dialect** in the
   [flash language](../../../src/backend/flash/CONTEXT.md). Use a dedicated
   CVT B6 exchange module; the Mitsubishi CVT crypto module retains its
   current scope.
9. Deliver the PRs in this order: P81, P12, P26, P84, P62. A candidate that
   fails its equivalence gate closes as retained local code with the reason
   recorded, rather than growing a configurable state machine.

## Proposed operation boundaries

All helpers live in `src/backend/flash/ecu`, consume caller-owned context or
ports, and use portable byte types. The signatures below fix their public
boundary. Expected wire bytes in executor tests remain literal and independent
of these helpers.

### P81: one encrypted Denso page

Extend `denso_iso15765_can_common.{h,cpp}` with the already proposed
`Result<bytes::Bytes> ReadDensoEncryptedPage(const CanExecutorContext& ctx,
std::uint32_t address)`. It composes B7 and the four-byte big-endian address,
uses the existing fatal request diagnostic and UDS policy (2000 ms initial
read, existing pending behavior), then decrypts the response payload in whole
four-byte words. It returns the decrypted bytes or the unchanged error.

The four callers retain setup acceptance and timeout, loop cancellation,
address-window selection, append, requested-size progress, SH-family 1 ms
sleep, best-effort read stop, and image padding. The helper adds no page-size
check: seven encrypted bytes still produce four decrypted bytes.

### P12: one CVT B6 chunk exchange

Add `cvt_b6_chunk_exchange.{h,cpp}` with:

```cpp
Status ExchangeCvtB6Chunk(uds::IUdsChannel& channel,
                          const ICancellationToken& cancellation,
                          std::uint32_t address,
                          bytes::ByteView encrypted_chunk);
```

It sends B6 plus `U24(address)` and the chunk, then calls the channel's
`Receive` once at 2000 ms. The caller supplies an address within its existing
wire window; the helper adds no new range check. An absent reply or any
successfully received PDU succeeds without inspecting its
content; send and receive errors propagate unchanged. The helper adds no
pre-send cancellation checkpoint or response-SID validation.

Hitachi and MH8111 retain their own top-of-loop cancellation check, channel
binding, encryption tables, block setup, 128 versus 256-byte chunks, address
range, progress, transfer close and checksum. Do not place this cross-family
exchange in the Mitsubishi-only crypto module or the generic UDS client.

### P26: one Denso B6 write request

Extend the Denso ISO-15765 common module with:

```cpp
Status DensoIso15765WriteChunk(const CanExecutorContext& ctx,
                              std::uint32_t address,
                              bytes::ByteView encrypted_chunk);
```

It composes B6 plus the four-byte big-endian absolute wire address and
chunk, then uses the same fatal UDS request path, 500 ms initial read policy,
and address-bearing diagnostic as the four current callers. It propagates
the same error and discards a successful reply.

Each caller retains whole-image encryption, the write image's ECU origin,
chunk index, 256-byte chunk selection, loop cancellation, progress, block geometry,
close and checksum. The helper neither indexes the image nor adds validation
to the established address window.

### P84: best-effort Denso transfer exit

Extend the Denso ISO-15765 common module with:

```cpp
void DensoIso15765BestEffortTransferExit(const CanExecutorContext& ctx);
```

It owns the current six re-sent transfer-exit requests. Each request uses the
existing 200 ms UDS policy; the first success stops the loop. The operation
keeps the current opening and success logs, including the success line's
spelling and PDU hex.
Every failed request is swallowed, including a returned cancellation error;
exhaustion also returns without an error. UDS responsePending re-reads within
one request remain distinct from the six re-sends.

The caller performs the 100 ms cancellable settle even after exhaustion, then
uses its own checksum request and timeouts. A genuinely cancelled token still
prevents later wire I/O and can fail that settle. No erase re-read loop is
included.

### P62: two-family Denso jump continuation

Extend the Denso ISO-15765 common module with:

```cpp
Status DensoIso15765WaitForJumpAck(const CanExecutorContext& ctx,
                                  bytes::Byte session,
                                  std::optional<bytes::Bytes> first_reply,
                                  int attempt_count);
```

The caller has already handled any transport error from the initial read and
passes its optional reply. Only N83M 4M and SH72531 call it. On each attempt,
it accepts a reply of at least two bytes starting `50` plus the session byte;
otherwise it
sleeps 100 ms and receives once at 200 ms. Sleep and receive errors propagate.
After 50 bench or ten in-car unsuccessful attempts it emits the existing error
log and returns success. Preserve the current ordering in which the last
receive is not checked again after the final attempt.

The callers retain the jump request, first read, bench-only discarded first
reply and 50 ms wait, session choice, and 50/ten attempt selection. N83M 1.5M
and SH72543 diesel retain their own jump paths; this helper takes no general
timeout or family-mode switches.

## Evidence and acceptance

Before each move, a focused helper test and the affected family suites must
pin the operation's observable result, literal bytes, I/O order, timing,
cancellation and error precedence. The existing family scripts from PR #654
remain the independent baseline. Add only missing cases that distinguish the
proposed boundary:

| PR | Focused regression evidence |
| --- | --- |
| P81 | B7 address, short whole-word decrypt, absent/empty reply, transport error and cancellation; run all four read suites, including pacing and padding scripts. |
| P12 | Literal B6/U24 frame, silence, uninterpreted present PDU, send/receive errors; run both CVT write suites and their distinct close scripts. |
| P26 | Literal first/last B6 frames, fatal negative/transport/cancellation diagnostics; run all four full-image write scripts, including nonzero N83M image bases. |
| P84 | Sixth-attempt success, six-attempt exhaustion, swallowed negative/transport/returned-cancellation errors; run all four family settle/checksum scripts. |
| P62 | First/late acknowledgement, exact 50/ten attempt timing and final-read ordering, sleep/receive errors; run both family bench and in-car scripts. |

Each PR regenerates managed BUILD files, updates the owning
[flash architecture](../../design-notes.md#flash-architecture) and closes its
extraction row in the [triage](2026-10-14-flash-duplication-triage.md). Preserve
unresolved external correctness evidence in the owning contract or triage
without presenting it as hardware qualification. Run focused Bazel tests during
the change and the applicable release build, full test, formatting, Gazelle,
static-analysis, platform CI and packaging gates before delivery. After the
series, re-run the investigation's identical jscpd scan as a measurement;
there is no target percentage.

Protocol behavior changes require independent family-specific evidence, a
reproducing test and a separate correction decision. The RomRaider and
ECUFlash limits recorded above apply to P81; the other four candidates also
lack mapped ECUFlash family and caller evidence in the
[triage](2026-10-14-flash-duplication-triage.md#evidence-limits).

## Outstanding correctness question

For P81, the stop-response log affects diagnostics only. The N83M 4M setup's
tolerance of absent or wrong replies and the SH families' 1 ms inter-page delay
affect observable protocol behavior. Whether those behaviors are correct
remains unresolved without independent family evidence.
