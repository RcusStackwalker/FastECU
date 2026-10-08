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

Startup chooses the first 15 gauges, 12 digital values, and 20 switches without
filtering support. Missing-ECU defaults use current support at those limits.
Unavailable capability bytes disable parameters but retain previous switch
flags. Identification without capability bytes does not apply capabilities.

Unreadable selection files leave choices alone. Successful reads clear selected
IDs before resolving entries; an absent ECU is not initialized without parameter
definitions. Failed saves retain edits, and definition failures remain nonfatal.
Unresolved IDs survive selection round trips, are omitted from displays, and
retain empty cells in their original CSV positions.

## Per-run snapshots

[Run preparation](../../src/backend/logging/logging_run_snapshot.h) returns one
immutable, owned logging run snapshot containing the validated session,
protocol, selection, support eligibility, original SSM response offsets, and a
typed ECU/TCU target. Stable sample identities derive from the captured protocol
and validated channel IDs. Later definition, selection, support, or target edits
do not change an active run. Only the first conversion is used, with fixed
decimal display formatting. Desktop code converts Qt inputs and binds the run;
backend preparation reuses [session validation](../../src/backend/logging/logging_session.h).

Polling channels currently come from the Digital/lower-panel selection. Captured
gauge and switch selections do not add acquisition requests. The
[XML workflow](../../resources/shared/config/README.md) describes loading and
choosing definitions through the existing UI.

- SSM polls disabled channels at their original lower-panel offsets. Its raw
  assembly concatenates the decimal spelling of each byte: `0x01, 0x02` becomes
  `"12"`. This is the conversion-expression input contract, not a decoded
  big-endian integer.
- MUT/DMA spells the decoded unsigned integer in decimal and filters unsupported
  channels. CDBG also uses unsigned-integer decimal and does not filter support.

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

Invalid participating selected channels prevent startup. Errors identify the
protocol, parameter ID, offending field, and reason; the desktop logs the error
and displays it literally in the Logging dialog. Unsupported MUT/DMA channels
are filtered before field validation. Unsupported SSM channels still participate
in polling and must validate; CDBG does not filter by support. Unresolved
selection IDs retain their existing treatment.

## Sample resolution and failure handling

[Sample resolution](../../src/backend/logging/logging_sample_resolution.h) returns
accepted identity/value/precision, a deliberate skip, or an error. Unsupported
SSM samples do not update display values. Desktop code owns fixed formatting,
cache lookup and mutation, and reporting of missing cache entries.

Unknown raw protocol channels or nonfinite conversions terminate the run before
publishing any samples from that polling batch. Delivered-sample identity errors
or missing cache entries report an error and allow other samples to continue;
existing values remain intact. The desktop retains its active-run checks during
sample handling and generation checks for queued worker events after a restart.

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

CSV preserves file lifetime, schema, trailing commas, column order, and numeric
formatting. It resolves the current selection within the active run's captured
protocol, even if the UI protocol changes. It does not resolve an ID across
unrelated protocols. Unresolved columns remain empty.

A Connect action stops and joins an active logging worker before opening the
port. The datalog file stays open for the next session to append. It closes when
Logging or "Log to file" is switched off, or when a flash operation starts.
Other session ends do not close it merely because the worker stopped.

The [logging engine checklist](../checklists/logging-engine-bench-checklist.md)
and [composition checklist](../checklists/logging-composition-bench-checklist.md)
own hardware verification. Live reconfiguration, missing-frame polling, plain
serial teardown, and CDBG gaps are tracked in [technical debt](../tech-debt.md#p2-logging-engine-follow-ups).

## SSM protocol evidence

The [wire evidence owner](logging-wire-evidence.md) records physical layout,
response integrity, and the separate maintained/OEM MUT format boundaries.

RomRaider's [request/response example](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/docs/ssm_info.txt#L108)
requests two addresses and receives two data bytes. Its
[SSM protocol implementation](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/io/protocol/ssm/iso9141/SSMProtocol.java#L57)
defines a three-byte address and one-byte returned datum, and its
[address expansion](https://github.com/RomRaider/RomRaider/blob/dafe0c36c1a68efadbeedb2825f3855463fdbc35/src/main/java/com/romraider/logger/ecu/definition/EcuAddressImpl.java#L78)
constructs ordered byte-address requests for multi-byte values.

The current FastECU base-address/Digital-offset path does not establish the same
mapping for multi-byte values or unresolved selection gaps. The
[correctness debt](../tech-debt.md#p1-resolve-known-correctness-gaps) owns its
characterization and correction. Decimal-byte conversion compatibility is a
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
