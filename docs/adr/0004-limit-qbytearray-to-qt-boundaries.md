# ADR 0004: Limit QByteArray to Qt Boundaries

## Status

Accepted

## Context

FastECU is a Qt application, but protocol, checksum, logging, and flash-planning
logic do not need to depend on Qt byte containers. Broad `QByteArray` use in
core helpers makes pure logic harder to test, harder to reuse, and easier to
couple to UI, file, serial, and Qt Remote Objects boundaries.

The project builds as C++23, so `std::span` is available. Several protocol
areas have already moved to the shared byte aliases in
`src/algorithms/protocol/bytes.h`.

## Decision

Use `QByteArray` as a boundary type for Qt adapters, UI/file code, serial
integration, tests that verify Qt boundary behavior, and temporary
compatibility wrappers.

Use the standard byte types from `src/algorithms/protocol/bytes.h` for pure protocol,
checksum, logging, and flash-planning logic:

- `bytes::Byte` for one byte.
- `bytes::Bytes` for owned variable-length byte buffers.
- `bytes::ByteView` for non-owning read-only input.
- `std::array<bytes::Byte, N>` for protocol-defined fixed-size frames and
  fixed seed/key fields.

Keep Qt conversion explicit through `src/algorithms/protocol/qt_bytes.h`. New pure helpers
should not include `QByteArray`.

## Consequences

Positive consequences:

- Core protocol and checksum helpers become easier to test without Qt fixtures.
- Fixed-size protocol frames can be represented with fixed-size containers.
- Byte parsing APIs can accept views instead of copying.
- Qt integration remains explicit at adapters and compatibility wrappers.

Negative consequences and risks:

- Existing operation, UI, file, and serial code still has broad `QByteArray`
  usage and must be migrated incrementally.
- Compatibility wrappers will exist for a while, so new logic must avoid
  growing inside them.
- Conversions at boundaries must stay explicit to avoid hidden copies and drift.

## Notes

Migrate one bounded slice at a time, starting from the reusable protocol and
checksum helpers and working outward to their consumers. Do not attempt a
sweeping conversion across all operation classes; leave `QByteArray` in place
where data is directly owned by Qt file APIs, widgets, Qt Remote Objects, or
serial adapters until a local byte-native boundary is practical.

The byte-type conventions this decision implies — `ByteView` for read-only
input, `Bytes` for owned output, fixed arrays for fixed-size frames — are
stated in [the coding style guide](../coding-style.md).
