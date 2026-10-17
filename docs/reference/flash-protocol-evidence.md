# Flash protocol evidence

This reference owns unresolved external protocol questions and the scope of the
flash duplication measurements. The [sharing and correction decisions](../design-notes.md#share-only-proven-protocol-equivalence)
own implementation boundaries; the [qualification matrix](../flash-qualification-matrix.md)
owns hardware status. Characterization establishes current behavior, not protocol
correctness. These questions do not represent pending authorized extractions.

## Reference sources and applicability

The inspected local RomRaider checkout at `~/claude-hobby/externals/RomRaider/`
is revision `dafe0c36c1a68efadbeedb2825f3855463fdbc35`.
`src/main/java/com/romraider/io/protocol/ssm/iso9141/SSMProtocol.java`, with its
checksum and response code, corroborates SSM framing. No candidate-family flash
executor was located. `flashmethod` metadata cannot establish flash execution,
retry, timing, address-window or erase behavior.

The inspected ECUFlash artifact in `~/claude-hobby/ecuflash-disassembly/` is
`ecuflash_flat_5630.bin`, SHA-256
`db807afd2c9bac193c50f8333baa422097200b54c74c1dea285f0aa40163afa4`.
Existing scripts describe x86 little-endian 32-bit code loaded at `0x00400000`.
Family/kernel diagnostic strings and family-name/constructor references exist,
but `decomp_batch.txt` does not map the disputed flash operations and callers.
Neither strings nor missing decompilation establish equivalence or a defect.
Before using this binary as an oracle, trace string cross-references to containing
functions and family-selection callers, and record applicable family, operation,
bytes and outcomes. Timestamped family captures may supply the missing wire
evidence. No disputed difference below has an independent verdict from these
sources.

## K-Line capability, verification and transfer questions

For Denso MC68 and SH7055, independently map capability-response layouts and
markers before changing decoding or SH7055 offset-six length diagnostics
(historical investigation P55). Map final verification outcomes before changing
Write success after a logged mismatch or TestWrite PASS reporting (P57).
Their [common primitives](../../src/backend/flash/ecu/denso_beef_kline_common.h)
and family executor tests preserve these behaviors; shared comparison is not
verification evidence.

The remaining K-Line questions require family-specific functions or captures:

- Hitachi TCU block-read pacing (P06): timestamp the write, 100 ms settle,
  500 ms read and 100 ms settle, including five-attempt exhaustion. The TCU
  executor tests `PacesEachBlockReadAsWriteThenDelayThenReadThenDelay` and
  `FailsWhenFiveConsecutiveBlockReadsProduceNoFrame` pin current ordering.
- Hitachi versus Mitsubishi ECU address windows (P19): establish why Hitachi
  sends `A0 00 00` with `0x100000` bias and returns 512 KiB, while Mitsubishi
  sends `A0 00 20` from `0x8000` and synthesizes the low 32 KiB as `FF` with a
  warning. `ReadsAt38400ProbeAndReturnsLogicalFullRom` and
  `ReadsAllUserspaceChunksAndSynthesizesBootPrefix` pin complete output and
  4096 versus 3840 chunks.
- MC68 stock/Ecutek kernel upload (P48): establish silence/error reply meaning.
  The single `UploadKernel` path accepts absence but rejects present-empty
  before kernel-ID traffic. `PresentEmptyUploadFrameIsNotNoFrameSuccess`
  and the stock/Ecutek upload transcripts pin the distinction.
- MC68 versus SH7055 CRC framing (P49): obtain raw kernel replies and mapped
  checksum, declared-length, prefix and failure-marker handling. MC68 accepts
  ten accumulated bytes without checksum validation; SH7055 validates length
  and checksum, strips a prefix and rejects its `7F` marker. Both retain up to
  twenty 50 ms continuation reads with 100 ms settles. Family fragmented,
  empty-initial, truncated and checksum/marker regressions pin these differences.

See the [K-Line ownership contract](../design-notes.md#flash-architecture) and
[package-owned executor tests](../../src/backend/flash/ecu/).

## Denso ISO-15765 read questions

For N83M 1.5M, N83M 4M, SH72531 and SH72543 diesel, no mapped family page-read
oracle establishes the B7 timeout, response policy or short-page acceptance
(P81). The shared operation uses a 2000 ms initial read and whole-word decryption:
seven encrypted bytes yield four decrypted bytes. Family loops advance progress
by the requested 256 bytes and retain padding. N83M 4M tolerates absent or wrong
setup replies; both SH families pace pages by 1 ms, while neither N83M does.
Independent evidence is required before changing these policies or stop-error
precedence. The common helper tests and four-family
`ShortPagesAdvanceByRequestedSizeAndStopIsBestEffort` and
`PageAbsentEmptyAndCancelledFailWithoutStopOrProgress` scripts pin current behavior.
The [read contract](../design-notes.md#share-only-proven-protocol-equivalence)
retains setup, pacing, progress and stop policy with callers.

## CVT security and transfer questions

Map Hitachi M32R and Mitsubishi MH8111 security/session functions and callers
(P11). Hitachi uses `other_uds` on `0x7E0` within dual addressing; MH8111 uses
primary `0x7E1`. Their current seed-key tables and stock transformation are
identical; payload encryption tables differ.
`SecurityUsesLiteralStockVectorsOnFamilyAddress` characterizes the bindings,
not interchangeable session policy.

Map CVT reflash reply acceptance and transfer-close retries (P12). The
[CVT B6 exchange](../../src/backend/flash/ecu/cvt_b6_chunk_exchange.h) accepts
silence and any successfully received framed PDU, propagating transport errors.
Hitachi closes once fatally; MH8111 tries up to twenty times and fails on
exhaustion. Family `WriteAcceptsSilentChunkReplies`,
`WritePropagatesChunkTransportError`, `WriteCloseExhaustionIsFatal` and MH8111
`WriteCloseCanSucceedOnTwentiethAttempt` tests preserve these boundaries.

## Denso connection and programming questions

For the four ISO-15765 families, map complete jump and caller traces before
changing discarded reads, timing or tolerated exhaustion (P22/P61/P62/P73/P74).
N83M 1.5M and SH72543 diesel read once before continuation; N83M 4M and SH72531
discard even a matching first bench reply, wait 50 ms and read again. Diesel's
initial read is 2000 ms, with 200 ms bench or 2000 ms in-car continuation;
other families use 200 ms. All tolerate 50 bench or ten in-car exhausted waits
with 100 ms sleeps. The shared 4M/SH72531 continuation deliberately leaves its
final receive unchecked. Common literal tests and family
`BenchJumpExhaustionPreservesReadCountAndTiming` and
`InCarJumpExhaustionAndCancellationPreserveTimeoutPolicy` pin these observations.

Map identity and OBK probe functions with surrounding callers (P24/P64).
SH72531 uses AA ECU ID; diesel uses `22 F1 82` and a custom parser. Identity
exchange errors, including returned cancellation, are swallowed; real token
cancellation still prevents subsequent I/O. OBK mismatch, absence and empty PDU
are misses; envelope, transport and cancellation errors propagate. Family
`IdentityExchangeFailuresIncludingCancellationAreNonFatal`,
`ObkMismatchAbsentAndEmptyAreMisses` and
`ObkErrorsAndCancellationPropagateBeforeInitialization` tests characterize this.

Map reflash addressing, close and checksum callers (P26/P67/P84/P85) before
changing address arithmetic, reply acceptance or result policy. Whole-image
encrypted bytes are indexed by absolute wire address minus image base, including
nonzero N83M origins. Independent literal first/last complete B6 frames supplement
family full-image sweeps. Transfer exit retransmits `37` at most six times with
200 ms reads and swallows returned errors, including cancellation; real token
cancellation prevents later I/O. Callers retain a cancellable 100 ms settle even
after exhaustion. These re-sends differ from erase receive-only continuations.
Checksum accepts immediate positives and pending/final replies; initial/pending
timeouts are 500/500 ms (1.5M), 500/3000 ms (4M), and 2000/2000 ms (both SH
families). The legacy pending-first discrepancy remains unresolved externally.
Common helper tests, [UDS exchange tests](../../src/backend/flash/ecu/uds_client_exchange_common_test.cpp)
and family `CloseRetriesSettleAndChecksumOutcomes` scripts pin current behavior.
The [Denso common module](../../src/backend/flash/ecu/denso_iso15765_can_common.h)
shares only the named bounded operations defined by the architecture contract.

## Reproducible duplication measurement

Use jscpd **4.0.8**, installed temporarily rather than as an application
dependency, from the repository root. Scope is production source and headers
under `src/backend/flash`, excluding tests and support with the exact patterns:

```sh
jscpd src/backend/flash --format cpp,c-header --min-lines 10 --min-tokens 70 \
  --ignore '**/*_test.cpp,**/test/**,**/tests/**,**/testing/**' \
  --reporters json --output /private/tmp/fastecu-jscpd-production-final --silent
```

| Revision | Files | Lines | Clone pairs | Duplicated lines | Percentage |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline `67f3e074` | 171 | 22,292 | 89 | 1,582 | 7.10% |
| Final series `d4afd419` | 177 | 22,065 | 74 | 1,222 | 5.54% |

The final report was inspected at
`/private/tmp/fastecu-jscpd-production-final/jscpd-report.json`; temporary reports
are not repository artifacts. These dated measurements count textual matches,
including comments, and do not prove semantic equivalence. There is no percentage
gate. Neither the measurements nor automated transcripts establish hardware
qualification. Corrections require independent applicable evidence, a reproducing
test and a separate [correction decision](../design-notes.md#corrections-require-local-evidence).
