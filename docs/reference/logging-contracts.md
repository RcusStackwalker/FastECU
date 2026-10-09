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

Conversion expressions use the shared
[checked evaluator](../../src/algorithms/expression/checked_expression.h) with
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

The maintained [codec sources](../../src/algorithms/protocol/mut_dma/) and
[driver sources](../../src/backend/protocol/) describe implemented behavior.
Static protocol extraction does not qualify the host adapter, electrical path,
or end-to-end desktop workflow. Runtime evidence remains subject to the
[logging checklist](../checklists/logging-engine-bench-checklist.md).
