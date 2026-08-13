# Compose byte frames with typed compose helpers

## Problem

The migrated flash executors under `src/backend/flash/` build wire frames by
hand-rolling endianness, even though `bytes.h` already provides
`appendU16Be` / `appendU24Be` / `appendU32Be`. A representative site,
`src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp:87-99`:

```cpp
return {
    0x34,
    static_cast<bytes::Byte>((addr >> 16) & 0xFF),
    static_cast<bytes::Byte>((addr >> 8) & 0xFF),
    static_cast<bytes::Byte>(addr & 0xFF),
    0x04,
    static_cast<bytes::Byte>((len >> 16) & 0xFF),
    static_cast<bytes::Byte>((len >> 8) & 0xFF),
    static_cast<bytes::Byte>(len & 0xFF),
};
```

Counting the shift-and-mask idiom across the portable tree:

| File | Sites |
|---|---|
| `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.cpp` | 29 |
| `src/backend/flash/ecu/subaru_denso_sh7055_02_executor.cpp` | 24 |
| `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp` | 22 |
| `src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor.cpp` | 11 |
| `src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.cpp` | 4 |
| `src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.cpp` | 3 |
| **Production total** | **93** |
| Sibling `*_test.cpp` files in those packages | 40 |

The last two files use a second idiom, `static_cast<bytes::Byte>(addr >> 16)`
with no mask, which is correct only because the cast truncates — a fact the
reader has to reconstruct each time.

Layered on top of this is a recurring shape: compose several fixed-width
values, then append a checksum computed over what was just composed. It
appears in `SsmProtocol::addHeader`, `frame()` in both the sh7055 and
mc68hc16y5 executors, `request_kernel_id_frame()` in the EEPROM K-Line
executor, and two legacy operations. Each spells the reserve, the compose,
and the checksum append separately.

## Relationship to prior work

This is the successor to
[the byte-shift consolidation design](2026-07-10-byte-shift-consolidation-design.md),
which produced today's `bytes.h`: the fixed-width `readU*` / `appendU*` /
`writeU*` families and `MutableByteView`. That effort consolidated
**fixed-width read and write**; this one consolidates **frame composition**,
the layer directly above it.

Two deliberate departures from the predecessor are recorded below: this
design does not add `Le` counterparts on symmetry grounds (see Decision 2),
and it does migrate test files rather than holding them fixed as an
independent regression net (see Decision 5 and the sequencing rule).

## Decisions

Recorded so the plan does not relitigate them.

1. **Wire width comes from the C++ type**, with `u24()` for the one width the
   type system cannot express. Non-exact types are a compile error, not a
   silent narrowing.
2. **Big-endian only, with a generic checksum width.** No `Le` compose
   variants. Every wire format in this repo is big-endian, and the
   predecessor spec's `appendU16Le` / `appendU24Le` / `appendU32Le` have zero
   production callers to this day — only `bytes_test.cpp` and
   `bytes_qt_compat_test.cpp` reference them. The `sizeof`-derived checksum
   width is kept because it falls out of the template for free.
3. **`checksum8`'s plain half is dropped, not renamed.** It is literally
   `return bytes::sum8(data)` today (`checksum_primitives.cpp:77-81`), so
   keeping it would leave two names for one operation.
4. **`dec0x100` leaves the SSM API.** Across all 89 call sites of
   `addHeader` / `hasValidFrame` / `hasPayloadPrefix` (excluding the
   declarations and definitions in `ssm_protocol*` themselves), 66 pass an
   explicit `false` and the remaining 23 take the default. None passes
   `true`; the identifier appears nowhere outside the
   `ssm_protocol*` and `checksum*` files themselves. It is a wire-format
   switch that nothing sets, on the function that frames every SSM request.
5. **Both production and test sites migrate** (133 total), with the
   sequencing rule below as the mitigation.
6. **A `_b` user-defined literal** carries byte literals, since a bare `0x34`
   is an `int` and rejected by Decision 1.

## Design

### New header

`src/algorithms/protocol/bytes_compose.h`, added to the existing
`//src/algorithms/protocol:protocol` target's `hdrs`. No new Bazel target, so
no consumer package changes its `deps`, and `//:portable_closure` needs no new
registration — the header is Qt-free and joins an already-portable target.

It is a separate file rather than an addition to `bytes.h` because `bytes.h`
is already ~300 lines of append/read/write/sum/hex primitives, and composition
is a distinct layer built on those.

### Vocabulary

```cpp
namespace bytes
{

// 24-bit field marker: uint32_t alone cannot say 3 bytes vs 4.
struct U24
{
    std::uint32_t value;
};
constexpr U24 u24(std::uint32_t value) noexcept { return U24{value}; }

namespace literals
{
// 0x34_b -> Byte. consteval, so 0x1FF_b is a compile error, not a truncation:
// a throw-expression is not a constant expression, so the call fails to compile.
consteval Byte operator""_b(unsigned long long value)
{
    return value <= 0xFF ? static_cast<Byte>(value)
                         : throw "byte literal does not fit in one byte";
}
} // namespace literals

template <typename T>
concept ByteRange = std::ranges::input_range<T> &&
                    std::same_as<std::ranges::range_value_t<T>, Byte>;

} // namespace bytes
```

### The width law

One table governs every argument, including the appended checksum.

| Argument | Emits |
|---|---|
| `Byte` / `std::uint8_t` | 1 byte |
| `std::uint16_t` | 2 bytes, MSB first |
| `u24(x)` | 3 bytes, MSB first |
| `std::uint32_t` | 4 bytes, MSB first |
| any `ByteRange` (`Bytes`, `ByteView`, `std::array<Byte, N>`) | spliced inline, `size()` bytes |
| `std::string_view` | its chars as bytes |
| anything else — `int`, `char`, `std::size_t`, enums | `static_assert` failure |

`std::string_view` is accepted but `const char*` is not, so a literal must be
written `std::string_view{"KERN2"}`. This earns its place:
`src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor_test.cpp:288-292`
pushes `'K'`, `'E'`, `'R'`, `'N'`, `'2'` one `push_back` at a time, and `char`
is otherwise rejected by the law.

Rejecting `std::size_t` is load-bearing, not incidental: it forces
`Byte(payload.size())` to be written explicitly at the one site that needs it,
rather than silently emitting eight bytes.

### The three compose functions

```cpp
template <typename... Args>
Bytes composeBeWithExtraCapacity(std::size_t extra_capacity, const Args&... args);

template <typename... Args>
Bytes composeBe(const Args&... args);   // extra_capacity == 0

template <typename ChecksumFn, typename... Args>
Bytes composeBeWithChecksum(ChecksumFn checksum, const Args&... args);
```

`composeBeWithChecksum` reserves
`sizeof(std::invoke_result_t<ChecksumFn, ByteView>)`, composes the arguments,
calls `checksum` over the composed bytes, then appends the result **through
the same width law**. A `Byte`-returning function therefore appends one byte
and a `uint32_t`-returning one appends four big-endian, with no separate rule
to remember. A `static_assert` requires the return type to satisfy
`std::unsigned_integral`.

Implementation notes:

- Dispatch is a single `if constexpr` chain in `detail::appendBe`, terminating
  in `static_assert(dependent_false<T>, ...)` — the dependent-false idiom
  rather than C++23's bare `static_assert(false)`, because MSVC's P2593R1
  support is newer than the rest of what this repo relies on, and MSVC builds
  via `/std:c++latest`.
- `reserve` sums the known widths; a non-sized range contributes 0. Capacity
  is a hint, never correctness.
- Arguments are taken by `const&` and appended left to right via a fold
  expression, so evaluation order is well defined.

The problem statement's example becomes:

```cpp
return composeBe(0x34_b, u24(addr), 0x04_b, u24(len));
```

### Checksum changes

In `src/algorithms/checksum/checksum_primitives.{h,cpp}`, `checksum8(bytes::ByteView, bool)`
is deleted outright and replaced by:

```cpp
// Additive 8-bit checksum complemented as 0x100 - sum, the framing
// convention used by the Subaru/Denso kernel-upload envelopes. The plain
// (uncomplemented) sum is bytes::sum8.
std::uint8_t negatedSum8(bytes::ByteView data);
```

Behavior is byte-identical to today's `dec0x100 == true` branch, including
`sum == 0` yielding `0x00` (`0x100 - 0` truncated to eight bits). There are
exactly five `dec0x100 == true` call sites: three production
(`subaru_denso_sh7055_02_executor.cpp:391` and `:393`,
`subaru_denso_mc68hc16y5_02_executor.cpp:383`) and two test assertions
(`checksum_primitives_test.cpp:71`, `qt_checksum_test.cpp:9`, the latter
deleted with the shim).

Thirty-one sites move to the existing `bytes::sum8`: 26 passing an explicit
`false`, three calling the single-argument form
(`flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp:651`,
`checksum_primitives_test.cpp:63`, `ssm_logging_protocol_test.cpp:45`), and
the two in `ssm_protocol_core.cpp` (`:91`, `:117`) that currently thread the
`dec0x100` variable through.

**`bytes::sum8(ByteView, from, len)` is renamed `bytes::sum8Range`.** `sum8`
is currently an overload set, and passing an overload set where the callable
is a deduced template parameter is an ambiguity error — there is no target
type to resolve against, so `composeBeWithChecksum(bytes::sum8, ...)` would
not compile. The 3-argument form has exactly one caller,
`src/algorithms/protocol/mut_dma/mut_dma_codec.cpp:10`, a pass-through
wrapper. After the rename, `sum8` is a single function that decays to a
function pointer cleanly.

### The Qt checksum shim is deleted

`qt_checksum.h`, `qt_checksum.cpp`, `qt_checksum_test.cpp`, and the
`//src/algorithms/checksum:qt_compat` target are removed. The shim's entire
content is one `checksum8(const QByteArray&, bool)` overload; with `checksum8`
gone it has nothing left to shim.

Its five consumers switch to `bytes::sum8(bytes::view(output))`:

- `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp`
- `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_sh7058_can_operation.cpp`
- `src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp`
- `src/platform/desktop/common/flash/legacy/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp`
- `src/platform/desktop/common/flash/legacy/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp`

and the dep at `src/platform/desktop/common/flash/legacy/BUILD.bazel:50` is
dropped. This shrinks the `qt_compat` shim surface, matching the direction the
fork's modularization program has already set.

### The SSM API change

`addHeader`, `hasValidFrame`, and `hasPayloadPrefix` lose the `dec0x100`
parameter in both `ssm_protocol_core.{h,cpp}` and the `ssm_protocol.{h,cpp}`
Qt shim. 66 call sites drop a literal `, false`; the 23 relying on the default
are untouched.

`addHeader` becomes a direct compose:

```cpp
bytes::Bytes addHeader(ByteView payload, Byte testerId, Byte targetId)
{
    return composeBeWithChecksum(bytes::sum8, 0x80_b, targetId, testerId,
                                 Byte(payload.size()), payload);
}
```

`hasValidFrame` keeps its own explicit
`bytes::sum8(frame.first(frame.size() - 1)) == frame.back()` comparison.
Verification is not composition, and routing it through the compose helper
would introduce exactly the self-cancelling coupling this design avoids
elsewhere.

### Sites that stay hand-rolled

Two, both deliberate, both to be left with an explanatory comment:

- `src/backend/flash/ecu/subaru_denso_sh7055_02_executor.cpp:391-393` patches
  a checksum into the middle of a frame (`request[7] = negatedSum8(request)`)
  and then appends a second one over the extended buffer.
  `composeBeWithChecksum` cannot express this, and adding a mid-frame-offset
  parameter for one caller would distort the API.
- `src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.cpp:757-760`, the
  `0x5aa5a55a - sum` kernel word, is computed over a twice-`resize`d buffer
  rather than a composed argument list. Its four `push_back` calls become a
  single `bytes::appendU32Be`.

## Rollout scope

| Change | Count |
|---|---|
| Production compose sites migrated | 93 across 6 files |
| Test compose sites migrated | 40 across 4 files |
| `checksum8` call sites rewritten | 36 (31 to `bytes::sum8`, 5 to `negatedSum8`) |
| SSM `, false` arguments removed | 66 |
| Legacy files moved off the deleted shim | 5 |
| Files/targets deleted | `qt_checksum.{h,cpp}`, `qt_checksum_test.cpp`, `:qt_compat` |

## Sequencing rule

Migrating tests alongside production code gives up the independent second
derivation of each wire format that the hand-rolled test expectations
currently provide. The order of work recovers it for the window in which it
matters. **Per file:**

1. Migrate the **production** code to `composeBe*`.
2. Run that package's tests **unmigrated**. They still hand-roll their
   expected frames, so a green run proves the rewritten builder is
   byte-identical to the old one.
3. Only then migrate that file's test to `composeBe*`.

The independent check is therefore spent during the migration, exactly when a
regression would be introduced, rather than discarded up front. Afterwards the
standing check is the byte-literal goldens in `bytes_compose_test.cpp`.

## Testing

New `src/algorithms/protocol/bytes_compose_test.cpp`, built with
`fastecu_portable_gtest`, asserting against **hardcoded byte literals only** —
never a second computed form. This is what preserves one independent
derivation of the wire format in the tree once test files have migrated, so a
bug in `composeBe` still fails something. `eeprom_read_plan_goldens_test.cpp`
is the existing precedent for the style.

Cases:

- One per width-law row, including `u24` and a spliced `ByteView`.
- `composeBeWithChecksum` with a `Byte`-returning function (appends one byte)
  and with a `uint32_t`-returning one (appends four, MSB first) — the
  width-derivation claim, asserted rather than assumed.
- `composeBeWithExtraCapacity` reserves without emitting:
  `capacity() >= size() + extra`, `size()` unchanged.
- A reconstruction of the SSM header frame shape, asserted against literal hex.

Compile-time rejections (`int`, `char`, `std::size_t`, `0x1FF_b`) cannot be
asserted from gtest. They are documented as commented negative examples in the
test file. No Bazel compile-failure harness is introduced: that machinery does
not exist in this repo and is not worth adding for six cases.

The existing per-executor suites are the regression net for the migration
itself, per the sequencing rule.

## Verification gates

- `bazel test --config=release //...`
- `prek run --all-files`
- `bazel run //:clang_tidy_report_changed`
- `//:portable_closure` must stay green. No new registration is required in
  either the `genquery` in `BUILD.bazel` or `PORTABLE_ROOTS` in
  `scripts/check-portable-closure.py`, since `bytes_compose.h` joins an
  existing portable target.
- `//:serial_compat_allowlist` is untouched. The `qt_compat` deletion only
  shrinks the graph, which no guard objects to.

## Hardware safety

This is a behavior-preserving refactor whose output must be byte-identical, so
nothing in the [flash qualification matrix](../../flash-qualification-matrix.md)
changes status. Byte-identity is asserted through the existing suites under the
sequencing rule; that is not re-qualification, and no path — `MUT_DMA` or
otherwise — becomes bench-qualified as a result of this work.

## ADR

A new **ADR 0013, "Compose byte frames with `composeBe`"** records the idiom
and its two documented exceptions. The repo already writes ADRs for this
species of codebase-wide convention (0009 `string_view`, 0011 `std::format`,
0012 ranges). Without one, the next migrated ECU will hand-roll shifts again,
because nothing in the tree tells its author not to.

## Out of scope

- **Legacy Qt call sites are not migrated to `composeBe`.** They build
  `QByteArray` directly, so every site would need a `bytes::toQByteArray`
  conversion — more code than the shifts it replaces, in files the fork is
  decomposing. Only their `checksum8` and SSM-parameter usages change.
- **No `Le` compose variants**, per Decision 2.
- **No mid-frame checksum support** in `composeBeWithChecksum`, per the
  hand-rolled sites above.
- **No changes to `ErrorKind`, the port interfaces, or `Result<T>`.** This
  design touches no error paths.
- **Algorithm-internal bit manipulation** — `seedToKey` scrambling,
  `transformWord` nibble packing, the M32R K-Line encryption packing — is
  untouched. None of it composes a contiguous fixed-width integer into a
  frame.
