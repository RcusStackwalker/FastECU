# Flash duplication investigation

Status: proposal confirmed by the user (Q6); design interview complete.
This document does not authorize implementation.

## Scope and evidence

The requested flash subtree is interpreted as `src/backend/flash`. Baseline:
`67f3e074`. jscpd 4.0.8 was installed in a temporary directory; no application
dependency was added. Production scan:

```sh
jscpd src/backend/flash --format cpp,c-header --min-lines 10 --min-tokens 70 \
  --ignore '**/*_test.cpp,**/test/**,**/tests/**,**/testing/**' \
  --reporters json --output /private/tmp/fastecu-jscpd-production-final --silent
```

Result: 171 files, 22,292 lines, 89 clone pairs, 1,582 duplicated lines
(7.10%). These are textual matches, including comments, not proven semantic
equivalence. The report is temporary local evidence, not a committed artifact.
The matching source/header scan including tests and support measured 251 files,
204 pairs and 3,722 duplicated lines (8.63%). Its report is at
`/private/tmp/fastecu-jscpd-all-final/jscpd-report.json`.

## Findings

| Candidate | Evidence | Proposed boundary |
| --- | --- | --- |
| Denso bootloader connection | N83M 1.5M executor lines 269–344 and N83M 4M lines 363–409 form the largest reported match, 76 lines; matched comments contribute | Retain family orchestration; examine named probe/identity operations individually |
| Hitachi ECU/TCU M32R K-Line optional exchange | ECU executor lines 29–62 and TCU lines 45–78 have the same cancellation/write/read body | Candidate for one narrow framed exchange primitive; retain explicit caller timeout and framing |
| BEEF response parsing | SH7058 petrol `ParseBeef` line 128 and diesel line 118 have identical bodies | Candidate pure parser for those two consumers; requires revisiting the common header's deliberate two-consumer exclusion |
| MC68/SH7055 helpers | Framing/exchange and CRC-read prefixes match | Investigate complete operation semantics before selecting an extraction |
| CVT Mitsubishi MH8104/MH8111 | Write orchestration and executor setup match textually | Retain local: raw-channel versus UDS handling and checksum failure policy differ |

Paths above refer to executors under `src/backend/flash/ecu`.
The [complete pair inventory](2026-10-14-flash-duplication-triage.md) covers
P01–P89 once each, with enclosing responsibilities, differences, and evidence
requirements. The BEEF CAN parser is an additional inspected candidate; it
must not be represented as an extra pair in the scanner's 89-pair baseline.

## Proposed cohesive approach

Organize sharing around named, proven operations within a protocol boundary.
Keep plans, family state transitions, retries, address translation, erase rules,
and result policy explicit in each family. Prefer existing common modules over
another general utility layer. Share pure parsing first, then narrowly bounded
exchange primitives only with independently specified behavioral evidence.

For each extraction, compare complete behavior: bytes and framing, timeout and
delay, cancellation checkpoints, absent/malformed replies, transport failures,
logs and returned errors. Retain independent family transcript expectations;
do not generate expected wire bytes through the extracted implementation.
Treat behavioral differences as potentially accidental. Investigate them against
the user's designated reference sources: `~/claude-hobby/externals/RomRaider/`
and `~/claude-hobby/ecuflash-disassembly/`. Record the exact reference revision
or binary identity, file/function, applicable family and operation, and what
behavior it establishes. A source covering another family or only logging
cannot settle a flash-protocol difference. Missing or conflicting evidence
leaves the difference unresolved rather than legitimizing normalization.
Preserve current behavior during extraction; propose evidence-backed corrections
separately with a reproducing test and qualification divergence note, following
the [correction contract](../../design-notes.md#corrections-require-local-evidence).
Re-run the identical scan to measure the result, but do not
use a duplication percentage as the correctness criterion.

The [current sharing contract](../../design-notes.md#share-only-proven-protocol-equivalence)
already requires this approach. The BEEF common header additionally says a
two-way parser match stays local. Q2 permits revisiting that historical
restriction: reconcile the header's rationale when a proven extraction is
implemented. No implementation/contract discrepancy has been established.
The [flash glossary](../../../src/backend/flash/CONTEXT.md) defines dialect by
proven shared operations, not by a scanner match; no new dialect is proposed.

## Settled decisions

Q1: Prioritize maintainable responsibility boundaries over a duplication target.
Do not assume current family differences are intentional: use RomRaider and
ECUFlash disassembly as the reference evidence when evaluating them.

Q2: Permit proven operations shared by two consumers. Consumer count alone does
not justify keeping duplicate implementations. Name the consumers, establish
equivalent behavior, and retain independent tests. The BEEF parser is eligible
for evaluation under this decision; its historical exclusion must be reconciled
if an extraction is selected.

Q3: Cover all 89 production clone pairs, grouped into cohesive operations, with
staged implementation priorities. Do not treat 89 pairs as 89 independent
refactorings.

Q4: When designated reference sources cannot establish correct behavior, label
the difference unresolved, preserve current family behavior, and allow shared
code around it only where equivalence is established. Uncertainty blocks
normalization, not every extraction.

Q5: Start with proven small extractions and pure plan primitives, then audit
disputed larger flows as a separate stage. Keep demonstrated defect corrections
separately reviewable from behavior-preserving sharing.

## Module responsibilities

| Boundary | Shared responsibility | Caller retains |
| --- | --- | --- |
| Existing BEEF CAN common module | Response envelope parsing for the two proven consumers | Exchange, raw-frame diagnostics, timeout and reply tolerance |
| Existing Mitsubishi CVT common module | Identical seed/key and payload-transform operations bound to its shared tables | Session setup, addressing, raw-channel versus UDS behavior, checksum outcome |
| Unisia SSM helpers | Framed send and single receive with existing cancellation/error semantics | Bootmode/program stages, SID parsing, erase and write sequencing |
| Hitachi/Mitsubishi M32R K-Line helpers | Proven optional framed exchange and narrow response predicates | Explicit timeout, ECU/TCU diagnostics, TCU block pacing, handshake and address bias |
| Older Denso K-Line kernel helpers | Proven framing/opcodes and optional raw exchange | CRC assembly/validation, settle/timeout values, boot entry, upload and flash policy |
| Existing plan primitives | Identical device/image/transfer invariants | Catalog binding, operation support, geometry, kernel padding unit, confirmations and rejection order |

New helpers are package-owned portable code using bytes and existing injected
ports. They add no Qt, owned execution context, filesystem access, public
executor hierarchy, or generic family state-machine interface. Keep separate
helper entry points where cancellation checkpoints or response representation
differ; do not replace those differences with a bag of behavioral switches.
Exact helper signatures and managed BUILD changes belong to later implementation
planning, after the design is confirmed.

## Reference-source applicability

The local RomRaider checkout is at
`dafe0c36c1a68efadbeedb2825f3855463fdbc35`. Its SSM protocol, checksum, and
response code can corroborate SSM framing. Initial inspection did not locate
flash executors for the candidate families; `flashmethod` metadata does not
establish execution behavior.

The ECUFlash flat binary contains relevant family and flash diagnostics strings.
Its existing decompilation excerpts do not yet map them to the disputed
operations. A string cross-reference, containing-function analysis, and
family-selection trace are required before treating it as a verdict. Existing
scripts describe an x86 little-endian 32-bit program loaded at `0x00400000`.
No disputed difference has yet been adjudicated by this binary.

The examined `ecuflash_flat_5630.bin` has SHA-256
`db807afd2c9bac193c50f8333baa422097200b54c74c1dea285f0aa40163afa4`.

## Proposed sequence and acceptance

1. Extract proven pure operations and bounded exchange primitives within their
   protocol boundary. Candidates include BEEF CAN response parsing, identical
   Mitsubishi CVT crypto wrappers in their existing common module, Unisia SSM
   send/receive, Hitachi ECU/TCU K-Line optional exchange, and older Denso
   K-Line framing/exchange. These remain separate responsibilities: sharing a
   byte marker does not establish interchangeable CAN and K-Line envelopes.
2. Consolidate repeated plan validation only where its invariant is identical.
   Retain family capability, geometry, kernel-address, and confirmation policy.
3. Audit differences in larger bootloader, upload, CRC, read, and write flows
   against applicable reference-source functions. Record each outcome as
   equivalent, intentional difference, demonstrated defect, or unresolved.
   Extract further named operations only after this audit establishes a seam.

For any later implementation, focused shared-operation and family executor
tests must pin successful exchanges, malformed/absent replies, transport errors,
short writes, cancellation checkpoints, and applicable timing. Expected frames
remain independent of shared implementations. Run focused Bazel tests during
each change, regenerate managed BUILD files, and complete the applicable full
release-test, formatting, Gazelle, and static-analysis gates before delivery.
Qualification evidence and hardware status do not improve through refactoring.

Do not add a new jscpd CI threshold in this proposal: preserve the reproducible
scan as a measurement, and judge each extraction by responsibility and behavior.
Keep suspected defect fixes separately reviewable from mechanical sharing.

## Confirmation

Q6: The user confirmed this complete proposal. No substantive design questions
remain open. Implementation planning and product code changes require a
subsequent request. No new domain term or dialect was introduced, so
the glossary remains unchanged. No ADR is proposed: this applies the existing
sharing/correction contracts and revisits a reversible extraction restriction.
