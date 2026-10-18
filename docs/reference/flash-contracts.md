# Flash sharing contracts

What each shared flash module covers, what deliberately stays in the family
executors, and the preserved quirks a reader could mistake for bugs. Rationale is
in the [flash design notes](../design-notes.md#share-only-proven-protocol-equivalence);
dialect terms are in the [flash language](../../src/backend/flash/GLOSSARY.md).
Exact timeouts, attempt counts and log text are asserted by each family's
executor tests and the shared module headers; read those rather than restating
them here. Open evidence questions are tracked in
[technical debt](../tech-debt.md#p3-flash-protocol-behaviors-awaiting-external-evidence).
Nothing here establishes hardware qualification.

## Plan predicates

[Plan primitives](../../src/backend/flash/ecu/plan_primitives.h) share only
fixed device shape, operation/image shape and transfer start/length equality.
Callers keep catalog identity, capabilities, diagnostics and validation order,
including unsupported TCU TestWrite and SH7055 Write rejection before invalid
image/kernel errors. Kernel upload arithmetic and erase-region construction keep
their existing owners; family upload padding still differs (six versus 128 bytes).

## Denso BEEF K-Line (MC68, SH7055)

The [BEEF K-Line primitives](../../src/backend/flash/ecu/denso_beef_kline_common.h)
share opcodes, framing, ACK checks, the optional exchange, write-address
encoding, block comparison and programming-voltage enable. The dialect is
separate from BEEF CAN.

- Framing keeps the modulo-65536 length conversion; ACK acceptance is
  deliberately permissive (marker, six bytes, opcode at offset four).
- The optional exchange distinguishes an absent reply from a present-empty one.
  Transport failures and short writes take precedence over later cancellation.
- Stays local: required-response diagnostics, CRC initial writes (their
  cancellation diagnostics differ), fragment accumulation, family CRC decoding,
  image rendering, address policy, startup, erase/commit sequencing and
  capability decoding.
- Preserved quirks: both families enable programming voltage before an
  unchanged-image return; a final Write mismatch logs an error and returns
  success; TestWrite keeps its PASS reporting.

## K-Line read mapping and kernel response policy

Family-owned. Hitachi ECU reads use a `0x100000` wire bias and reconstruct all
512 KiB; Mitsubishi reads start at `0x8000`, synthesize the low 32 KiB as `FF`
and warn. MC68 upload success is an absent frame and a present-empty reply is an
error. MC68 CRC decoding does not validate a checksum; SH7055 checks size and
checksum and rejects its `7F` failure marker.

## Denso ISO-15765 bootloader dialect

The [common module](../../src/backend/flash/ecu/denso_iso15765_can_common.h)
shares ten operations across N83M 1.5M, N83M 4M, SH72531 and SH72543 diesel:
setup-PDU encoding, seed/key, erase, one encrypted B7 page read, one B6 write
request, best-effort transfer exit, the N83M-only in-car opening run, the
N83M 4M/SH72531 jump continuation wait, and the connect prelude (OBK probe,
identity queries, access-method probe and branch selector), and the
single-block `write_memory` body (block-count log, erase, then the family's
own reflash under the write phase), plus the dialect's `FatalQuery` prefix. Its header
documents each operation and why everything else stays local.

Kept local in each executor: read loops, reflash,
the in-car and bench connect arms, complete kernel jumps, close/checksum and
family response policy. In particular:

- Setup PDUs accept only a region, so callers cannot select another SID. Read
  setup failure is tolerated on N83M 4M and fatal on the other three.
- The shared in-car run requests session `0x63`; the SH-family in-car runs send
  `0x03` and stay local.
- Page reads accept short pages: seven encrypted bytes yield four decrypted
  bytes, and progress still advances by the requested 256 bytes. Only the two SH
  families add per-page pacing.
- Programming indexes whole-image encrypted bytes by wire address minus image
  base, including the N83M nonzero bases.
- Transfer exit swallows every returned error, including cancellation; its
  re-sends are distinct from erase's receive-only continuations. It keeps the
  legacy `Closed succesfully` spelling.
- Bench jumps: 4M and SH72531 discard even a matching first reply, wait and read
  again; N83M 1.5M and SH72543 diesel read once and keep their own continuations.
  The final continuation receive is unchecked and exhaustion succeeds.
- Non-fatal identity queries swallow exchange errors, including cancellation;
  a cancelled token still prevents later transport I/O. SH72543 diesel reads
  the ECU ID by DID and keeps that query local.
- The OBK probe's success log is a parameter (`OBK is active` on N83M,
  `OBK is active!` on the SH families), and so is the access-method and selector
  probe timeout (200 ms, 2000 ms on SH72543 diesel). An absent OBK frame or an
  empty PDU in a valid envelope is a miss; a raw empty frame fails envelope
  validation.

Best-effort read-stop stays local because its logs differ, even though the
stop retry policy matches. Stop ignores every exchange failure, including
cancellation.

## M32R SSM exchange

The [M32R SSM module](../../src/backend/flash/ecu/m32r_ssm_exchange.h) provides:

- The ordinary optional exchange, shared by the Subaru Hitachi M32R K-Line ECU,
  Mitsubishi M32R K-Line ECU and TCU Hitachi M32R K-Line executors.
- `SendM32rSsmFrame` / `ReceiveM32rSsmOnce`, shared by the Unisia Jecs M32R
  K-Line and bootmode program executors. These have no after-write
  cancellation checkpoint; the ordinary exchange adds one.
- The offset-four payload-prefix predicate, shared by the two Hitachi executors.
  It does not validate headers, lengths or checksums.

Families keep reply parsing, short-response and prefix-mismatch diagnostics,
required-response diagnostics (`no response from ECU`/`TCU`), probes,
handshakes, timeout and retry policy, acknowledgement tolerance, address mapping,
programming stages and cleanup ordering.

## Subaru TCU CVT

MH8104 and MH8111 share the pure seed/key, ROM encryption and page decryption in
the [CVT common module](../../src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h).
MH8104 keeps raw-channel exchanges and tolerant checksum handling; MH8111 keeps
UDS exchanges and fatal checksum handling.

Hitachi M32R CVT security/session stays separate from MH8111: Hitachi sends
security through `other_uds` on `0x7E0` and returns to `0x7E1`, while MH8111
uses its primary `0x7E1` channel. Their seed-key tables match; payload
encryption tables differ.

The [CVT B6 chunk exchange](../../src/backend/flash/ecu/cvt_b6_chunk_exchange.h)
is the only operation of the CVT B6 chunk dialect. It adds no range guard,
cancellation checkpoint or SID validation. Hitachi closes each block with one
fatal attempt; MH8111 retries, succeeds on the first valid `0x77` and reports
`BadResponse` on exhaustion.
