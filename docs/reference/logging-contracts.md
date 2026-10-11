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

[Run preparation](../../src/backend/logging/logging_run_snapshot.h) constructs an
owned snapshot through a validating factory with const-only accessors. It
captures protocol, selection, support eligibility, conversion, numeric storage
and byte order, and a typed ECU/TCU target before engine startup. Later model changes
cannot modify a prepared run. Identities derive from the captured protocol and
validated channels rather than a second mutable map. Unresolved selected IDs are
omitted from acquisition; participating duplicate IDs are rejected. This scope
acquires only lower-panel parameters.
Failed desktop preparation logs the returned error detail and shows it in a
plain-text Logging dialog. It restores the stopped UI state without starting
a protocol factory or worker.

Later selection edits do not change the worker's session. Only the first
conversion is used, with fixed decimal display formatting.

- SSM polls disabled channels as captured channels. A portable
  [read plan](../../src/backend/logging/logging_read_plan.h) expands every
  contiguous base address and width into ordered byte addresses and maps each
  channel to its packed reply bytes. Unresolved display positions do not leave
  gaps in the wire response. Distinct channels remain distinct requests even
  when their address ranges overlap. The request is bounded to 84 byte addresses
  by the one-byte SSM body length; every expanded address must fit 24 bits.
  Binary bytes decode numerically before scaling: `0x01, 0x02` is `258` under
  the unsigned big-endian default. There is no decimal-concatenation mode.
- MUT/DMA spells the decoded unsigned integer in decimal and filters unsupported
  channels before field validation. Unsupported SSM channels still validate. CDBG also uses unsigned-integer decimal and does not filter support.
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
poll replies deliver no samples and retain the existing retry behavior. The
payload must contain exactly one byte for every requested byte address.
Short or oversized checksum-valid replies deliver no samples; partial channels
never produce values. Nonfinite IEEE float data is a `kBadResponse` error,
without a fabricated zero. Framing, deadline, cancellation and buffered-frame
lifetime guards apply independently of numeric decoding.

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

## Delivered sample validation

[Sample resolution](../../src/backend/logging/logging_sample_resolution.h)
derives identity and precision from a validated run. An unknown delivered ID is
a `kInternal` error; an unsupported SSM sample is deliberately skipped; other
known samples return captured identity, numeric value and precision. Conversion
remains the acquisition use case's responsibility. A raw-channel or conversion
failure prevents publication of the entire polling batch.

The desktop adapter delegates identity and eligibility policy to this resolver.
Desktop owns formatting, cache lookup/mutation and reporting missing cache
entries. Delivered identity errors allow subsequent valid samples to continue;
existing cache values remain intact. The UI rechecks the active snapshot between
samples because error reporting may synchronously stop the run.

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
