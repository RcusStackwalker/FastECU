# 13. Compose byte frames with composeBe

Date: 2026-08-13

## Status

Accepted

## Context

Protocol frames were built by hand-rolling endianness — 93 shift-and-mask
sites across six migrated flash executors — even though `bytes.h` already
provided `appendU16Be` / `appendU24Be` / `appendU32Be`. The idiom is easy to
get wrong in a way the compiler cannot catch, and these frames are written
to ECUs, where a wrong length or byte order is a bricking risk.

A second pattern recurred on top of it: compose several fixed-width values,
then append a checksum computed over what was just composed.

## Decision

Build byte frames with `bytes::composeBe`, `bytes::composeBeWithExtraCapacity`,
and `bytes::composeBeWithChecksum` from
[bytes_compose.h](../../src/algorithms/protocol/bytes_compose.h).

Each argument's wire width comes from its C++ type: `Byte` emits one byte,
`std::uint16_t` two, `u24(x)` three, `std::uint32_t` four, and any range of
`Byte` splices inline. Byte literals are written with the `_b` suffix. Any
other type is a compile error, which is what stops a `std::size_t` from
silently emitting eight bytes.

A value declared wider than its wire field must be narrowed explicitly at the
call site — `std::uint16_t(kReadPageSize)`, `u24(address)`. Count the bytes
the frame needs; do not infer them from the variable's declared type.

## Consequences

Frame construction reads as a declaration of the wire format. Endianness bugs
become compile errors rather than silent truncations.

Two shapes stay hand-rolled, deliberately:

- A checksum patched into the middle of a frame and a second appended over the
  extended buffer (the SH7055 and MC68HC16Y5 kernel-upload envelopes).
- A checksum over a buffer built by `resize` rather than from an argument list
  (the EEPROM CAN kernel word), which uses `bytes::appendU32Be` directly.

Verification code does not use the helpers. `hasValidFrame` compares an
explicit `bytes::sum8` against the received checksum, so that a bug in the
compose helper cannot cancel itself out on both sides of the comparison.

There are no little-endian variants. Every wire format in this repository is
big-endian, and the little-endian append helpers added earlier on symmetry
grounds never acquired a production caller.
