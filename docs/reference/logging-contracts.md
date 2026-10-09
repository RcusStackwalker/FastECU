# Logging contracts

Compatibility and ownership details for logger definitions, selections,
snapshots, and output. The [logging design notes](../design-notes.md#logging)
explain the policy boundary.

## Model ownership and identity

[LoggerModel](../../src/backend/logging/logger_model.h) separates immutable
definitions, operator selection, and ECU support. Desktop composition owns the
model and definition service; the UI has its own display cache. XML `enabled`
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
Changing selections persists pending choices, shows a restart-needed state, and
keeps active acquisition, displays and CSV columns unchanged until the next run.
XML definition edits require an application restart.

[SSM read plans](../../src/backend/logging/logging_read_plan.h) expand multi-byte
sources into ordered byte addresses and map complete replies back to logical
measurements. The 84-entry limit counts requested addresses, rather than display
positions or logical channels. Raw parameter conversion still concatenates decimal
byte spellings: `0x01,0x02` supplies `"12"`, not a decoded big-endian integer.

MUT/DMA and CDBG supply unsigned integer decimal input. MUT uses the explicitly
selected dialect described by the [wire evidence](logging-wire-evidence.md).
CDBG publishes only channels from the received CAN frame, with their captured
logical positions; it does not manufacture values for unseen frames. Desktop
caches retain last-known values only within the active run.
Switches read one byte and extract a separate sample bit, supplying 0/1 to caches
and CSV; the desktop indicator renders OFF/ON. Gauge selections acquire/cache/export
values, while construction of a new gauge renderer remains a follow-up.

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

Standard nested `<address length="...">` metadata and legacy parameter-level
lengths are normalized before validation. Multiple explicit SSM byte addresses
retain their order; inconsistent lengths, address overflow and invalid switch bits
fail selected-source preparation. Nested switch sample bits and top-level ECU
capability bits have distinct roles; a legacy sample bit is not capability evidence.

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
readings appear blank in CSV and Pending in active displays.

SSM and MUT reject incomplete/checksum-invalid response shapes before delivering
values. A checksum-valid short MUT payload cannot fabricate zero readings. Bad
responses follow the existing polling retry policy. Unknown raw channels or
nonfinite conversions terminate the run before publishing any part of that batch.
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

## Selection persistence and CSV

Selection XML writes with explicit four-space indentation. pugixml's default
tab indentation would otherwise reformat existing files on save. The
[selection reader/writer](../../src/backend/logging/logger_conf.h) owns this
contract.

The [portable column policy](../../src/backend/logging/logging_csv_columns.h)
resolves captured gauge → Digital → switch columns, retaining repeated display
positions. The [portable record serializer](../../src/backend/logging/logging_csv_record.h)
quotes commas, quotes and CR/LF, preserves UTF-8 and trailing commas, and keeps
field positions stable. Desktop code supplies fixed numeric formatting and elapsed
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

## SSM protocol evidence

The [wire evidence owner](logging-wire-evidence.md) records physical layout,
response integrity, and the separate maintained/OEM MUT format boundaries.

RomRaider's [request/response example](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/docs/ssm_info.txt#L108)
requests two addresses and receives two data bytes. Its
[SSM protocol implementation](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/io/protocol/ssm/iso9141/SSMProtocol.java#L57)
defines a three-byte address and one-byte returned datum, and its
[address expansion](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/logger/ecu/definition/EcuAddressImpl.java#L78)
constructs ordered byte-address requests for multi-byte values.

FastECU uses ordered physical byte plans and complete-response mapping.
The [wire evidence owner](logging-wire-evidence.md) records literal packets and
dialect-specific boundaries. Decimal-byte conversion compatibility is a
separate requirement from correct address-to-response association. Static source
evidence does not replace the logging checklist's hardware qualification.

## MUT/DMA protocol evidence

OEM K-Line DMA activation research is held in the parent research repository,
under the title "OEM K-Line DMA Logging — Activation Control Flow & Wire Protocol"
(2026-06-07, Z27AG / 33520003). Search that repository by title/date for the
original extraction; it is not a deleted FastECU-local spec.

The maintained [codec sources](../../src/algorithms/protocol/mut_dma/) and
[driver sources](../../src/backend/protocol/) describe implemented behavior.
Static protocol extraction does not qualify the host adapter, electrical path,
or end-to-end desktop workflow. Runtime evidence remains subject to the
[logging checklist](../checklists/logging-engine-bench-checklist.md).
