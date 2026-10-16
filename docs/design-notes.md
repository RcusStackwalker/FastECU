# Design Notes

Current design decisions and their rationale. Read the section relevant to the
change; detailed compatibility contracts are linked from each topic. Structural
decision lifecycle belongs to the [ADRs](adr/README.md), unresolved work to
[technical debt](tech-debt.md), and hardware status to the
[qualification matrix](flash-qualification-matrix.md).

## Layering

Applications compose UI and platform services. Desktop UI may use backend policy,
algorithms, and deliberately designed UI-facing platform adapters. Platform
adapters implement backend-owned ports; backend depends only on algorithms.
Backend and algorithms are Qt-free, thread-free, and free of direct filesystem
I/O. Caller-supplied ports and execution context allow reuse on another platform.

A single-consumer presentation flow stays in the UI. A portable port is useful
when the core needs the capability, not merely because a desktop class exists.
Concrete desktop adapters and UI-owned channels are intentional boundaries.
Build visibility enforces these rules; see [ADR 0016](adr/0016-enforce-qt-reachability-by-visibility.md).

## Backend ports and errors

### Silence is a value, not an error

Ordinary missing frames are successful values (`responded = false` or an empty
optional), so polling/reconnect policy can count misses. Transport faults,
malformed/negative responses, cancellation, and timeouts distinct from ordinary
silence return errors. Exceptions never cross ports; the returned result is
authoritative and events carry advisory logs, progress, and notices.

### Name distinct wire behaviors

Raw and echo-checked K-Line writes have separate methods. They coincide on J2534
but differ on direct serial; a defaulted policy parameter could silently select
the wrong wire behavior. Line controls name their effects, such as programming
voltage or boot-mode lines, instead of exposing raw RTS/DTR operations.

The session-lifetime facade retains flags across operations. Transport setup
explicitly clears ISO-14230 auto-headers and fails closed on configuration errors,
so a CAN operation cannot inherit a prior K-Line header setting.

## Flash architecture

### Backend workflows own staging policy

Backend flash workflows own routing, preparation and staging. Each workflow
owns its file-repository and [flash transport source](../src/backend/flash/CONTEXT.md)
ports. Desktop composition supplies those ports and each attempt's caller-owned
clock, binds execution to workers and renders the workflow's prompts.

### The caller owns transport lifetime

Attempts configure, open, and close typed transports; executors receive them
already open. Protocol-required baud/header/line transitions stay inside the
executor because their call counts are determined by the ECU state machine.
Binding checks executor/transport compatibility before type erasure. This avoids
family-specific cleanup contracts; [ADR 0015](adr/0015-caller-owns-flash-transport-lifetime.md)
defines cleanup and error precedence.

### Operator decisions sit outside executors

Executors are synchronous and dialog-free. Event sinks never solicit or wait
for an answer. Collect a `ConfirmationSpec` before starting, or prompt between
bounded `FlashAttempt`s at a real protocol connection boundary. Blocking prompt
callbacks make shutdown unbounded; serialized executor continuations add
scheduling machinery without a consumer.

The workflow can have several attempts, including EEPROM inspect/retry and
bootmode kernel/program stages. The returned result produces at most one
terminal failure; user cancellation produces no failure dialog. Exact family
ordering and unsupported operations live in the qualification matrix.

### Share only proven protocol equivalence

Port each family into a tested executor before factoring a clone cluster. Share
pure algorithms, framing, validation, block planning, and workflow plumbing only
when byte-level evidence establishes equivalence. Preserve family sequencing,
timeouts, response tolerance, retry policy, address translation, and erase rules.
Do not build a universal configurable state machine from similar-looking code.

Pure [plan predicates](../src/backend/flash/ecu/plan_primitives.h) share only
fixed device shape (nonnull device/flash/kernel tables and exact ROM size/block
count), operation/image shape (no image for Read, exact ROM size otherwise), and
transfer start/length equality with the device. Callers retain catalog identity,
capabilities, diagnostics and validation order, including unsupported TCU
TestWrite and SH7055 Write rejection before invalid image/kernel errors.
Kernel upload arithmetic and erase-region construction retain their existing
owners; family upload padding remains six versus 128 bytes.

MC68 and SH7055 share the [Denso BEEF K-Line primitives](../src/backend/flash/ecu/denso_beef_kline_common.h):
opcodes, frame/ACK checks, optional exchange, four-byte big-endian write-address
encoding, block comparison and programming-voltage enable. The dialect stays
separate from BEEF CAN. Framing preserves the 16-bit length conversion; ACK
acceptance requires only six bytes, the BEEF marker and opcode at offset four.
The optional exchange preserves before/after write/read cancellation checks,
caller timeout and optional settle, and distinguishes absence from present-empty
replies. Write/read transport failures and short writes precede later cancellation;
programming-line errors likewise precede the post-line cancellation check.
Required-response diagnostics remain local. CRC initial writes stay local because
their cancellation diagnostics differ from ordinary exchange; fragment
accumulation and family CRC decoding also remain local.

Block comparison takes caller-prepared absolute-addressed image bytes, one flag
per block and a family CRC reader, stops at cancellation or CRC failure, and
preserves block order and modification counts. Families retain image rendering,
address policy, startup, erase/commit sequencing, capability decoding and logs
(including SH7055 offset-six lengths). MC68 chunk exchanges retain no settle and
3000 ms reads; SH7055 retains 50 ms settles and 2000 ms reads. Both enable
programming voltage before an unchanged-image return. Final Write mismatch still
logs an error and returns success; TestWrite keeps its PASS reporting. Independent
capability-layout and verification evidence is still missing, so these preserved
behaviors do not authorize corrections or establish hardware qualification.

The four executors of the Denso ISO-15765 bootloader dialect share four
operations in the common module: pure setup-PDU encoding, the SecurityAccess
seed/key exchange, the erase flow, and, for the two N83M families only, the
in-car opening exchange run. The shared 0x7E1 request in
that run asks for session `0x63`; the SH72531 and SH72543 diesel in-car runs send
`0x03` and stay local. The `write_memory` orchestration, reads, reflash, connect
and probe flows, kernel jumps, checksum verification, and response policies stay
in each executor, and no configurable family state machine is shared.
`DensoIso15765DownloadSetupPdu` and `DensoIso15765UploadSetupPdu` encode their
fixed service (`34` or `35`), `04 44`, and the supplied region's four-byte
big-endian start and length without validation or address arithmetic. The
public interface accepts only the region, so callers cannot select another SID.
Independent literal vectors and family transcripts pin its eleven-byte output.
Read setup remains tolerant on N83M 4M and fatal on the other three families;
SH72531 retains its long setup timeout, while SH72543 diesel and N83M 1.5M
retain their short setup timeouts. Encoding does not change erase regions,
caller response policy or hardware qualification. `DensoIso15765Erase` accepts
the family-selected region and constructs its download setup internally; family
write orchestration calls it directly without forwarding wrappers.

The Denso read-boundary characterization resolves the broader extraction
suggestions for OBK probes and read stops; the
[P81 page follow-up](superpowers/specs/2026-10-14-flash-duplication-triage.md) remains open.
Those proposals to share probe/stop/read operations disagreed with the common
header's deliberate local-flow contract; this decision keeps that contract and
bounds the page-only follow-up.
Keep complete OBK probes and best-effort read-stop operations local: their
observable logs differ (N83M success punctuation; 4M omits the stop-response
log), even though the wire-prefix predicate and six-attempt stop policy match.
An absent OBK frame is a miss; an empty PDU in a valid envelope is also a miss,
while a raw empty frame fails envelope validation. Probe transport errors and
cancellation propagate. Stop ignores all exchange failures, including
cancellation and six-attempt exhaustion; this is characterization, not approval
to tighten permissiveness or a claim of hardware qualification.

The bounded page request/decrypt operation matches across all four families:
`B7` plus the four-byte big-endian address, a 2000 ms initial read timeout,
UDS positive-response checking, then decryption of whole four-byte words.
Seven encrypted bytes currently yield four decrypted bytes; progress advances
by the requested 256 bytes even for short pages. Setup, append, progress,
padding, logs and pacing stay family-owned. Contrary to the task proposal's
single-family pacing distinction, **both SH72531 and SH72543 diesel wait 1 ms
per page**; neither N83M family does. Independent family scripts pin both waits.
A separately authorized extraction may introduce only
`Result<bytes::Bytes> ReadDensoEncryptedPage(const CanExecutorContext& ctx,
std::uint32_t address)` in the common module, preserving the existing fatal
request diagnostic and response policy. It must run the four family suites,
including short-page, absent/empty, transport-error and cancellation cases,
with no page-length tightening, progress move, setup change or pacing change.
No production interface is introduced by this characterization. Local EcuFlash
artifacts yielded no tractable mapping to these exact operations; no external
oracle supports changing short-page acceptance or stop-error precedence.

Denso connection/programming characterization also keeps complete jumps,
identity preludes, close/checksum operations and reflash orchestration local.
Bench jumps read once on N83M 1.5M and SH72543 diesel; 4M and SH72531 discard
even a matching first reply, wait 50 ms and read again. Diesel initially waits
2000 ms, then uses 200 ms bench or 2000 ms in-car continuation reads; other
families use 200 ms. All tolerate 50 bench or ten in-car exhausted waits,
with 100 ms sleeps; transport cancellation errors propagate at the jump.
Non-fatal identity queries swallow exchange errors, including cancellation
results; a genuinely cancelled token still prevents subsequent transport I/O.
Programming indexes whole-image encrypted bytes by wire address minus image
base, including the N83M nonzero bases. Address-sensitive full transcripts pin
the first and last 256-byte chunks. Transfer exit retransmits `37` up to six
times at 200 ms, tolerates exhaustion and settles 100 ms before checksum;
these retries are distinct from erase's receive-only continuations. Checksum
accepts immediate positives and pending/final replies: initial/pending timeouts
are 500/500 ms (1.5M), 500/3000 ms (4M) and 2000/2000 ms (both SH families).
The legacy pending-first discrepancy remains unresolved externally. ECUFlash
family-name/constructor references do not map these operations and callers;
neither addressing nor discarded-read correctness has an independent verdict.
A separately authorized extraction may consider only the matching bounded wait
tail or B6/address/chunk exchange, preserving caller policy and independent
family tests. These observations do not authorize corrections or hardware
qualification; the [remaining pair evidence](superpowers/specs/2026-10-14-flash-duplication-triage.md)
records unresolved mappings.

The [ordinary optional M32R SSM exchange](../src/backend/flash/ecu/m32r_ssm_exchange.h)
is shared by the Subaru Hitachi M32R K-Line ECU, Subaru Mitsubishi M32R K-Line
ECU, and Subaru TCU Hitachi M32R K-Line executors. Byte-level characterization
establishes equivalent framing, write-size checks, cancellation checkpoints,
and transport-error precedence; replies remain unchanged for family parsing.
The two Hitachi executors also share the offset-four payload-prefix predicate;
it does not validate headers, declared lengths, or checksums. Short-response and
prefix-mismatch diagnostics remain family-owned. Required-response diagnostics
remain local (`no response from ECU` or `no response from TCU`). The TCU block
exchange retains its 100ms sleeps on either side of its 500ms read and five-attempt retry policy. Optional probes,
handshakes, timeout policy, acknowledgement tolerance, and address mapping remain
family-owned. Shared declarative plan validation does not establish
interchangeable transfer loops. Transport seams keep failure and cancellation
paths scriptable; sharing does not change hardware qualification.

K-Line read mapping and kernel response policy remain family-owned
([P06, P19, P48 and P49](superpowers/specs/2026-10-14-flash-duplication-triage.md)).
Hitachi ECU reads send `A0 00 00` with a `0x100000` wire bias and reconstruct
all 512 KiB; Mitsubishi reads send `A0 00 20` from `0x8000`, synthesize the low
32 KiB as `FF`, and warn. MC68 stock/Ecutek upload success is an absent frame;
a present-empty reply is an error. Current MC68 CRC decoding accumulates ten
bytes without checksum validation; SH7055 checks declared size and checksum,
then strips a payload prefix and rejects its `7F` failure marker. Both retain
twenty bounded 50 ms continuation reads and 100 ms continuation settles.
Scripts characterize these differences; they do not prove their protocol
correctness. Independent family-specific pacing, address-window, upload-silence
and CRC-marker evidence remains unavailable in the inspected local EcuFlash
artifacts. Changing any policy requires mapped protocol functions or captured
family traffic and a separate correction decision; no hardware qualification
follows from these tests.

The Subaru Unisia Jecs M32R K-Line and bootmode program executors share
`SendM32rSsmFrame` and `ReceiveM32rSsmOnce` in the
[M32R SSM module](../src/backend/flash/ecu/m32r_ssm_exchange.h), keeping separate
send and receive call sites. Send checks cancellation only before writing and
compares the write count against the full framed request, preserving SSM's
one-byte length wrap. Receive reads exactly once with the caller's timeout,
propagates transport errors before checking cancellation after the read, and
returns absent, present empty, or malformed replies unchanged. It adds no
pre-read cancellation gate. The ordinary combined exchange above composes these
same primitives with an additional after-write cancellation checkpoint. Family
parsing, polling, timeouts, programming stages, cleanup ordering, and error
precedence remain in each executor. Independent primitive fixtures and family
transcripts pin this sharing; hardware qualification is unchanged.

The Subaru TCU Mitsubishi CVT MH8104 CAN and MH8111 CAN executors share
`TcuCvtMitsuSeedKey`, `TcuCvtMitsuEncryptRom`, and `TcuCvtMitsuDecryptPage` in
the [CVT common module](../src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h).
These pure byte operations bind the existing family tables to the canonical
stock transformation. Independent literal vectors and family wire transcripts
pin the extraction. Connection, image addressing, programming flow, retries,
chunk sizes, and acknowledgement policies remain family-owned: MH8104 retains
its raw-channel exchanges and tolerant checksum handling, while MH8111 retains
UDS exchanges and fatal checksum handling. Legacy line citations establish
table provenance only; this sharing changes no hardware qualification.

The Hitachi M32R CVT CAN security/session sequence remains separate from
Mitsubishi MH8111: Hitachi sends its security exchanges through `other_uds` on
`0x7E0`, then returns to `0x7E1`; MH8111 uses its primary `0x7E1` channel.
Their current seed-key tables and stock transformation are identical, while
payload encryption tables differ. Literal seed/key transcripts characterize
these bindings without establishing interchangeable connection policy.
Both write-chunk loops tolerate an absent reply but propagate transport errors.
Hitachi closes each block with one fatal attempt; MH8111 allows twenty attempts,
succeeds on the first valid `0x77`, and reports `BadResponse` on exhaustion.
Executor scripts pin these outcomes, including success on MH8111's twentieth
attempt. They establish current behavior only: independent EcuFlash CVT
security and reflash/retry function mappings remain unavailable, and the
[qualification matrix](flash-qualification-matrix.md) remains authoritative.
A possible bounded chunk exchange extraction must retain each family's
geometry, encryption, progress, and close policy; no correction is inferred
from similarity.

### Corrections require local evidence

A correction needs local evidence of the defect, a reproducing test, exact
corrected expectations, and a qualification note describing the wire/outcome
divergence. Similarity to a sibling family is a reason to investigate, never
authority to change its protocol. Preserve command bytes, sequencing, and timing
budgets while correcting demonstrated bounds, reply-gating, read-integrity,
and cancellation defects. The [testing conventions](coding-style.md#tests)
describe mutation checks; automated success does not establish hardware qualification.

### Keep operation kinds narrow

`FlashOperation` stays Read, TestWrite, and Write. A family-specific Write that
bootstraps a RAM kernel uses existing plan payloads plus an explicit operator
confirmation, rather than adding an enum value every family must handle.

## Logging

### Backend policy owns the run loop

The synchronous `LoggingUseCase::Run()` owns miss counting, reconnect cadence,
and status transitions. Platform code supplies a thread and forwards events.
A platform-owned loop would duplicate policy in each future consumer; a
callback-driven portable scheduler has no current consumer.

### One desktop owner coordinates an active run

The window-owned `DesktopLoggingRun` borrows the composition-owned engine and
owns the captured snapshot, formatted readings, elapsed timing and CSV lifecycle.
The window renders copied views and retains identification, pending selection
and presentation. This concentrates ordering and cleanup invariants in the same
interface tested without constructing widgets. The
[desktop run contract](reference/logging-contracts.md#desktop-run-ownership)
owns the lifecycle and reentrancy details.

### Separate definitions, selection, support, and samples

Logger definitions are immutable, selection belongs to the operator, support
can be supported, unsupported or unknown from capability evidence, and display
values belong to the desktop. XML enablement determines default eligibility, not
ECU capability. Explicit user-authored MUT selections with unknown support are
validated without ROM matching. Protocol/ID
identity avoids ambiguity from duplicate labels or IDs shared across protocols.
Owned per-run snapshots insulate workers from later widget/selection changes.
Backend preparation owns channel parsing, support/selection rules, and sample
identity resolution. One validated, immutable run value prevents independently
mutable identity maps and channel metadata from disagreeing. Desktop adapters
retain input conversion, fixed formatting, display caches, and transport binding.

Definition fields follow an explicit portable grammar, allowing useful whitespace
and hexadecimal prefixes while rejecting malformed formats. Empty units permit
dimensionless measurements; contextual startup errors identify authoring mistakes.
The [logging contract](reference/logging-contracts.md#definition-input-validation)
owns the accepted syntax and failure behavior.

Acquisition uses the union of selected gauges, Digital values and switches,
sharing repeated identities within each measurement kind. Active displays and CSV
columns use captured metadata; saved edits show a restart-needed state only when
they differ. CSV exports each acquired identity once. Portable
column projection/record serialization define output, while the desktop creates
and closes an exclusive CSV for every run. These contracts are owned by the
[logging reference](reference/logging-contracts.md).

SSM decodes binary numeric values before applying scaling, using the first
conversion's storage and byte-order metadata. Its portable read plan maps
contiguous sources and ordered byte-address lists independently of display positions.
Run-owned compiled expressions use checked arithmetic; a value-dependent failure
clears only that measurement for the current update while healthy peers continue. Read the relevant section of the
[logging reference](reference/logging-contracts.md) before changing these semantics.

## Definitions

### Portable helpers own XML policy

pugixml is the adopted shared portable XML dependency, used directly without a
wrapper. Further runtime dependencies require explicit adjudication. Named
header drafts keep address text editable until submission; Qt forms own widgets,
encoding conversion, and error presentation, while backend helpers own extraction,
normalization, validation, and serialization.

Reading permits partial drafts; loading and submission validate identities;
writing emits canonical headers. These share primitives without making authoring
parse tables or flatten nested markup. Application policy is distinguished from
reference-tool evidence in the [header contract](reference/definition-headers.md).

### Catalog lookup preserves provenance

`DefinitionCatalogSession` shares the definition service with ROM opening and
uses injected ports. Startup lookup and fresh validated catalogs have distinct
lifetimes: refreshing discovery must not erase the notice for an indexed file
deleted after startup. Operator decisions stay in dialogs, and authored locations
register only after successful writes. The [catalog contract](reference/desktop-contracts.md#definition-catalog-lookup)
defines ordering and refresh outcomes.

## Calibration

### Stable sessions own data; views own presentation

Stable session IDs prevent delayed actions from editing a different ROM after
another closes. ROM bytes are the truth; maps decode on demand and edits write
bytes. View state and color ranges live with the session's windows, and hex
display owns a snapshot. Closing a session removes its views without renumbering
other sessions.

### Numeric data stays typed until presentation

Decoded snapshots distinguish numeric values/errors, static labels, blob bytes,
and absent axes. Calculations use `double`; presentation precision does not
feed back into encoding. This replaces the former comma-separated transport and
its accidental empty-cell extent. Retaining cell strings would have removed the
delimiter but kept arithmetic dependent on formatting.

Edits validate every write before mutation and clamp to definition limits before
checked storage encoding. Invalid cells can receive absolute replacements;
relative operations require valid inputs. An increment applies one requested
step, and decoding errors remain visible. The
[calibration contract](reference/calibration-compatibility.md#decoded-values-and-map-edits)
owns numeric rules, operation-local target ownership, and operator outcomes.
Shared backend interpretation supplies definition facts; prepared targets own
metadata and fresh decoded cells without exposing borrowed views. Whole-map
validation preserves axis failures for body operations, and each operation
validates only the metadata it consumes. The shared checked expression evaluator
also supplies logging conversion.

### Save and flash use operation images

Checksum correction works on a temporary image rather than mutating editable
session bytes. Save/write outcomes therefore cannot change the user's in-memory
calibration incidentally. A successful save updates path/dirty metadata; failure
preserves it. The [calibration reference](reference/calibration-compatibility.md)
records save/open behavior and warnings for files edited by older builds.
The operation image is the ROM file as opened (the session's file layout),
never the definition-addressed view, so saving reproduces the opened file's
size.

### The UI owns operator sequencing

`CalibrationOperationCoordinator` has a Qt-free closure but belongs to the UI
because it sequences presentation through `ICalibrationInteraction`. The window
retains hardware lifecycle and flash dispatch. Preflight cancellation stops an
operation; declining correction may continue with unchanged bytes. Preserve
these distinct [cancellation contracts](reference/calibration-compatibility.md#preflight-and-correction-cancellation).

## Desktop composition

### Composition outlives its consumers

Composition owns sessions, workers, clocks, and adapters. Consumers stop before
borrowed services are released; restarts reconstruct composition and window
together. Protocol registration performs no I/O and factories capture per-run
values rather than widgets. See [desktop lifetime contracts](reference/desktop-contracts.md#composition-lifetime).

### Platform selection belongs to composition and the build

The serial facade receives a backend factory instead of interpreting command-line
or OS policy. Binaries select per-OS hook implementations behind shared headers.
The construction factory keeps facade headers out of application consumers;
visibility plus `implementation_deps` protects the production UI boundary.

### Desktop connection and diagnostics use the narrowest seam

`AdapterConnection` is concrete desktop lifecycle support; portable identification
owns protocol validation. The UI coordinator fences asynchronous completion and
serializes facade use. BIU/terminal exchanges remain bounded and synchronous;
multi-exchange DTC runs use a worker. These are deliberate execution boundaries,
with [connection and diagnostic contracts](reference/desktop-contracts.md#connection-and-identification).

### UI-owned channels express UI needs

Composition connects UI logging/remote channels to platform services. A portable
port would not express desktop timestamp/linefeed requirements and is unnecessary
for a single-platform service. The long-lived channel preserves logs from destroyed
dialogs. Signal names and remote-wait connection type are
[contracts](reference/desktop-contracts.md#ui-channels).

### Configuration and catalog have explicit owners

`ConfigSession` initializes before consumers; a saved stable vehicle ID determines
selection, and derived catalog fields are not cached as parallel state. Provisioned
paths are fixed for a run. Catalog facts remain independent of flash-plan facts
and are checked for agreement, as [ADR 0019](adr/0019-compile-the-protocol-catalog-into-the-backend.md)
requires. [Configuration contracts](reference/desktop-contracts.md#configuration-session)
describe startup outcomes and ordering.
