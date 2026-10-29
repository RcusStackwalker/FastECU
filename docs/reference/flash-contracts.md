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
existing path. Direct Colt Execute callers must supply the guard.

Colt's read and comparison loops report cancellation through the next exchange,
including its address in the warning. A precancelled direct Execute call is
refused in plan admission with `cancelled before setup`, before any events or
I/O; an invalid plan still reports its validation error first. An accepted
erase reply can produce its completion event before the next request is refused.
Complete ROM data and verification failures retain their meaning after late
cancellation. Family abort returns and confirmations remain separate owners.

The [pilot results](colt-cancellation-centralization-results.md) own measurements,
preserved cases and mutation evidence. This is a Colt-only ownership change;
test compression needs separate assessment and expansion needs reviewed results
and the recorded Colt bench checklist. Hardware status remains unchanged.

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
fixed device shape, operation/image shape, transfer start/length equality and
the exact protocol-id lookup with its MCU check (`FindCatalogEntry`). Callers
keep their catalog rows, capabilities, diagnostics and validation order,
including unsupported TCU TestWrite and SH7055 Write rejection before invalid
image/kernel errors. The four Denso SH705x kernel-loaded builders share the rest
of their plan (kernel fit, image rules, transfer, erase-all, build and family
validation) through `BuildWholeRomKernelPlan`, and their validators re-check the
derived parts through `ValidateWholeRomKernelPlan`; family upload padding still
differs (six versus 128 bytes) and is the template argument of both.

## Denso BEEF CAN kernel

[BeefCanKernel](../../src/backend/flash/ecu/beef_can_kernel.h) owns three
operations used directly by the Subaru SH7058 petrol ECU, SH7058 diesel ECU and
SH705x TCU executors: kernel payload transfer, ROM read and ROM write. It borrows
the caller's ports and profile; execution context, lifetimes and transport
lifecycle remain with callers. The cold
[session](../../src/backend/flash/ecu/beef_can_session.h) retains tolerant
identity/session queries and kernel-ID handling, without forwarding warm work.

- `TransferPayload` validates, pads, checksums and encrypts the kernel, sends
  `34 04 33` with the low 24 address bits and checked 24-bit encrypted payload
  length, both big-endian, then sends 128-byte B6 blocks and the final
  address-only B6. Each block has one raw reply read; content and nonterminal
  adapter outcomes are discarded, while cancellation/disconnection terminate.
  The caller supplies the existing download exchange policy and block timeout;
  the diesel's timings remain distinct from petrol/TCU.
- `Read` requests fixed 0x400-byte raw BEEF pages, checks each reply and bounds
  the appended tail and progress to the requested region. No normal-ROM BEEF
  read decrypts the payload. Per-page clock sampling keeps the 1 ms elapsed
  floor and existing rate logs.
- `Write` owns CRC comparison and its raw stale-frame drain, flash init,
  voltage query, erase, 0x200-byte buffers, 0x1000-byte validate/commit windows,
  recovery warnings and final comparison. Unchanged images skip flashing;
  ECU TestWrite selects disable/validate rather than enable/commit. Existing
  address-window guards, cancellation boundaries and final-mismatch reporting
  remain intact. The existing profile keeps wording, init timeout and hex
  formatting: its uppercase-size setting also preserves TCU uppercase write
  addresses and padded CRC logs, without selecting a different wire algorithm.

Cold paths and post-upload handoffs stay in the family executors. Petrol and
diesel retain their own identity/security/session sequences, transfer-exit and
kernel-start policies and strict kernel-ID checks. The TCU retains its separate
probe/identity/session/security sequence and kernel-ID continuation/drain logic;
after kernel start it resets, configures and opens through `RestartIso15765`,
waits 500 ms cancellably, then probes strictly. The outer attempt still owns the
initial lifecycle and final close. Plan admission still rejects TCU TestWrite
and SH7055 Write. These distinct cold paths and handoffs do not imply different
warm read/write wire behavior.

The unchanged family scripted wire suites and plan goldens are the preservation
contract. Hardware status remains in the
[qualification matrix](../flash-qualification-matrix.md); this sharing establishes
no bench or vehicle qualification.

## Denso BEEF K-Line (MC68, SH7055)

The [BEEF K-Line primitives](../../src/backend/flash/ecu/denso_beef_kline_common.h)
share opcodes, framing, ACK checks, the optional exchange, write-address
encoding, the block CRC request (frame, guarded write, 3000 ms first read),
block comparison, programming-voltage enable and the write sequence
(`DensoBeefKlineWriteChangedBlocks`: compare, voltage enable, flash init, a
200 ms programming-voltage query and reflash per changed block, re-compare). The dialect is
separate from BEEF CAN.

- Framing keeps the modulo-65536 length conversion; ACK acceptance is
  deliberately permissive (marker, six bytes, opcode at offset four).
- The optional exchange distinguishes an absent reply from a present-empty one.
  Transport failures and short writes take precedence over later cancellation.
- Stays local: required-response diagnostics, CRC continuation reads and
  decoding (the CRC request's cancellation wording is normalized), fragment
  accumulation, image rendering, address policy, startup, erase/commit
  sequencing and capability decoding.
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

MH8111 and the Subaru Hitachi M32R CAN ECU share the fatal seed/key unlock and
kernel jump (`27 01`, `27 02`, `10 42`) through `UnlockAndKernelJump` in the
[UDS exchange module](../../src/backend/flash/ecu/uds_client_exchange_common.h);
each keeps its exchange policy, rejection wording, seed-key table, session
entry and kernel alive check.

The Subaru Hitachi M32R CAN ECU, Hitachi M32R CVT and MH8111 share the encrypted
B7 page loop (`ReadEncryptedB7Pages` in the
[UDS exchange module](../../src/backend/flash/ecu/uds_client_exchange_common.h):
0x100-byte pages, a cancellation check before each, fatal reads); each keeps its
exchange policy, rejection wording, region, decrypt table, dump setup, stop
command and padding.

The [CVT B6 chunk exchange](../../src/backend/flash/ecu/cvt_b6_chunk_exchange.h)
is the only operation of the CVT B6 chunk dialect. It adds no range guard,
cancellation checkpoint or SID validation. Hitachi closes each block with one
fatal attempt; MH8111 retries, succeeds on the first valid `0x77` and reports
`BadResponse` on exhaustion.
