# Coding Style Guide

Conventions for C++ code in this repository. Everything here is the rule for
new and edited code; there is no proposed/accepted lifecycle. Pre-existing
sites that predate a rule are converted opportunistically as files are touched,
not in repo-wide sweeps.

Architectural decisions — the build graph, CI ownership, layering boundaries —
live in [the ADR index](adr/README.md) instead. If a convention is enforced by
a build-graph guard rather than by review, it belongs there and is cross-linked
from here.

Enforcement is PR review. Only a few of these rules have a mechanical check
(`prek` formatting, `//:openpty_includes`); the rest do not, by design.

## Strings and messages

Use `std::string_view`, passed by value, for non-owning string parameters.

Not `const char*`: `string_view` is NULL-safe, iterable, and works with the
standard algorithms. Not `const std::string&`: its `constexpr` constructor is
unreliable. Both alternatives also force conversions at the pugixml and
standard-library boundaries this codebase crosses constantly.

Do not persist a `std::string_view` beyond the call that received it.

Construct messages with `std::format` rather than concatenation:

```cpp
// Yes
return std::unexpected(Error{ErrorKind::kInvalidArgument,
                             std::format("block {} exceeds ROM size {}", index, size)});

// No
return std::unexpected(Error{ErrorKind::kInvalidArgument,
                             "block " + std::to_string(index) + " exceeds ROM size " +
                                 std::to_string(size)});
```

The format string keeps the message structure visible and greppable. The cost
of formatting is irrelevant on an error path.

Two things are deliberately outside this rule:

- `QString`-based code in `src/ui`, `src/platform`, and the legacy
  `src/backend/definitions/file_actions.cpp` monolith. `file_actions.cpp` is
  slated for extraction into portable use cases by the
  [modularization plan](modularization-plan.md)'s step 5; the pieces adopt
  `std::format` as they land as portable code.
- Path and filename joining (for example `src/backend/config/config_paths.cpp`).
  There is no message text and nothing to search for, so a format string buys
  nothing.

## Bytes and wire frames

Pure protocol, checksum, logging, and flash-planning logic uses the byte
aliases from `src/algorithms/protocol/bytes.h`:

- `bytes::Byte` — one byte.
- `bytes::ByteView` — non-owning read-only input.
- `bytes::Bytes` — owned variable-length output.
- `std::array<bytes::Byte, N>` — protocol-defined fixed-size frames, and
  fixed seed/key fields.

`QByteArray` is a boundary type only; the reasoning and the exact boundaries
are in [ADR 0004](adr/0004-limit-qbytearray-to-qt-boundaries.md). Conversions
go through `src/algorithms/protocol/qt_bytes.h` and stay explicit, so that
copies are visible at the call site.

Build frames with `bytes::composeBe`, `bytes::composeBeWithExtraCapacity`, and
`bytes::composeBeWithChecksum` from
[bytes_compose.h](../src/algorithms/protocol/bytes_compose.h) rather than
hand-rolled shifts and masks:

```cpp
// Yes
Bytes frame = composeBe(0x31_b, std::uint16_t(kReadPageSize), u24(address));

// No
Bytes frame;
frame.push_back(0x31);
frame.push_back(static_cast<Byte>(kReadPageSize >> 8));
frame.push_back(static_cast<Byte>(kReadPageSize));
frame.push_back(static_cast<Byte>(address >> 16));
// ...
```

Each argument's wire width comes from its C++ type: `Byte` emits one byte,
`std::uint16_t` two, `u24(x)` three, `std::uint32_t` four, `std::string_view`
and any range of `Byte` splice inline. Byte literals take the `_b` suffix. Any
other type is a compile error — that is what stops a `std::size_t` from
silently emitting eight bytes.

**Width is still your job.** A value declared wider than its wire field must be
narrowed explicitly at the call site: `std::uint16_t(kReadPageSize)`,
`u24(address)`. Passing a `std::uint32_t` where the field is three bytes
compiles cleanly and silently changes the frame length. The type system only
rejects argument types it has never heard of; it does not know how wide a given
wire field is supposed to be. Count the bytes the frame needs — do not infer
them from a variable's declared type. These frames are written to ECUs, where
a wrong length or byte order is a bricking risk.

There are no little-endian variants. Every wire format in this repository is
big-endian.

Four shapes stay hand-rolled on purpose, and are not inconsistencies to tidy
away:

- A checksum patched into the middle of a frame with a second appended over the
  extended buffer — the SH7055 and MC68HC16Y5 kernel-upload envelopes.
- A checksum over a buffer built by `resize` rather than from an argument list
  (the EEPROM CAN kernel word), which calls `bytes::appendU32Be` directly.
- Verification code. `hasValidFrame` compares an explicit `bytes::sum8` against
  the received checksum, so a bug in a compose helper cannot cancel itself out
  on both sides of the comparison.
- Five test sites that build their expected wire bytes by hand:
  `subaru_hitachi_m32r_kline_executor_test.cpp`'s `scriptReadChunks` and its two
  block-write loops, and `subaru_mitsu_m32r_kline_executor_test.cpp`'s two
  block-write loops. The test derives the wire format independently of the
  production code it checks, rather than through the same helper that could be
  wrong on both sides at once.

## Error handling

Backend operations return `fastecu::Result<T>` (`std::expected<T, Error>`);
`fastecu::Status` is `Result<void>`. Check both with `.has_value()`:

```cpp
// Yes
if (Status s = writeBlock(block); !s.has_value()) {
    return s;
}

// No
if (Status s = writeBlock(block); !s) {
    return s;
}
```

Do not use the implicit `operator bool`. For a `Result<bool>` or a
`Result<std::optional<T>>`, `if (result)` reads as a question about the
contained value rather than about success, and the two readings disagree
exactly when the contained value is falsy. Spelling the check out means no
reader has to know a type's value category to know what is being tested.

Keep the declaration inside the `if` — splitting it out only widens the
variable's scope for no benefit.

Exceptions never cross a port. See CLAUDE.md for the `ErrorKind` set and the
rule against extending it.

## Collections

Use `std::ranges` algorithms and views instead of iterator pairs or raw index
loops. Ranges carry their own bounds and make it impossible to mismatch
iterators from two different containers.

## Tests

**Use gmock matchers** (`#include <gmock/gmock-matchers.h>`) to assert
properties of containers, rather than loops or standard algorithms that reduce
to a bare `EXPECT_TRUE`:

```cpp
// Yes
EXPECT_THAT(frame, ElementsAre(0x31, 0x00, 0x80));
EXPECT_THAT(blocks, Each(Field(&Block::size, Le(kMaxBlockSize))));

// No
EXPECT_TRUE(std::ranges::all_of(blocks, [](const Block& b) { return b.size <= kMaxBlockSize; }));
```

A failing matcher describes itself and prints the container's contents; a
failing `EXPECT_TRUE` prints `false` and needs a comment to be intelligible.

**Mocks are package-owned.** A package that defines an interface adds a
`testing/` subpackage with one `cc_library(testonly = True)` per mock, each
with its own test. `src/backend/ports/testing/` is the reference. One mock, one
owner, no behavioural drift between copies — and the mock's own test keeps the
blast radius small when it changes. See
[ADR 0008](adr/0008-use-package-owned-mocks.md).

**Platform-specific tests go in separate source files**, listed in the matching
`*_UNIX_SRCS` / `*_UNIX_HDRS` / `*_WIN32_SRCS` Bazel list — not behind `#ifdef`
in a common source. Common backend test sources must compile on every supported
platform, and must not reach for `openpty` or other Unix-only APIs. The
`//:openpty_includes` guard enforces this; run it and the full platform matrix
when moving a platform-specific test. Where separating sources is impractical,
use a small local preprocessor guard with standard compiler/platform macros
rather than Qt ones. See
[ADR 0005](adr/0005-separate-platform-specific-backend-tests.md).

Test placement, target macros, and `MOC_HDRS` wiring are in CLAUDE.md.

## Formatting and headers

`clang-format` and the `#pragma once` check run under `prek`; run
`prek run --all-files` before pushing. Every header needs `#pragma once`.
