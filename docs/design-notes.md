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

The four executors of the Denso ISO-15765 bootloader dialect share three
operations in the common module: the SecurityAccess seed/key exchange, the erase flow, and, for the two
N83M families only, the in-car opening exchange run. The shared 0x7E1 request in
that run asks for session `0x63`; the SH72531 and SH72543 diesel in-car runs send
`0x03` and stay local. The `write_memory` orchestration, reads, reflash, connect
and probe flows, kernel jumps, checksum verification, and response policies stay
in each executor, and no configurable family state machine is shared.

Hitachi and Mitsubishi M32R K-Line share framing but differ in optional probes,
handshake timeouts, fallback parsing, and acknowledgement tolerance. Likewise,
shared declarative plan validation does not establish interchangeable transfer
loops. Shared headers identify their consumers and the equivalence they rely on.
Transport seams keep failure and cancellation paths scriptable.

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

The synchronous `LoggingUseCase::run()` owns miss counting, reconnect cadence,
and status transitions. Platform code supplies a thread and forwards events.
A platform-owned loop would duplicate policy in each future consumer; a
callback-driven portable scheduler has no current consumer.

### Separate definitions, selection, support, and samples

Logger definitions are immutable, selection belongs to the operator, support
comes from identification, and display values belong to the desktop. Protocol/ID
identity avoids ambiguity from duplicate labels or IDs shared across protocols.
Owned per-run snapshots insulate workers from later widget/selection changes.
Backend run preparation now owns definition parsing and validated immutable
construction, including the typed target. The desktop bridge binds those values
to transports and workers; presentation still owns formatting and caches.


SSM raw values deliberately concatenate decimal byte spellings because shipped
conversion expressions depend on that input. CSV and selection persistence also
retain their compatibility contracts. Read the relevant section of the
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
owns numeric rules and operator outcomes. The logging evaluator and `wrx02`
predicate remain separate; further edit-policy ownership extraction is
[unresolved work](tech-debt.md#p1-separate-ui-from-application-logic).

### Save and flash use operation images

Checksum correction works on a temporary image rather than mutating editable
session bytes. Save/write outcomes therefore cannot change the user's in-memory
calibration incidentally. A successful save updates path/dirty metadata; failure
preserves it. The [calibration reference](reference/calibration-compatibility.md)
records save/open behavior and warnings for files edited by older builds.

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
