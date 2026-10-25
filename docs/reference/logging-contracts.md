# Logging contracts

Compatibility and ownership details for logger definitions, selections,
snapshots, and output. The [logging design notes](../design-notes.md#logging)
explain the policy boundary.

## Model ownership and identity

[LoggerModel](../../src/backend/logging/logger_model.h) separates immutable
definitions, operator selection, and ECU support. Desktop composition owns the
model and definition service; the window-owned desktop run coordinator owns the
active display cache. XML `enabled`
defaults remain immutable. Selection reads/edits do not restore XML support
flags. Parameters and switches have separate `(protocol, ID)` namespaces;
presentation and defaults retain definition order. Chooser items carry those
identities rather than resolving labels, which may be duplicated.

Defaults choose available entries in definition order within the selected protocol,
at the existing limits of 15 gauges, 12 Digital slots, and 20 switches. Switch-only
definitions can initialize defaults. Known-supported entries are available even
when XML-disabled; known-unsupported entries are unavailable. Unknown support
uses XML enablement for default/chooser eligibility. Explicit selections with
unknown support still undergo normal preparation, including user-authored MUT
sources. No ROM matching is required.

All connection-specific support evidence resets when a new target is identified,
including custom protocol keys. Changing the ECU/TCU selection invalidates both
identification and support evidence; the next run identifies the selected target. Missing capability metadata or payload bytes leave
support unknown. SSM's framing checksum is excluded from capability data. MUT
setup ACKs and returned data do not establish per-measurement support.

Unreadable selection files leave choices alone. Successful reads clear selected
IDs before resolving entries; an absent ECU initializes defaults from available
parameter or switch definitions. Failed saves retain edits, and definition failures
remain nonfatal. Unresolved and known-unsupported selections remain visible and
replaceable; they prevent startup with an actionable protocol/identity error.

## Desktop run preparation

The UI-owned, Qt-free
[preparation module](../../src/ui/desktop/logging/logging_run_preparation.h)
borrows the model and captured choices for one synchronous call. It returns an
owned engine registration key and validated logging run snapshot together, without
I/O or retained references. The saved protocol selects acquisition behavior;
the separate SSM filter selects logging definitions. Exact, case-sensitive
resolution supplies these desktop defaults:

| Saved protocol | Registration key / acquisition | Definition filter | Poll timeout | Silence threshold | Reconnect threshold | Retry period |
| --- | --- | --- | --- | --- | --- | --- |
| `MUT_DMA` | `MUT_DMA` / MUT/DMA | `MUT_DMA` | 50 ms | 20 | 100 | 20 |
| `CDBG` | `CDBG` / CDBG | `CDBG` | 50 ms | 20 | 100 | 20 |
| Every other string | `SSM` / SSM | Supplied SSM filter | 300 ms | 10 | 30 | 10 |

There is no trimming or case normalization. Empty or unknown saved strings keep
the SSM fallback; an empty SSM filter retains its existing validation error.
MUT/DMA and CDBG ignore the supplied SSM filter. Backend validation order,
error kind/detail and capture semantics are unchanged. The captured selection's
protocol is not normalized to the effective definition filter.

The returned registration key and acquisition protocol agree when produced.
Engine registration remains independent: generic callers can supply an explicitly
registered key and an already prepared snapshot, including custom protocols and
polling policies. Engine dispatch is not inferred from the snapshot's definition
filter or acquisition protocol.

The window captures ECU/TCU choice before preparation and performs Qt string
conversion. It handles detailed preparation failures before starting the run;
no protocol factory or CSV file is opened on those failures. Run startup keeps
its existing generic failure notice. Active display refresh uses the captured
snapshot's definition filter, and the run owner retains lifecycle and reentrancy
ownership below.

## Per-run snapshots

[Run preparation](../../src/backend/logging/logging_run_snapshot.h) returns an
immutable, owned snapshot containing the validated session, protocol, ECU/TCU
target, selected display positions, logical measurements and captured presentation
metadata. It acquires the ordered union of gauges, Digital values and switches.
Repeated positions share one measurement by `(kind, protocol, ID)`; parameter and
switch namespaces remain distinct. Distinct definitions keep separate acquisitions.

Selected unresolved or known-unsupported entries fail before any protocol factory
starts. Unknown support does not silently omit an explicit selection. Each selected
source validates its syntax, width, conversion and aggregate protocol budget.
Changing selections persists pending choices, shows a restart-needed state only
while they differ from the captured choices, and
keeps active acquisition, displays and CSV columns unchanged until the next run.
XML definition edits require an application restart.

[SSM read plans](../../src/backend/logging/logging_read_plan.h) expand multi-byte
sources into ordered byte addresses and map complete replies back to logical
measurements. The 84-entry limit counts requested addresses, rather than display
positions or logical channels. Contiguous base-plus-width sources and explicit
ordered byte-address lists normalize into this plan without sharing physical reads
between distinct definitions. Binary bytes decode before scaling: `0x01,0x02`
supplies 258 with the unsigned big-endian default.

MUT/DMA and CDBG supply unsigned integer decimal input. MUT free-form request
codes and values always use firmware-backed little endian, as recorded in the
[wire evidence](logging-wire-evidence.md).
CDBG publishes only channels from the received CAN frame, with their captured
logical positions; it does not manufacture values for unseen frames. Desktop
caches retain last-known values only within the active run.
Switches read one byte and extract a separate sample bit, supplying 0/1 to caches
and CSV; the desktop indicator renders OFF/ON. Gauge selections acquire/cache/export
values, while construction of a new gauge renderer remains a follow-up.

Conversion expressions use the shared
[checked evaluator](../../src/algorithms/expression/expression.h) with
`double` arithmetic throughout; intermediate results are not rounded. Raw
parameter input must be a finite decimal number; malformed raw input is
`kBadResponse`, and an expression that fails evaluation is `kInvalidConfig`.
Session preparation compiles owned expressions once, without arbitrary trial inputs.
Syntax failures include their source position; checked runtime failures include the
failed operation. Polling evaluates the immutable compiled representation. Switch
raw input must be a one-byte unsigned decimal value before its sample bit is extracted.

## Desktop run ownership

The window owns [DesktopLoggingRun](../../src/ui/desktop/widgets/desktop_logging_run.h)
and borrows the composition-owned engine. The coordinator owns the captured
snapshot, formatted readings, elapsed timer, CSV file, most recent output path
and CSV failure latch. `view()` returns an independent copy of presentation
facts, including active selection and measurement metadata. The window retains
pending selection, identification sequencing, idle display choices, translations,
controls and dialogs; it supplies prepared inputs after identification succeeds.

`start()` rejects an already active run before modifying it. Snapshot and fresh
cache initialization precede engine startup; a returned startup failure releases
run state and output without manufacturing a completion event. CSV creation
failures are reported after startup returns and leave successful acquisition
running. CSV enablement survives successive runs. Open, header and row write
failures latch once; subsequent samples continue to update readings without
retrying output. An explicit disable/re-enable or new run retries with captured
columns and a new exclusive file. Elapsed time resets for a new acquisition run,
not when CSV is re-enabled. Closed files and their contents remain intact.

Normal engine completion synchronously joins acquisition and releases the
snapshot, readings and output before publishing `sessionEnded`. The completion
subscriber may start another run immediately; cleanup never resumes against that
replacement. The last successfully opened output path remains available after
close. CSV disablement releases output alone. `shutdown()` is terminal and
idempotent: it disconnects engine callbacks, stops acquisition and releases output
without ordinary completion or CSV notices. Close errors use `shutdownError`,
which the window logs without a modal dialog. The coordinator is destroyed before
its borrowed engine and hardware services.

Sample error delivery preserves healthy peers while the same run remains active.
`presentationChanged` publishes a committed cache before CSV append, preserving
the window's existing rendering/event-processing order: a stop processed during
rendering prevents that batch's row. Each entered batch captures the coordinator's
run identity. A stop, shutdown, or replacement during error or presentation
delivery abandons its remaining samples and pending CSV append, even when the
replacement has identical selections. The replacement owns a fresh cache and
receives only its own batches. Coordinator checks protect already-entered
handlers; the engine's generation fence separately rejects queued stale events.
These checks change no protocol traffic or hardware qualification.

## Definition input validation

[Channel preparation](../../src/backend/logging/logging_channel_preparation.h)
accepts hexadecimal address digits with an optional `0x`/`0X` prefix and positive
decimal lengths. Both trim surrounding ASCII whitespace and reject signs,
internal whitespace, trailing junk, and overflow. Protocol address, length, and
aggregate wire-capacity limits continue to apply.

Display formats are exactly `0`, or `0.` followed by 1–15 zeros; the fractional
zero count defines fixed decimal precision. Other patterns are rejected rather
than interpreted by counting zeros within arbitrary strings. Empty units are
valid and display without a suffix. Omitted XML attributes retain parser
defaults: units `#`, expression `x`, format `0.00`, and length `1` for a parameter
with an address.

Parameter width comes solely from nested `<address length="...">`, defaulting to
one when absent. Parameter-level width is ignored. Multiple explicit SSM byte
addresses retain their order and each declares at most one byte. Inconsistent
lengths, address overflow and invalid switch bits fail selected-source preparation.
Switches use top-level `byte` and `bit`; nested addresses and `ecubit` do not supply
switch metadata. The same external bit supplies sample extraction and, when
`ecubyteindex` is present, capability evidence. These roles remain separate internally.

The first conversion supplies parameter scaling, units and fixed display precision.
Selected malformed fields fail with protocol, identity, field and reason. Repeated
display positions are allowed; duplicate participating definitions are rejected.
The desktop displays startup errors as literal plain text.

## Sample resolution and failure handling

[Sample resolution](../../src/backend/logging/logging_sample_resolution.h) maps
internal transport identities through captured measurement records to the user
identity, kind, numeric value and precision. It does not infer kind from user text.
Desktop code owns fixed formatting and separate parameter/switch caches. Each run
starts its selected cache entries unavailable until sampled; missing current-run
readings appear blank in CSV and Unavailable in active displays.

SSM and MUT reject incomplete/checksum-invalid response shapes before delivering
values. A checksum-valid short MUT payload cannot fabricate zero readings. Bad
responses follow the existing polling retry policy. A value-dependent compiled
conversion failure emits an explicit unavailable reading for only that measurement;
healthy peers continue and a later successful conversion restores it. The desktop
clears its cached value, so CSV and displays cannot present a stale reading as
current. Diagnostics report at most once per measurement per run. Invalid expressions
fail preparation; unknown raw channels remain internal errors and prevent batch
publication. Malformed numeric input remains a bad-response error.
Delivered identity/cache errors report an error and permit other samples to
continue. The desktop keeps per-sample active-run checks and worker generation
fencing across restarts. User-defined names/units display literally.

## Desktop protocol binding

[Registration](../../src/platform/desktop/common/transport/desktop_logging_protocol_registration.cpp)
is called once by composition before constructing the window and performs no
adapter I/O. The engine invokes factories synchronously during `start()` before
launching the worker. Factories borrow the facade and clock; composition stops
and destroys the engine before those services.

The desktop reads the ECU/TCU choice before preparation; the validated snapshot
captures it before engine start.
SSM uses that per-run target and checks the adapter capability when its factory
runs. Factories do not read widgets. CDBG preserves ordered setup with
first-failure return and a short-circuited open check; MUT/DMA uses
`AlreadyInMode(125000)`. Actual wire qualification belongs to the
[logging composition checklist](../checklists/logging-composition-bench-checklist.md).

## SSM response integrity

The [SSM logging protocol](../../src/backend/logging/protocols/portable_ssm_logging_protocol.cpp)
extracts one direct-transport frame using its declared body length and retains
coalesced following bytes for the next poll. Starting or stopping clears those
bytes, so reconnect probes cannot accept a previous polling reply. It completes
fragmented frames and resynchronizes to the captured target sender (`0x10` ECU, `0x18` TCU) within the
poll deadline. Available complete frames are extracted even when a read finishes
at the deadline; additional direct reads are capped to the remaining time.
OpenPort keeps its single adapter read per response.

Startup requires a checksum-valid `0xE8` response with exactly one probe data
byte (seven bytes including framing). Polling checks the header, captured sender,
declared total length and checksum before delivering samples. Invalid or missing
poll replies deliver no samples and retain the existing retry behavior. Mapping uses the captured physical response positions and binary numeric assembly;
the read plan expands logical widths into the requested byte addresses.

Backend scripted TCU responses verify target-aware parsing. Direct desktop TCU
operation remains limited: the direct serial facade recognizes ECU-specific
SSM prefixes and can discard TCU replies before bytes reach the logger. Actual
adapter and ECU/TCU behavior, including repeated requests during continuous
replies, requires the
[composition checklist](../checklists/logging-composition-bench-checklist.md).

## Selection persistence and CSV

Selection XML writes with explicit four-space indentation. pugixml's default
tab indentation would otherwise reformat existing files on save. The
[selection reader/writer](../../src/backend/logging/logger_conf.h) owns this
contract.

The [portable column policy](../../src/backend/logging/logging_csv_columns.h)
exports unique acquired measurements in first-occurrence gauge → Digital → switch
order. Repeated positions share one column; parameter and switch IDs stay distinct. The [portable record serializer](../../src/backend/logging/logging_csv_record.h)
quotes commas, quotes and CR/LF, preserves UTF-8 and empty field positions, and adds no extra trailing column.
A final unavailable field still requires its preceding delimiter. Desktop code supplies fixed numeric formatting and elapsed
run time, binds columns to current-run caches, and owns filesystem operations.

The [desktop CSV adapter](../../src/platform/desktop/common/logging/logging_csv_file.h)
creates each run's file exclusively with `NewOnly`, trying numeric suffixes on
existing-name collisions. It never truncates completed output or appends another
run, including identical restarts and restart after Connect. Header and first valid
sample row are both written; the first sample is not discarded. File ownership
ends on normal stop, cancellation, adapter loss, runtime/factory/handshake failure,
Connect, file logging being disabled, or window destruction.

Enabling file logging during a run uses that run's captured columns. Re-enabling
creates another exclusive file with the same captured schema. File-open/write
failure reports once, releases ownership and leaves display updates running;
a new run or explicit re-enable retries creation. Pending edits cannot change an
active file's column meanings. Hardware checks remain owned by the
[logging engine](../checklists/logging-engine-bench-checklist.md) and
[composition](../checklists/logging-composition-bench-checklist.md) checklists.

## MUT/DMA response integrity

The [MUT/DMA driver](../../src/backend/protocol/mut_dma_driver.cpp) validates
stream framing, checksum, and exact payload length against the sum of selected
channel widths before decoding. Short and oversized checksum-valid payloads are
`kBadResponse`; they publish no measurements and cannot fabricate zero readings.
Absent data remains a no-response poll. Malformed replies follow the existing
logging retry policy, and a subsequent complete reply can supply measurements.
The [free-form decoder](../../src/algorithms/protocol/mut_dma/mut_dma_freeform.cpp)
also rejects payloads whose length differs from the selected widths.

## MUT/DMA protocol evidence

OEM K-Line DMA activation research is held in the parent research repository,
under the title "OEM K-Line DMA Logging — Activation Control Flow & Wire Protocol"
(2026-06-07, Z27AG / 33520003). Search that repository by title/date for the
original extraction; it is not a deleted FastECU-local spec.

The [free-form wire evidence](logging-mut-freeform-wire.md) records
little-endian request codes and multi-byte values in Colt 33520003 firmware.
PR #577 corrected the old big-endian interpretation. There is no compatibility
dialect or XML selector. Preparation permits at most 96 entries and 96 requested
response bytes, matching the analyzed firmware. The count field's 255-entry
representation is not demonstrated ECU capacity. Other MUT commands retain their
own byte order; no ROM match or inferred capability from ACK/data is required.

The maintained [codec sources](../../src/algorithms/protocol/mut_dma/) and
[driver sources](../../src/backend/protocol/) describe implemented behavior.
Static protocol extraction does not qualify the host adapter, electrical path,
or end-to-end desktop workflow. Runtime evidence remains subject to the
[logging checklist](../checklists/logging-engine-bench-checklist.md).

## Definition input grammar

[Definition parsing](../../src/backend/logging/logger_definition_parser.h)
reads parameter width solely from the nested `address` element's `length`
attribute, for example `<address length="2">0x804cfc</address>`. If that
attribute is absent, the width defaults to `1`; a `length` attribute on
`parameter` is ignored, including when both elements declare differing widths.
The bundled [MUT/DMA example](../../resources/shared/config/logger_mut_dma_example.xml)
and [CDBG example](../../resources/shared/config/logger_cdbg_example.xml)
use this syntax. A missing `address` retains an empty address and width for
channel preparation to validate.

[Channel preparation](../../src/backend/logging/logging_channel_preparation.h)
accepts hexadecimal addresses with an optional `0x`/`0X` prefix and positive
decimal lengths. Surrounding ASCII whitespace is accepted; signs, internal
whitespace, trailing junk and overflow are rejected. Display formats are exactly
`0`, or `0.` followed by 1–15 zeros. Empty units are valid; only the first
conversion is used. For SSM, its `storagetype` and `endian` attributes are
normalized into portable typed channel metadata. Integer widths are 1, 2 or 4
bytes; `uint`, `uint8`, `uint16` and `uint32` select unsigned assembly, while
`int`, `int8`, `int16` and `int32` select signed two's-complement assembly.
The address width governs assembly, independently of the integer type suffix.
Omitted storage selects unsigned assembly, including multi-byte sources.
`float` requires four bytes and decodes IEEE binary32; its exact finite value
is preserved as decimal numeric input for checked scaling. Omitted byte order
is big endian; `big` and `little` are accepted. Storage and byte-order values
are case insensitive; unknown selected values fail preparation. Alternatives
retain their original metadata but do not affect this run. MUT/DMA and CDBG
retain their protocol-owned unsigned decoding, including firmware little-endian
MUT/DMA free-form data; selected signed/float storage or a nondefault endian
attribute is rejected for these protocols. Default metadata does not override
protocol byte order. Existing protocol bounds and shared expression validation
remain applicable. Errors identify the definition's protocol, parameter ID and
invalid field.

## SSM definition numeric evidence

The pinned [RomRaider conversion implementation](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/logger/ecu/definition/EcuParameterConvertorImpl.java)
uses the received width for integer decoding, masks unsigned values, applies
conversion expressions after binary assembly, and supports four-byte floats.
Its omitted storage default is `uint8`, which remains unsigned at every
supported received width. Its [definition handler](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/logger/ecu/definition/xml/LoggerDefinitionHandler.java)
reads storage and endian from each conversion and defaults endian to big.
FastECU keeps checked errors for nonfinite data and rejects unknown selected
metadata instead of reproducing upstream zero substitution or permissive
fallbacks. These source-backed decoding contracts do not establish hardware
qualification; the [composition checklist](../checklists/logging-composition-bench-checklist.md)
owns that evidence.

## SSM protocol evidence

The [wire evidence owner](logging-wire-evidence.md#ssm-ordered-byte-reads) records
source-pinned address expansion and packet examples. Host regressions do not
replace the [composition checklist](../checklists/logging-composition-bench-checklist.md).
