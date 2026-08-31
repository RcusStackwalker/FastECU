# Consolidate manual byte-shift read/write into shared bytes:: helpers

## Problem

Across the codebase, ~200 call sites manually compose/decompose 16/24/32-bit
integers from byte arrays using shift-and-mask idioms, e.g.:

```cpp
// read (BE)
romcrc = ((received.at(0)&0xFF)<<24)+((received.at(1)&0xFF)<<16)+((received.at(2)&0xFF)<<8)+(received.at(3)&0xFF);
// write (BE)
output[10]=(addr>>16)&0xFF; output[11]=(addr>>8)&0xFF;
```

Two helper sets already exist but are incomplete and diverged:

- `protocol/bytes.h` (namespace `bytes::`) — BE-only. Has `readU16/24/32Be(view,
  offset)` and `appendU16/24/32Be(Bytes&, value)`. No LE, no in-place
  offset-write (only append-to-end). Used only by
  `protocol/mitsu_colt_can_cdbg_protocol.cpp`, `mut_dma_codec.cpp`,
  `mut_dma_memory.cpp`.
- `EcuOperations::byte_to_int32/24/16` + `int16/24/32_to_byte`
  (`ecu_operations.cpp:2217-2251`) — BE-only, dead code (zero live callers; a
  commented-out call at `ecu_operations.cpp:1018` sits directly above a manual
  reimplementation of the same logic at line 1019).
- `modules/ssm_protocol.cpp` has its own anonymous-namespace
  `readBigEndianWord`/`appendBigEndianWord` that reimplement
  `bytes::readU32Be`/`appendU32Be` exactly, despite already including
  `protocol/qt_bytes.h`.

The rest of the ~200 sites (mostly near-duplicate per-ECU-variant files under
`modules/ecu/`, `modules/tcu/`, `modules/eeprom/`, `modules/checksum/`, plus
`ecu_operations.cpp`, `dtc_operations.cpp`, `get_key_operations_subaru.cpp`,
`serial_port/serial_port_actions_direct.cpp`, `protocol/fastecu_can_transport.cpp`,
`modules/biu/biu_operations_subaru.cpp`, `modules/jtag/*`) each hand-roll the
same fixed-width BE or LE read/write, or split a compile-time protocol
constant into bytes for a multi-line `||` response comparison.

## Goal

One canonical, tested set of fixed-width BE/LE read/write helpers; every
fixed-width shift-based call site converted to use them; dead duplicate
helpers removed.

## Design

### New API — `protocol/bytes.h` (Qt-free, span-based)

Existing (unchanged): `appendU16/24/32Be(Bytes&, value)`,
`readU16/24/32Be(ByteView, offset = 0)`, `sum8(...)`.

Add:

```cpp
using MutableByteView = std::span<Byte>;

// LE counterparts to the existing BE read/append
std::uint16_t readU16Le(ByteView bytes, std::size_t offset = 0);
std::uint32_t readU24Le(ByteView bytes, std::size_t offset = 0);
std::uint32_t readU32Le(ByteView bytes, std::size_t offset = 0);
void appendU16Le(Bytes& out, std::uint16_t value);
void appendU24Le(Bytes& out, std::uint32_t value);
void appendU32Le(Bytes& out, std::uint32_t value);

// New: in-place write at offset into an existing buffer (BE + LE), for the
// dominant `output[i] = (value >> N) & 0xFF` call-site shape.
void writeU16Be(MutableByteView out, std::size_t offset, std::uint16_t value);
void writeU24Be(MutableByteView out, std::size_t offset, std::uint32_t value);
void writeU32Be(MutableByteView out, std::size_t offset, std::uint32_t value);
void writeU16Le(MutableByteView out, std::size_t offset, std::uint16_t value);
void writeU24Le(MutableByteView out, std::size_t offset, std::uint32_t value);
void writeU32Le(MutableByteView out, std::size_t offset, std::uint32_t value);
```

Reads/writes past the end of `bytes`/`out` are a no-op (read returns 0,
matching existing `readU*Be` bounds behavior; write silently does nothing) —
consistent with the existing functions' style of not throwing.

### New API — `protocol/qt_bytes.h` (QByteArray-native overloads)

Most legacy call sites build a `QByteArray` directly rather than a
`bytes::Bytes`. Add thin overloads so call sites don't need an explicit
`fromQByteArray`/`toQByteArray` round trip:

```cpp
void appendU16Be(QByteArray& out, std::uint16_t value);
void appendU24Be(QByteArray& out, std::uint32_t value);
void appendU32Be(QByteArray& out, std::uint32_t value);
void appendU16Le(QByteArray& out, std::uint16_t value);
void appendU24Le(QByteArray& out, std::uint32_t value);
void appendU32Le(QByteArray& out, std::uint32_t value);

void writeU16Be(QByteArray& out, std::size_t offset, std::uint16_t value);
void writeU24Be(QByteArray& out, std::size_t offset, std::uint32_t value);
void writeU32Be(QByteArray& out, std::size_t offset, std::uint32_t value);
void writeU16Le(QByteArray& out, std::size_t offset, std::uint16_t value);
void writeU24Le(QByteArray& out, std::size_t offset, std::uint32_t value);
void writeU32Le(QByteArray& out, std::size_t offset, std::uint32_t value);
```

Implemented by reinterpreting `out.data()`/appending chars and delegating to
the `bytes.h` span-based functions (same aliasing approach already used by
`view()`/`fromQByteArray()` in this file).

Raw `unsigned char output[N]` / `std::vector<uint8_t>` call sites construct a
`std::span` directly (no adapter needed) and call the `bytes.h` versions.

### Cleanup

- Delete `EcuOperations::byte_to_int32/24/16` and `int16/24/32_to_byte`
  (declarations in `ecu_operations.h:98-103`, definitions in
  `ecu_operations.cpp:2217-2251`) — dead, fully superseded.
- `modules/ssm_protocol.cpp`: delete the anonymous-namespace
  `readBigEndianWord`/`appendBigEndianWord`; replace their 2 call sites with
  `bytes::readU32Be`/`bytes::appendU32Be` directly.

### Rollout scope

Convert every genuine fixed-width (16/24/32-bit) BE/LE read or write call site
found by the initial scan, across:

- `ecu_operations.cpp`, `dtc_operations.cpp`, `get_key_operations_subaru.cpp`,
  `serial_port/serial_port_actions_direct.cpp`
- `protocol/fastecu_can_transport.cpp`,
  `protocol/mitsu_colt_can_cdbg_protocol.cpp` (the fixed-width sites that
  don't already call `bytes::`), `modules/ssm_protocol.cpp`,
  `modules/flash_utils.cpp` (`buildIso15765Request`)
- All `modules/ecu/flash_ecu_subaru_*`, `modules/tcu/flash_tcu_subaru_*`,
  `modules/eeprom/eeprom_ecu_subaru_*`, `modules/checksum/checksum_*`,
  `modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp`,
  `modules/biu/biu_operations_subaru.cpp`,
  `modules/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp`

Also convert the ~80 protocol-constant-splitting response comparisons (e.g.
`modules/tcu/flash_tcu_subaru_denso_sh705x_can_operation.cpp` and siblings'
`init_flash_write`/`upload_kernel` guards):

```cpp
// before
if ((uint8_t)received.at(4) != ((SUB_KERNEL_START_COMM >> 8) & 0xFF) ||
    (uint8_t)received.at(5) != (SUB_KERNEL_START_COMM & 0xFF) || ...)
// after
if (bytes::readU16Be(bytes::view(received), 4) != SUB_KERNEL_START_COMM || ...)
```

### Out of scope

- Algorithm-internal bit manipulation: `seedToKey`'s rotate/scramble
  (`mitsu_colt_can_cdbg_protocol.cpp:42`), `mut_dma_freeform.cpp`'s 2-bit
  descriptor packing (line 40), `transformWord`'s nibble packing
  (`ssm_protocol.cpp:31`), `flash_ecu_subaru_hitachi_m32r_kline_operation.cpp`'s
  encryption nibble packing (line 1034). None of these convert a contiguous
  fixed-width integer to/from a byte array.
- Runtime-variable-width / endian-selectable sites: `menu_actions.cpp`'s
  `get_rom_data_value` (lines 1421/1426, width + endianness chosen at
  runtime), the three near-identical loops in
  `file_actions.cpp:open_subaru_rom_file` (lines 2679-2795), and
  `mut_dma_freeform.cpp:67`'s `decodeStreamValues` accumulate loop. These are
  genuinely different shapes (variable width, not a fixed 16/24/32-bit
  swap) and are left as manual code.
- `dtc_operations.cpp:396`'s single-bit test (`(response.at(i) >> j) & 0x01`)
  — not an integer composition.

## Testing

New `tests/test_bytes.cpp` (+ `.h` / `_main.cpp`, wired into `tests.pro` and
`tests/main.cpp`'s `run_test_bytes(argc, argv)` call, following the exact
pattern `test_flash_utils.{cpp,h}` already establishes), covering:

- Each new `readU*Le`/`writeU*Be`/`writeU*Le` (bytes.h) round-trips against a
  known byte pattern (e.g. write then read back, and read against a
  hand-computed literal).
- Bounds behavior (write/read at an offset that doesn't fit — no-op / 0).
- The QByteArray overloads in `qt_bytes.h` produce identical bytes to the
  span-based versions for the same input.

Existing suites (`test_checksum_results.cpp`, `test_flash_utils.cpp`,
`test_ssm_protocol.cpp`, `test_mitsu_colt_can_cdbg_protocol.cpp`, and the
per-ECU-operation test files) are the byte-exact regression net: every
converted call site must keep producing identical output, verified by running
the existing suites unchanged (no test assertions should need to change,
since this is a behavior-preserving refactor).

## Execution note

Given the number of independent files (~30) doing the same mechanical
transformation, implementation is expected to proceed as parallel per-file or
per-family passes (each `modules/ecu/*`, `modules/tcu/*`, `modules/eeprom/*`,
`modules/checksum/*` file is independent of the others), landing on this
branch (`markelov/consolidate-cks-add8-checksum`) alongside the prior
`cks_add8` consolidation work.
