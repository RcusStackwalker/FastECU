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

## Colt cancellation ownership

[GuardedCanFlashTransport](../../src/backend/flash/guarded_can_flash_transport.h)
borrows the existing CAN port. Read and Write refuse delegation when their
supplied token is cancelled at entry; otherwise bytes, timeout, token and the
delegate's outcome pass unchanged. There is no post-call check, final sweep,
error remapping or persistent stopped state. Cancellation after an accepted
operation can leave that operation successful.

Lifecycle, the inner RestartIso15765 override and RequestUnblock forward without
a new gate. BoundAttempt constructs the executor's nested CancellationGuard
only around Execute, after Open. Preparation, configuration, Open, Close and
unblock remain on the owned transport. Execution failure wins over Close
failure; a Close-only failure is returned. Non-opted-in families retain their
existing path. Direct Execute callers of opted-in executors must supply the guard. Shared CAN
test runners use the executor-selected
[direct-call adapter](../../src/backend/flash/testing/execute_with_cancellation_guard.h).
The [migration results](can-cancellation-migration-results.md#shared-test-support)
record its opt-in and cancellation-blind runner proofs; this support alone does
not migrate another executor.

Colt's read and comparison loops report cancellation through the next exchange,
including its address in the warning. A precancelled direct Execute call is
refused in plan admission with `cancelled before setup`, before any events or
I/O; an invalid plan still reports its validation error first. An accepted
erase reply can produce its completion event before the next request is refused.
Complete ROM data and verification failures retain their meaning after late
cancellation. Family abort returns and confirmations remain separate owners.

The [pilot results](colt-cancellation-centralization-results.md) own measurements,
preserved cases and mutation evidence. Additional explicit opt-ins and their
retained checkpoints are recorded in the
[CAN migration results](can-cancellation-migration-results.md). [ADR 0025](../adr/0025-enforce-cancellation-in-a-guarded-transport.md#per-executor-migration-requirements)
sets the requirements for explicit opt-in by additional CAN and mixed-CAN
executors; it does not opt them in. Test compression needs separate assessment,
and hardware status remains unchanged.

The Denso ISO-15765 bootloader template selects cancellation through an empty
default policy with explicit plan specializations in its shared definition
header. Migrated instantiations and per-family evidence are recorded in the
[Denso migration results](can-cancellation-migration-results.md#denso-iso-15765-n83m-15m).
Shared polls, profile values, best-effort stop/close policy and SH page pacing
remain with their current owners. A completed N83M dump can succeed after
cancellation at its best-effort stop; an SH final-page pacing cancellation
still returns its clock failure.

Hitachi SH7058 CAN explicitly selects the guard while retaining all Session
checkpoints. Delegated Read errors still precede its after-read cancellation
check; successful reads retain `SH7058 write cancelled`. The
[SH7058 evidence](can-cancellation-migration-results.md#hitachi-sh7058-can)
distinguishes preserved family behavior from the synthetic selection proof.

Hitachi SH72543R CAN explicitly selects the guard and retains Send, Receive,
Sleep and finalization checks. Receive still gives cancellation precedence over
a concurrently failed delegated Read. Its
[SH72543R evidence](can-cancellation-migration-results.md#hitachi-sh72543r-can)
records the preserved policy and independent selection proof.

Denso SH705x CAN EEPROM explicitly selects the guard while retaining raw
exchange, kernel chunk and EEPROM page checkpoints. Cancellation after an
accepted B6 Write still stops at `cancelled after kernel chunk write` before
its acknowledgment Read. The
[EEPROM evidence](can-cancellation-migration-results.md#denso-sh705x-can-eeprom)
records the measured predecessor boundary and separate upload characterizations.

## Plan admission

Every flash executor derives from
[FamilyExecutor](../../src/backend/flash/flash_executor.h), which owns
[plan admission](../../src/backend/flash/GLOSSARY.md) once instead of each family
copying it. `TransportSetup` and `Execute` run the same checks in the same order:
`CheckFamily` (`plan family does not match this executor`), then the family's
`PlanValidator`. `Execute` then refuses a cancelled run with
`cancelled before setup`. No event, control line or transport call precedes
admission, so a refused run leaves the ECU and the adapter as it found them.

Family validators do not repeat the family and transport check, because
`ValidateAndBuild` already ties both to the plan's variant. A mode guard (Hitachi
SH7058 CAN is write only, Hitachi SH7058 K-Line is read only) lives in the family's
`PlanValidator` specialization, so `TransportSetup` and `Execute` refuse an
unsupported operation alike. The shared plan builder stays free of the guard. An
invalid plan reports its validation error even when the run is also cancelled.

Later cancellation checkpoints inside a family's `Run` keep their own texts.
Colt's in-flight cancellation stays with the guarded transport described above.

## Plan predicates

[Plan primitives](../../src/backend/flash/ecu/plan_primitives.h) share only
fixed device shape, operation/image shape and transfer start/length equality.
Callers keep catalog identity, capabilities, diagnostics and validation order,
including unsupported TCU TestWrite and SH7055 Write rejection before invalid
image/kernel errors. Kernel upload arithmetic and erase-region construction keep
their existing owners; family upload padding still differs (six versus 128 bytes).

Denso SH7058 BEEF petrol explicitly selects the same CAN guard. Its raw upload
reply discard newly refuses a Read after cancellation from an accepted B6 Write;
all shared polls and family error/tolerance policy remain. The
[petrol migration evidence](can-cancellation-migration-results.md#denso-sh7058-beef-petrol)
records the boundary, measurements and outstanding bench work.

Denso SH7058 BEEF diesel also selects the CAN guard. Its raw B6 discard and
pending UDS Read refuse new delegation after cancellation; the existing CRC
pre-drain checkpoint remains. The
[diesel evidence](can-cancellation-migration-results.md#denso-sh7058-beef-diesel)
records the exact delegated timeout prefix and unchanged pending timeout policy.

Denso SH705x BEEF TCU explicitly selects the CAN guard. Its B6 raw discard
refuses delegation after cancellation from an accepted Write. Kernel-ID drain
bounds/polls, exact session validation, UI preflight and the executor-owned
inner restart keep their existing owners. The
[TCU migration evidence](can-cancellation-migration-results.md#denso-sh705x-beef-tcu)
records the real boundary proof and retained restart integration.

## Denso BEEF CAN kernel download framing

The [BEEF CAN helpers](../../src/backend/flash/ecu/denso_beef_can_common.h) own
the shared kernel-download request: `34 04 33`, then the low 24 address bits and
the checked 24-bit encrypted payload length, both big-endian. Payload validation
precedes serialization; each family retains its reply policy and upload ordering.

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
shares eleven operations across N83M 1.5M, N83M 4M, SH72531 and SH72543 diesel:
setup-PDU encoding, seed/key, erase, one encrypted B7 page read and the read dump built
on it (page loop, best-effort stop and 0xFF padding; SH page pacing is a
parameter), one B6 write request and the image write loop
built on it (whole-image encryption, image-base indexing, whole chunks only),
best-effort transfer exit with the settle-and-checksum close (the
checksum read timeouts are a parameter), the in-car arm with its opening runs (an N83M
run and an SH run; one read timeout, 2000 ms on the diesel, covers the probe,
the confirmations, the run's 0x7E0 reply and the jump), the bench arm (its
settle, session strictness and jump timeouts are parameters), the
kernel jump with its continuation wait (first and loop read timeouts and the
N83M 4M/SH72531 bench discard are parameters), and the connect prelude (OBK probe,
identity queries, access-method probe and branch selector), and the
single-block `write_memory` body (block-count log, erase, then the family's
own reflash under the write phase), plus the dialect's `FatalQuery` prefix. Its header
documents each operation.
The seed/key and payload tables it uses, which the BEEF CAN dialect uses too,
are [shared protocol data](../../src/backend/flash/ecu/denso_iso15765_crypto.h)
in a library of their own.

One [dialect executor](../../src/backend/flash/ecu/denso_iso15765_bootloader_executor.h)
runs all four families in the same order. It owns read setup, the identity
query, the region log lines and the reflash. Every way a family deviates is a
field of its dialect profile, next to its plan type; the fields, with legacy
citations, are listed side by side in
[Denso ISO-15765 family deviations](denso-iso15765-families.md). In particular:

- Setup PDUs accept only a region, so callers cannot select another SID. Read
  setup failure is tolerated on N83M 4M and fatal on the other three.
- The in-car runs differ by one byte: the N83M run asks 0x7E1 for session `0x63`,
  the SH run for `0x03`.
- Page reads accept short pages: seven encrypted bytes yield four decrypted
  bytes, and progress still advances by the requested 256 bytes. Only the two SH
  families add per-page pacing.
- Programming indexes whole-image encrypted bytes by wire address minus image
  base, including the N83M nonzero bases.
- Transfer exit swallows every returned error, including cancellation; its
  re-sends are distinct from erase's receive-only continuations. It keeps the
  legacy `Closed succesfully` spelling.
- Bench jumps: 4M and SH72531 discard even a matching first reply, wait and read
  again; N83M 1.5M and SH72543 diesel read once. The diesel reads its first
  reply at 2000 ms and continues at 2000 ms in car; everything else reads at
  200 ms. The final continuation receive is unchecked and exhaustion succeeds.
- Non-fatal identity queries swallow exchange errors, including cancellation;
  a cancelled token still prevents later transport I/O. SH72543 diesel reads
  the ECU ID by DID and keeps that query local.
- The OBK probe's success log is a parameter (`OBK is active` on N83M,
  `OBK is active!` on the SH families), and so is the access-method and selector
  probe timeout (200 ms, 2000 ms on SH72543 diesel). An absent OBK frame or an
  empty PDU in a valid envelope is a miss; a raw empty frame fails envelope
  validation.

The best-effort read stop ignores every exchange failure, including
cancellation; every family logs the stop response.

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
