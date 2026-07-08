# ADR 0004: Limit QByteArray to Qt Boundaries

## Status

Accepted

## Context

FastECU is a Qt application, but protocol, checksum, logging, and flash-planning
logic do not need to depend on Qt byte containers. Broad `QByteArray` use in
core helpers makes pure logic harder to test, harder to reuse, and easier to
couple to UI, file, serial, and Qt Remote Objects boundaries.

The project already builds as C++20, so `std::span` is available. Several
protocol areas have already moved to the shared byte aliases in
`protocol/bytes.h`.

## Decision

Use `QByteArray` as a boundary type for Qt adapters, UI/file code, serial
integration, tests that verify Qt boundary behavior, and temporary
compatibility wrappers.

Use the standard byte types from `protocol/bytes.h` for pure protocol,
checksum, logging, and flash-planning logic:

- `bytes::Byte` for one byte.
- `bytes::Bytes` for owned variable-length byte buffers.
- `bytes::ByteView` for non-owning read-only input.
- `std::array<bytes::Byte, N>` for protocol-defined fixed-size frames and
  fixed seed/key fields.

Keep Qt conversion explicit through `protocol/qt_bytes.h`. New pure helpers
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

## Current Baseline

Already byte-native:

- Transport interfaces: `IKlineTransport`, `ICanTransport`, and
  `ISsmTransport`.
- MUT/DMA protocol helpers and logging support.
- CDBG raw-CAN logging helpers.
- Mitsubishi CAN flashing and vendor-extension protocol helpers.
- SSM logging helper internals.
- Shared byte utilities in `protocol/bytes.h`.
- Byte-native test fixtures in `tests/byte_test_utils.h`.

Still intentionally Qt-bound or partially migrated:

- `modules/ssm_protocol.cpp` keeps `QByteArray` wrappers over byte-native core
  helpers.
- Checksum modules still use `QByteArray` broadly.
- Flash operation modules still use `QByteArray` for command construction, file
  contents, payloads, and received messages.
- Serial backends, Qt Remote Objects, UI/file classes, and integration tests for
  boundary behavior may keep Qt types.

## Migration Notes

Migrate one bounded slice at a time:

1. Keep completed protocol slices byte-native and avoid adding new `QByteArray`
   logic there.
2. Continue converting protocol tests to `bytes::Bytes`, fixed arrays, and
   `tests/byte_test_utils.h::bytesFromHex()`.
3. Convert checksum families to `bytes::ByteView` inputs and structured
   byte-native outputs one family at a time.
4. Migrate flash operation families after their reusable protocol and checksum
   helpers are byte-native.
5. Defer broad serial backend conversion until protocol, logging, checksum, and
   flash consumers are mostly byte-native.

Do not start with a sweeping conversion across all operation classes. Keep
`QByteArray` where data is directly owned by Qt file APIs, widgets, Qt Remote
Objects, or serial adapters until a local byte-native boundary is practical.

## Guardrails

- Use `bytes::ByteView` for read-only inputs and `bytes::Bytes` for owned
  variable output.
- Use fixed arrays for protocol-defined fixed-size frames.
- Keep `QByteArray` wrappers only for compatibility and call them out as
  temporary where practical.
- Avoid touching UI, QtRO, and platform serial implementation classes unless
  the current migration slice requires it.
- Run focused tests for each migrated area, plus `//:qmake_bazel_sync` when
  qmake/Bazel source lists change.
