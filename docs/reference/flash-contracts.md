# Flash sharing contracts

What each shared flash module covers, what deliberately stays in the family
executors, and the preserved quirks a reader could mistake for bugs. Rationale is
in the [flash design notes](../design-notes.md#share-only-proven-protocol-equivalence);
dialect terms are in the [flash language](../../src/backend/flash/GLOSSARY.md).
Each family's executor tests and the shared module headers assert the timing,
attempt counts and log text behind these contracts. Open evidence questions are
tracked in
[technical debt](../tech-debt.md#p3-flash-protocol-behaviors-awaiting-external-evidence).
Nothing here establishes hardware qualification.

## CAN cancellation ownership

[GuardedCanFlashTransport](../../src/backend/flash/guarded_can_flash_transport.h)
borrows the existing `ICanFlashTransport` port and gates Read and Write.
[GuardedMixedCanFlashTransport](../../src/backend/flash/guarded_mixed_can_flash_transport.h)
borrows `IMixedCanFlashTransport` and gates WriteIso15765, ReadIso15765,
WriteRaw and ReadRaw. Each refuses delegation when its supplied token is
cancelled at entry; otherwise bytes/frame, timeout, token and the delegate's
outcome pass unchanged. There is no post-call check, final sweep,
error remapping or persistent stopped state. Cancellation after an accepted
operation can leave that operation successful.

Lifecycle, the CAN inner RestartIso15765 override, mixed-CAN mode transitions,
buffer control and RequestUnblock forward without a new gate. BoundAttempt
calls the production `ExecuteWithTransportGuard` overload after Open. The
executor interface selects the CAN or mixed-CAN decorator; K-Line receives its
original transport. Preparation, configuration, Open, Close and unblock remain
on the owned transport. Execution failure wins over Close failure; a Close-only
failure is returned.

Ordinary family tests and shared conformance runners call undecorated Execute
to test family-owned checkpoints. Composition cases use BindFlashAttempt to
exercise production guarding. The alias-free CAN, mixed-CAN and K-Line cases in
[binding tests](../../src/backend/flash/bound_flash_attempt_test.cpp) assert
guard selection at this execution boundary. The
[migration results](can-cancellation-migration-results.md#shared-policy-and-test-ownership)
record the surviving shared and family test ownership.

### Cancellation exposure and reporting

The `Error::cancellation_io_exposure` field uses
`CancellationIoExposure::kMayHaveStarted` by default, preserving conservative
reporting and existing two-field aggregate construction.
`kRefusedBeforeDelegation` is positive typed evidence that the represented
operation was not delegated. `kCancelled` remains the outcome kind; exposure is
never inferred from error-detail strings.

CAN and mixed-CAN guards mark entry refusals, and `UdsClient` marks pre-request
refusal while preserving failed-Send provenance. The shared
CAN reporter returns typed pre-delegation refusal quietly, without an event;
may-have-started cancellation, including unknown exposure, retains the existing
ECU safety warning. Non-cancellation rejection logging stays unchanged.
After a successful Send, `AfterIoMayHaveStarted(Error)` promotes cancellation
from delays, Receive and pending rereads to may-have-started, even when a later
call is refused. CVT Hitachi raw erase and MH8104 raw exchange apply the same
promotion so earlier request exposure is retained.
`NonFatalQuery` keeps its direct error logging and tolerance policy; raw paths
without the shared reporter keep their existing propagation policy.

Redundant pre-I/O CAN and mixed-CAN entry and transfer-loop polls defer to
those owners. Only request/frame/address preparation with admitted valid inputs
lies before the next guarded or UdsClient operation. Family-specific entry detail
is normalized to the owner's `kCancelled` refusal. Raw exchanges without the
shared reporter retain their existing error policy.

Family-owned post-operation and non-I/O checkpoints remain in place, including
waits, validation/error precedence, progress and log boundaries, confirmations,
retry bounds and lifecycle checks. Generic refusal, exposure and binding tests
stay with shared owners. Direct undecorated family execution requires a
cancellation-aware transport for entry refusal formerly supplied by those polls;
production binding supplies the guard. This ownership transfer establishes no
hardware qualification.

### Colt cancellation ownership

Colt's read and comparison loops return cancellation through the next exchange;
a refused unsent request is quiet, while may-have-started cancellation retains
its warning with the request address. A precancelled direct Execute call is
refused in plan admission with `cancelled before setup`, before any events or
I/O; an invalid plan still reports its validation error first. An accepted
erase reply can produce its completion event before the next request is refused.
Complete ROM data and verification failures retain their meaning after late
cancellation. Family abort returns and confirmations remain separate owners.

The [pilot results](colt-cancellation-centralization-results.md) own measurements,
preserved cases and mutation evidence. The completed interface ownership and
retained checkpoints are recorded in the
[CAN migration results](can-cancellation-migration-results.md). [ADR 0025](../adr/0025-enforce-cancellation-in-a-guarded-transport.md#durable-completion-contract)
owns the durable completion contract. The earlier conservative rollout retained
polls and predecessor behavior tests; redundant pre-I/O polls now defer to the
shared owners above. Guard selection belongs to the binding. Hardware status
remains unchanged.

### Other CAN family boundaries

Denso SH7058 BEEF petrol uses the binding's CAN guard. Its raw upload
reply discard newly refuses a Read after cancellation from an accepted B6 Write;
post-operation and non-I/O polls and family error/tolerance policy remain. The
[petrol migration evidence](can-cancellation-migration-results.md#ownership-matrix)
records the boundary, measurements and outstanding bench work.

Denso SH7058 BEEF diesel also uses the binding's CAN guard. Its raw B6 discard and
pending UDS Read refuse new delegation after cancellation; the existing CRC
pre-drain checkpoint remains. The
[diesel evidence](can-cancellation-migration-results.md#ownership-matrix)
records the exact delegated timeout prefix and unchanged pending timeout policy.

Denso SH705x BEEF TCU uses the binding's CAN guard. Its B6 raw discard
refuses delegation after cancellation from an accepted Write. Kernel-ID drain
bounds, exact session validation, UI preflight and the executor-owned
inner restart keep their existing owners. The
[TCU migration evidence](can-cancellation-migration-results.md#ownership-matrix)
records the real boundary proof and retained restart integration.

The Denso ISO-15765 bootloader template uses the binding's CAN guard through its
executor interface. Earlier per-family migration evidence is recorded in the
[Denso migration results](can-cancellation-migration-results.md#ownership-matrix).
Profile values, best-effort stop/close policy, clock waits and SH page pacing
remain with their current owners. A completed N83M dump can succeed after
cancellation at its best-effort stop; an SH final-page pacing cancellation
still returns its clock failure.

Hitachi SH7058 CAN uses the binding's guard for Exchange and erase-continuation
entry refusal, retaining Session post-operation checkpoints. Delegated Read
errors still precede its after-read cancellation check; successful reads retain
`SH7058 write cancelled`. The
[SH7058 evidence](can-cancellation-migration-results.md#ownership-matrix)
distinguishes preserved family behavior from the synthetic selection proof.

Hitachi SH72543R CAN uses the binding's guard for Send/Receive entry refusal
and retains their post-operation checks, Sleep and finalization checks. Receive
still gives cancellation precedence over a concurrently failed delegated Read. Its
[SH72543R evidence](can-cancellation-migration-results.md#ownership-matrix)
records the preserved policy and independent selection proof.

Denso SH705x CAN EEPROM uses the binding's guard for raw exchange, kernel
chunk and page-Read entry refusal, retaining their post-operation checkpoints
and the outer page-loop checkpoint. Cancellation after an
accepted B6 Write still stops at `cancelled after kernel chunk write` before
its acknowledgment Read. The
[EEPROM evidence](can-cancellation-migration-results.md#ownership-matrix)
records the measured predecessor boundary and separate upload characterizations.

Subaru Denso SH705x DensoCAN uses the binding's mixed-CAN guard.
Its four raw/ISO-15765 I/O methods refuse cancellation at entry and preserve
accepted delegate outcomes. Reset, configuration, Open/Close, buffer control,
mode transitions and unblock forward without a new gate. The executor retains
post-I/O, startup, validation and log-boundary checks, including after-write
diagnostics, transport-error precedence and post-erase recovery warnings. The
[DensoCAN evidence](can-cancellation-migration-results.md#ownership-matrix)
separates these preserved family behaviors from the raw and ISO selection proofs.

## CAN cancellation completion evidence

Interface-owned guarding, shared generic test ownership, typed exposure and
normalized reporting, and all 33 audited pre-I/O poll removals are implemented.
[ADR 0025](../adr/0025-enforce-cancellation-in-a-guarded-transport.md#durable-completion-contract)
owns the durable completion and delivery gates. The
[results](can-cancellation-migration-results.md#measurements-and-accepted-budget)
record 66,215 production / 89,985 test NLOC against current-master trunk's
66,211 / 88,889: accepted growth of `+4` / `+1,096`. The approved budget
replaced net-negative NLOC as a completion condition; test NLOC did not shrink
against trunk. Software ownership completion does not establish hardware
qualification or completion of the separate top-of-stack delivery gates.

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
SH705x TCU execution owners: kernel payload transfer, ROM read and ROM write.
It borrows the caller's ports and profile; execution context, lifetimes and
transport lifecycle remain with callers. Framing, reply parsing, CRC comparison,
flash initialization and write windows are private to this module. There is no
session facade or callback operation runner.

- `TransferPayload` validates, pads, checksums and encrypts the kernel, sends
  `34 04 33` with the low 24 address bits and checked 24-bit encrypted payload
  length, both big-endian, then sends 128-byte B6 blocks and the final
  address-only B6. Each block has one raw reply read; content and nonterminal
  adapter outcomes are discarded, while cancellation/disconnection terminate.
  The download policy delays 50 ms before reading, then waits 2000 ms for
  petrol/TCU or 10 ms for diesel; pending replies use 3000 ms and at most ten
  repeats. Block reads use 2000 ms for petrol/TCU and 500 ms for diesel.
- `Read` uses opcode `03` with `00 + U24 address + U16 0400` and a 2000 ms
  timeout for fixed 0x400-byte raw BEEF pages, checks each reply and bounds
  the appended tail and progress to the requested region. No normal-ROM BEEF
  read decrypts the payload. Per-page clock sampling keeps the 1 ms elapsed
  floor and existing rate logs.
- `Write` owns CRC comparison (`02`) and its 200 ms raw stale-frame drain,
  flash init, voltage query (`04`, 500 ms), erase (`25`, 3000 ms), 0x200-byte
  buffer writes (`22`, 800 ms), 0x1000-byte validate/commit windows
  (`23`/`24`, 3000 ms), recovery warnings and final comparison. Unchanged images skip flashing;
  ECU TestWrite selects disable/validate rather than enable/commit. Existing
  address-window guards, cancellation boundaries and final-mismatch reporting
  remain intact. The existing profile keeps wording, init timeout and hex
  formatting: init uses 500 ms for ECU and 2000 ms for TCU. The uppercase-size
  setting also preserves TCU uppercase write addresses and padded CRC logs,
  without selecting a different wire algorithm.

The [ECU execution module](../../src/backend/flash/ecu/denso_beef_can_ecu_execution.h)
has two typed `RunDensoBeefCanEcu` overloads, admitted only for petrol and diesel.
Their family-named executors retain setup/reset hooks and delegate execution.
One private algorithm owns this fixed order: BEEF kernel probe; if absent,
ECU ID, VIN, CAL ID and CVN; security preparation; tolerant session `03` then
`43` discovery; seed/key; programming `02` and/or `42` according to accepted
sessions; optional kernel payload transfer; ECU handoff; then kernel read/write.
Vehicle-information fields share one fixed `09`/PID query and `49`/PID reply
validation loop; CVN renders uppercase compact hex, while VIN and CAL ID retain
text rendering. Read naming remains `CAL-ID_ECU-ID_` with unavailable parts
omitted. Identity queries retain their 50 ms delay and 2000 ms timeout;
cancellation/disconnection abort, while other query failures are logged and
tolerated.

Exactly two private family-trait specializations supply compile-time facts and
security calculations. Petrol retains stock, EcuTek, RaceRom, RaceRom-alt and
Cobb security, including the three CAL-ID-dependent RaceRom-alt RAM mappings.
Diesel retains stock Denso security. Neither traits nor profiles select operation
steps or carry callbacks. ECU probes send `BE EF 00 01 01 00 00 00`, wait 200 ms,
then read for 800 ms. The initial probe tolerates malformed/nonterminal outcomes;
post-upload verification is strict. ECU handoff sends transfer exit, waits
100 ms, sends `31 01 02 02 02`, then requires the BEEF ID. Transfer exit uses the
family download policy; routine start uses the 10 ms first-read policy. Both
accept service-only positive replies. Diesel's extra failure and byte-count logs,
petrol's hex PDU wording and diesel's described PDU wording remain distinct.

The [TCU executor](../../src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.cpp)
owns a visibly separate cold path. Its proprietary `7A A0 00 00 00 00 00 00`
probe makes at most five attempts with 100 ms waits and 800 ms reads. It filters
CAN IDs and coalesces kernel-ID fragments with a bounded 32-frame, 200 ms drain.
If no kernel answers, it requests ECU ID and CAL ID, requires session `03`,
performs its seed/key exchange, and requests programming `02`. After optional
payload transfer it sends transfer exit and routine start, accepting service-only
positive replies. It then resets, configures and opens through `RestartIso15765`
at 500000 bit/s with standard IDs `7E1`/`7E9`, waits 500 ms cancellably, and
strictly repeats its proprietary probe. The outer attempt still owns the initial
lifecycle and final close. Plan admission rejects TCU TestWrite and SH7055 Write.

The unchanged plan goldens and original family scripted wire cases remain the
preservation contract: [petrol](../../src/backend/flash/ecu/subaru_denso_sh7058_can_executor_test.cpp),
[diesel](../../src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor_test.cpp)
and [TCU](../../src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor_test.cpp).
Retired session/common-helper cases now exercise the public kernel and family
executor seams, preserving literal request/reply vectors, complete bounded log
sections, absent/present ROM IDs, session selection, timeout/delay assertions and
abort/continue behavior. The [kernel tests](../../src/backend/flash/ecu/beef_can_kernel_test.cpp)
retain malformed-envelope, declared-payload-bound, transfer and warm-operation
evidence. Expected bytes do not use production encoders. Hardware status remains
in the
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
  re-sends are distinct from erase's receive-only continuations. It logs
  `Closed successfully`.
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
0x100-byte pages, UdsClient-owned entry cancellation, fatal reads); each keeps its
exchange policy, rejection wording, region, decrypt table, dump setup, stop
command and padding.

The [CVT B6 chunk exchange](../../src/backend/flash/ecu/cvt_b6_chunk_exchange.h)
is the only operation of the CVT B6 chunk dialect. It adds no range guard,
cancellation checkpoint or SID validation. Hitachi closes each block with one
fatal attempt; MH8111 retries, succeeds on the first valid `0x77` and reports
`BadResponse` on exhaustion.
