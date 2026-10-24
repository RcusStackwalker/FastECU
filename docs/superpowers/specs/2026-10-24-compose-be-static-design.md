# Compile-time-sized big-endian composition

Status: design approved in conversation; implementation pending.

## Intent and constraints

Avoid dynamic allocation when a composed frame has a type-known length, and
replace awkward fixed-frame initialization followed by `Write*Be` calls.
Follow the [byte conventions](../../coding-style.md#bytes-and-wire-frames).
Keep the implementation portable, header-only, Qt-free, and compatible with
the existing C++23 Bazel graph and warnings-as-errors policy.

## Approved contract

- `ComposeBe(const Args&...)` returns `std::array<bytes::Byte, N>` when every
  argument has a type-known encoded width. Supported scalar widths remain
  unchanged. Static ranges include supported `std::array`, built-in arrays,
  and fixed-extent `std::span`; values need not be compile-time constants.
- Any dynamically sized argument makes the result `bytes::Bytes`. Dynamic
  spans, `ByteView`, vectors, and `std::string_view` remain dynamic even when
  their underlying storage or observed length happens to be fixed.
- Statically zero-length output is a compile-time error, including
  `ComposeBe()` and compositions consisting only of empty static ranges.
  Empty static ranges may participate in a nonempty composition.
  Dynamically empty output remains valid.
- `ComposeBeWithChecksum` follows the same storage-selection rule. Its
  existing supported scalar checksum result determines the trailing width.
  The checksum sees only the payload, never its own reserved suffix.
  Checksum-only composition is valid because its output is nonempty.
- Fixed-size composition is `constexpr`, including checksum composition
  when the callable permits constant evaluation. Its construction path
  does not allocate, including temporary allocations.
- `ComposeBeWithExtraCapacity` always returns `Bytes`, even with zero extra
  capacity. It remains the explicit construction path for appendable output.
  Its existing empty-output behavior is unchanged.
- Preserve supported field types and encoding behavior, including low-24-bit
  truncation by `U24`, big-endian wide-integer ranges, argument order, and
  rejection of bare integer literals. Do not add scalar widths or range types.

## Approach and migration

Automatic type-based selection was chosen over a separate fixed-size helper
or an explicit output-size template argument: callers already encode field
widths in their argument types. Keep the existing extra-capacity helper as
the vector escape hatch instead of adding another public helper.

Implementation details such as internal width traits and shared encoding
primitives are left to the implementer. Keep static and dynamic encoding
semantics aligned; do not construct an intermediate vector for array output.

Migrate affected explicit-vector assignments, returns, and subsequent append
operations deliberately: retain vector ownership where required, and use
arrays or `auto` for truly fixed output. Replace suitable fixed-frame
construction-plus-`Write*Be` sequences, including the Colt seed/key, security
key, logging-start, and pointer-frame examples. Preserve actual buffer
patches, independent protocol verification, and independently derived test
expectations. Preserve transmitted bytes and hardware qualification status.

Update the owning coding conventions and relevant helper comments. Verify
types, encoded bytes, mixed static/dynamic inputs, static-empty rejection,
runtime emptiness, checksum-only output, and constant evaluation.

## Non-goals

No new wire formats, changed protocol widths, address guards, hardware
qualification claims, layer exceptions, dependencies, or new public helpers.

## Delivery

Follow the [implementation plan](../plans/2026-10-24-compose-be-static.md).
Preserve this approved proposal in delivered Git history before removing
completed planning artifacts under the repository documentation policy.
