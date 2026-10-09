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

[Session preparation](../../src/backend/logging/logging_session.h) captures
protocol, stable identities, support, conversion, and target as owned values.
Later selection edits do not change the worker's session. Only the first
conversion is used, with fixed decimal display formatting.

- SSM polls disabled channels at their original lower-panel offsets. Its raw
  assembly concatenates the decimal spelling of each byte: `0x01, 0x02` becomes
  `"12"`. This is the conversion-expression input contract, not a decoded
  big-endian integer.
- MUT/DMA spells the decoded unsigned integer in decimal and filters unsupported
  channels. CDBG also uses unsigned-integer decimal and does not filter support.
  Each CDBG poll publishes only measurements decoded from its received frame,
  mapped through that frame's logical channel offset. Out-of-order frames do not
  fabricate zero values for unseen channels or republish measurements from prior
  frames. The desktop owns its existing display cache.

Conversion expressions use the shared
[checked evaluator](../../src/algorithms/expression/expression.h) with
`double` arithmetic throughout; intermediate results are not rounded. The raw
value is parsed once as a finite decimal number: a non-numeric raw value is a
`kBadResponse` error, and an expression that fails to evaluate is
`kInvalidConfig`. Session preparation accepts an expression if it evaluates for
at least one of the probes 1, 16 and 1616. Compiling the expression once per
session instead of per sample is tracked in
[#575](https://github.com/RcusStackwalker/FastECU/issues/575).

## Desktop protocol binding

[Registration](../../src/platform/desktop/common/transport/desktop_logging_protocol_registration.cpp)
is called once by composition before constructing the window and performs no
adapter I/O. The engine invokes factories synchronously during `start()` before
launching the worker. Factories borrow the facade and clock; composition stops
and destroys the engine before those services.

The snapshot captures ECU/TCU selection after validation and before engine start.
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
poll replies deliver no samples and retain the existing retry behavior. Mapping
still uses the historical captured response offsets and decimal-byte assembly;
response integrity does not impose a new channel-count or address-expansion rule.

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

CSV preserves file lifetime, schema, trailing commas, column order, and numeric
formatting. The portable [record serializer](../../src/backend/logging/logging_csv_record.h)
escapes every header and row field, including Time: fields containing a comma,
quote, CR, or LF are quoted, and embedded quotes are doubled. UTF-8 bytes and
empty field positions are preserved. The desktop retains column resolution,
timing, and file ownership; the first write opens the file and emits only the
header. It resolves the current selection within the active run's captured
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

## MUT/DMA response integrity

The [MUT/DMA driver](../../src/backend/protocol/mut_dma_driver.cpp) validates
stream framing, checksum, and exact payload length against the sum of selected
channel widths before decoding. Short and oversized checksum-valid payloads are
`BadResponse`; they publish no measurements and cannot fabricate zero readings.
Absent data remains a no-response poll. Malformed replies follow the existing
logging retry policy, and a subsequent complete reply can supply measurements.
The [free-form decoder](../../src/algorithms/protocol/mut_dma/mut_dma_freeform.cpp)
also rejects payloads whose length differs from the selected widths.

## MUT/DMA protocol evidence

OEM K-Line DMA activation research is held in the parent research repository,
under the title "OEM K-Line DMA Logging — Activation Control Flow & Wire Protocol"
(2026-06-07, Z27AG / 33520003). Search that repository by title/date for the
original extraction; it is not a deleted FastECU-local spec.

The [free-form wire evidence](logging-mut-freeform-wire.md) records request
codes and multi-byte stream values in little-endian order, derived from Colt
33520003 firmware. Free-form numeric values are converted to unsigned decimal
input for scaling. This corrects the previous unverified big-endian behavior;
XML definitions do not select a dialect. Other MUT command layouts retain their
own byte order. Firmware-specific capacity and hardware qualification remain
separate work.

The maintained [codec sources](../../src/algorithms/protocol/mut_dma/) and
[driver sources](../../src/backend/protocol/) describe implemented behavior.
Static protocol extraction does not qualify the host adapter, electrical path,
or end-to-end desktop workflow. Runtime evidence remains subject to the
[logging checklist](../checklists/logging-engine-bench-checklist.md).

## Portable definition preparation API

[Channel preparation](../../src/backend/logging/logging_channel_preparation.h)
accepts hexadecimal addresses with an optional `0x`/`0X` prefix and positive
decimal lengths. Surrounding ASCII whitespace is accepted; signs, internal
whitespace, trailing junk and overflow are rejected. Display formats are exactly
`0`, or `0.` followed by 1–15 zeros. Empty units are valid; only the first
conversion is used. Existing protocol bounds and shared expression validation
remain applicable. Errors identify the definition's protocol, parameter ID and
invalid field.

This backend API is tested independently. Desktop preparation still uses its
existing Qt parsing until the adoption PR switches its callers.

## Portable run preparation API

[Run preparation](../../src/backend/logging/logging_run_snapshot.h) constructs an
owned snapshot through a validating factory with const-only accessors. It
captures protocol, selection, support eligibility, original SSM offsets and a
typed ECU/TCU target. Later model changes cannot modify a prepared run. Identities
are derived from the captured protocol and validated channels rather than a
second mutable map.

Unsupported MUT parameters are filtered before definition validation;
unsupported SSM parameters remain polled and must validate, but are ineligible
for display. CDBG does not filter support. Unresolved selected IDs are omitted
from acquisition; participating duplicate IDs are rejected. Only lower-panel
parameters are acquired in this preparation scope.

Desktop consumers still use their existing snapshot representation until
adoption. CSV's current-selection behavior remains unchanged.
