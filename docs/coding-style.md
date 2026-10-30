# Coding Style Guide

Conventions for new and edited C++ code. Pre-existing code is converted
opportunistically as files are touched, not in repo-wide sweeps; remaining
violations are tracked in [technical debt](tech-debt.md). When adopting or
widening an enabled mechanical rule, clean every existing finding in its full
platform scope before enabling it; this includes [naming](#naming) and
[assertions](#assertions). Review-only conventions retain opportunistic cleanup.

Structural decisions (build graph, layering, CI) live in the
[ADR index](adr/README.md). Enforcement is review plus the mechanical
[checks](#checks); the custom clang-tidy checks report conservative patterns
from this guide and do not replace review.

## Naming

Names follow the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html#Naming).
[`.clang-tidy`](../.clang-tidy) (`readability-identifier-naming`) is the source
of truth and comments each deviation; kinds it does not check yet follow the
surrounding code.

- Functions and methods are `CamelCase`; parameters and locals are `lower_case`.
- Enumerators (including `enum class`), `constexpr` variables at every storage
  duration, and static-storage `const` values are `kCamelCase`. A
  non-`constexpr` local never takes `k`. Macros are `UPPER_CASE`.
- An underscore may separate words only next to a digit (`kFlashBlocksSH7058_1block`).
- Mutable globals take `g_` (`g_log_level`).
- Public struct members are `lower_case` with no trailing `_`; private and
  protected class members keep it.
- A function must not reuse the spelling of a type it could hide (`Error()`
  beside `struct Error`) or of a gtest base member (`Run`, `Setup`); use a
  verb-led name (`LogError`).
- Avoid `windows.h` macro names (`CreateDirectory`, `CopyFile`, `SetPort`): the
  macro rewrites them to `A`/`W` variants. Pick another verb (`CopyFileTo`).

Qt exceptions:

- Under `src/ui/desktop`, functions, parameters and locals are `camelBack`.
- Overrides take the name they override (`closeEvent`, `readData`). Qt signals
  and slots outside `src/ui/desktop` keep `camelBack` inside a
  `NOLINTBEGIN/NOLINTEND` block that says why. `on_<widget>_<signal>` slots are
  allowed by `MethodIgnoredRegexp` in `src/ui/desktop/.clang-tidy`.

Analysis covers reachable first-party `.h` and `.hpp` headers under `src/`,
`apps/`, `tests/`, and `scripts/`; generated and vendor headers are excluded.
See [the analysis workflow](tooling.md#repository-checks).

## Aggregate initialization

Initialize aggregates with braces: `bytes::U24{length}`, not `bytes::U24(length)`.
This also applies to aggregate variable declarations, arrays, `new` expressions,
and aggregate initialization in instantiated templates and macros.
The `fastecu-aggregate-parens` check enforces this rule. Constructor calls
(including aggregate copy construction) and value initialization such as
`Aggregate()` are different operations and remain permitted.

The check reports a diagnostic without an automatic fix: replacing parentheses
with braces can reject narrowing conversions or change reference-member
lifetimes. Review those semantics when converting an existing initialization.

## Strings and messages

**Take non-owning strings as `std::string_view` by value**, not `const char*`
(not NULL-safe) or `const std::string&` (forces conversions at pugixml and
standard-library boundaries). Never store a `string_view` beyond the call.

**Build messages with `std::format`**, not concatenation, so the message stays
readable and greppable:

```cpp
// Yes
std::format("block {} exceeds ROM size {}", index, size)
// No
"block " + std::to_string(index) + " exceeds ROM size " + std::to_string(size)
```

Out of scope: `QString` code in `src/ui` and `src/platform`, and path joining.

## Bytes and wire frames

Pure protocol, checksum, logging and flash-planning code uses the aliases in
[bytes.h](../src/algorithms/protocol/bytes.h): `bytes::Byte`, `bytes::ByteView`
(read-only input), `bytes::Bytes` (owned output), and `std::array<bytes::Byte, N>`
for fixed-size frames and seed/key fields. `QByteArray` is a boundary type only
([ADR 0004](adr/0004-limit-qbytearray-to-qt-boundaries.md)); convert explicitly
through [qt_bytes.h](../src/platform/desktop/common/bytes/qt_bytes.h).

**Build frames with `ComposeBe`, `ComposeBeWithExtraCapacity` and
`ComposeBeWithChecksum`** from [bytes_compose.h](../src/algorithms/protocol/bytes_compose.h),
not hand-rolled shifts:

```cpp
auto frame = ComposeBe(0x31_b, std::uint16_t(kReadPageSize), U24(address));
```

Each argument's C++ type sets its wire width: `Byte` 1, `std::uint16_t` 2,
`U24` 3, `std::uint32_t` 4; `std::string_view` and `Byte` ranges splice inline.
Byte literals take `_b`. Wide unsigned-integer ranges emit each element at its
own width, most-significant byte first. Any unsupported type is a compile error.

`ComposeBe` returns `std::array<bytes::Byte, N>` when every argument has a
statically known encoded width: supported scalars, `std::array`, built-in
arrays, and fixed-extent `std::span`. Values can still come from runtime data.
The fixed-size path allocates no temporary storage and supports `constexpr`.
Statically empty output, including `ComposeBe()`, is a compile error; empty
static ranges can participate in a nonempty composition.

A dynamically sized argument, including `ByteView`, a vector, a dynamic span,
or `std::string_view`, selects `Bytes`; dynamically empty output is allowed.
`ComposeBeWithChecksum` uses the same return-type rule, counting the checksum
in the output width and passing only the payload to the checksum function.
Checksum-only output is allowed, and static composition is `constexpr` when
the checksum callable permits it.

Use `ComposeBeWithExtraCapacity(extra_capacity, ...)` when vector ownership or
subsequent appending is required. It always returns `Bytes`, including with
zero extra capacity or no fields.

**Width is still your job.** Narrow explicitly to the wire field
(`std::uint16_t(x)`, `U24(x)`): a `std::uint32_t` in a 3-byte field compiles and
silently lengthens a frame that will be written to an ECU.

The helpers are big-endian. Byte order belongs to each protocol contract; the
[MUT/DMA free-form codec](reference/logging-mut-freeform-wire.md), for example,
is little-endian.

Hand-roll bytes where sharing a helper could hide a bug: checksums patched
mid-frame or over a `resize`-built buffer, verification code (`HasValidFrame`
sums independently of the composer), and test expectations, which must derive
the wire format independently of the code under test.

## Units

**Carry units in the type, not a name suffix:** `std::chrono::milliseconds timeout`,
never `int timeout_ms`.

## Error handling

Backend operations return `fastecu::Result<T>` (`std::expected<T, Error>`);
`fastecu::Status` is `Result<void>`.

**Check results with `.has_value()`, never the implicit `operator bool`.** For
`Result<bool>` or `Result<std::optional<T>>`, `if (result)` reads as a question
about the value:

```cpp
if (Status s = writeBlock(block); !s.has_value()) {
    return s;
}
```

**Chain a straight-line run of fallible steps with `and_then`.** When two or
more fallible calls run in sequence, each consuming only the previous step's
value (or nothing), and every failure branch only propagates the error, write
one `and_then` chain instead of an early return per step. Step logging goes
inside the lambdas; a reply check is an `and_then` lambda that returns
`Fail(...)`; `transform` is only for an infallible last step. Capture with
`[&]` (the lambdas never outlive the call, so Sonar's explicit-capture rule
cpp:S3608 is disabled), and spell the return type (`-> Result<T>`, `-> Status`) when a lambda
has more than one `return`:

```cpp
return FatalQuery(ctx, seed_request, ...)
    .and_then(
        [&](const bytes::Bytes& seed_reply)
        {
            log("Seed request ok");
            return FatalQuery(ctx, KeyRequest(seed_reply), ...);
        })
    .transform([&](const bytes::Bytes&) { log("Seed key ok"); });
```

Keep the early-return form inside loops, when a step needs values from two or
more earlier steps, and when a failure branch does more than propagate
(cleanup, logging, a different error). A qualifying run inside a longer
function is chained on its own.

**A lambda that only logs and runs one step is a `LogThen`.** When an
`and_then` lambda's body is info logs plus one `return` of a step that ignores
the incoming value, use `LogThen` from
[logged_step.h](../src/backend/ports/logged_step.h); a multi-line `.transform`
that only logs is a `LogDone`. Messages are captured when the chain is built,
so one that depends on an earlier step's result stays in a hand-written
lambda, as does any lambda that logs at another level:

```cpp
return Handshake(...)
    .and_then(LogThen(events, "Seed key ok", "Jumping to kernel", [&] { return Jump(...); }))
    .and_then(LogThen(events, "Requesting kernel ID", [&] { return RequestKernelId(...); }));
```

**Exceptions never cross a port.** The `ErrorKind` set in
[error.h](../src/backend/ports/error.h) is closed; adding a value needs an
[ADR](adr/README.md).

**Discard a result on purpose with `std::ignore =`**, plus a comment when the
reason is not obvious. Not `(void)` or `static_cast<void>`: GCC ignores a cast
on `warn_unused_result`. Unused variables and parameters still take `(void)name;`.

## Scope and loops

**Give every variable the smallest scope that covers all its uses.** A value
used only within one `if`/`else if` chain goes in its init-statement; one used
after the chain stays where it is. (Sonar cpp:S6004)

```cpp
if (const auto *plan = std::get_if<FooPlan>(&plan_variant); plan == nullptr) {
    return fail(InvalidConfig, "wrong plan kind");
}
```

**Prefer range-for and `std::ranges`** over iterator pairs or index loops, and
`std::ranges::find`/`find_if` over hand-written search loops.

**When an index loop is needed, its condition tests only the counter.** Exit
early with `break`; do not fold a status flag into the condition. (Sonar cpp:S886)

```cpp
// Yes
for (int attempt = 0; attempt < 20; ++attempt) {
    if (probe(attempt)) { connected = true; break; }
}
// No
for (int attempt = 0; attempt < 20 && !connected; ++attempt) { ... }
```

## Templates

**Write `&` on a function used as a non-type template argument**
(`KernelFreeCanWorkflow<FooExecutor, &build_foo_plan>`), so the address is
explicit. (Sonar cpp:S936)

**Declare template type parameters with `typename`, not `class`**
(`template <typename T>`). The keywords are equivalent there; one spelling keeps
declarations uniform. Enforced by the `fastecu-template-typename` plugin check
([tooling](tooling.md#repository-checks)), which also provides the fix-it.

## Complexity

**No function exceeds cyclomatic complexity 15** ([Lizard](tooling.md#lizard));
none is exempt. Reduce it by extracting small same-file helpers (a repeated
send/check/log exchange, a predicate for a gnarly condition), keeping each
family's log wording and legacy-citation comments at the call site. Lizard's
`forgives` marker is reserved for a confirmed parser inaccuracy, with a comment
recording it. Sonar's cognitive complexity (cpp:S3776) is a separate metric
with its own limit.

## Tests

### Assertions

**Assert `Result`/`Status` with the matchers** from
[result_matchers.h](../src/backend/ports/testing/result_matchers.h)
(`//src/backend/ports/testing:result_matchers`): `IsOk()`, `IsOkAnd(m)`,
`IsErr(kind)`, `IsErrWith(kind, m)`. They print the actual error; a bare
`has_value()` assertion prints `false`. Do not append `result.error().detail`.

```cpp
// Yes
EXPECT_THAT(result, IsErrWith(ErrorKind::kTimeout, HasSubstr("no reply")));
// No
ASSERT_FALSE(result.has_value());
EXPECT_EQ(result.error().kind, ErrorKind::kTimeout);
```

- **Prove the matching state before accessing** `*result`, `result->` or
  `result.error()`: use fatal `ASSERT_THAT` with a success or error matcher,
  or a terminating `.has_value()` guard. `EXPECT_THAT` continues past failure.
  Mutation invalidates earlier proof. `IsErrWith` checks the error without
  accessing it.
- **Pass the operation straight to the assertion** when its result is used
  once, unless a local is needed to preserve operation or cleanup order.
- `Not(IsOk())` only when the contract permits any error, inside a
  `NOLINTBEGIN/NOLINTEND(fastecu-result-assertion)` block that says why. Only
  the low-level Result API tests use ordinary assertions.
- **Other `std::expected` types** use `IsOk()` and `IsErrThat(m)`, which
  takes a matcher for the error value. **`std::optional`** uses
  `Optional(m)` (`Optional(_)` for presence alone) and `Eq(std::nullopt)`; a
  bare `has_value()` or `explicit operator bool` assertion prints `false`.
- Helpers that fallibly build plans return `Result<FlashPlan>` and propagate
  errors without asserting or throwing to unwrap. The test body asserts
  `IsOk()`. A fatal assertion in a helper returns only from the helper.
  Receiving or forwarding an already valid plan is allowed.
  A lambda immediately invoked in a GoogleTest fixture constructor's member
  initializer may instead check `.has_value()` and throw with the error detail
  to initialize a `const FlashPlan` member. Successful fixture construction then
  guarantees a valid plan before the test body runs; GoogleTest reports a
  constructor exception as a test failure with its normal exception handling.
  This exception does not apply to ordinary helpers or constructor-body lambdas.

**Assert containers with gmock matchers** (`ElementsAre`, `Each`, `Field`),
not element-checking loops or equality algorithms reduced to `EXPECT_TRUE`.
Ordinary `EXPECT_EQ` container equality remains allowed. Assert emptiness with
`IsEmpty()` / `Not(IsEmpty())` rather than `EXPECT_TRUE(c.empty())`, so a
failure prints the contents.

For uniform ROM buffers, use `test_bytes::BytesFilledWith(value)` from
[byte_matchers.h](../src/algorithms/protocol/testing/byte_matchers.h). It checks
every byte and reports the first mismatching offset without Google Mock's
per-element matcher overhead in unoptimized mutation builds. Keep an explicit
size assertion when replacing equality against a constant-filled buffer.

**A loop with assertions needs an iteration-local `SCOPED_TRACE`** identifying
the input or iteration, or becomes a `TEST_P` with an explicit name generator,
so a failure names its case. Name quality and whether an expensive outcome
should become its own `TEST_P` remain review responsibilities.
Use separate `TEST_P` cases for expensive transfer outcomes: GoogleTest
sharding can distribute them, and mutation fail-fast can stop between cases.

### Structure

- **Mocks are package-owned:** one `cc_library(testonly = True)` per mock in
  the interface package's `testing/` subpackage, each with its own test
  ([ADR 0008](adr/0008-use-package-owned-mocks.md); reference:
  `src/backend/ports/testing/`). See the [Google Mock reference](gmock-reference.md).
- **Platform-specific tests go in separate sources** in the matching
  `*_UNIX_SRCS`/`*_WIN32_SRCS` lists, not behind `#ifdef`
  ([ADR 0005](adr/0005-separate-platform-specific-backend-tests.md)). Never fix
  a Windows break by marking a common target `target_compatible_with` Unix:
  Bazel skips it and the platform's coverage vanishes behind a green run.
  Unavoidable guards use standard macros, not Qt's.
- **Name a suite for the type under test, without a `Test` suffix**
  (`SubaruHitachiM32rCanExecutor`).
- **A ported test's file header names the legacy source file only**, never a
  line range or commit: the legacy file is gone from the tree, so finer
  citations cannot be checked.
- **Comments explain why an input or failure mode matters**; delete ones that
  restate the test name.

### Python tooling tests

Keep Python tooling suites, test-only runners, and helpers in a `tests/`
directory under the owning script directory. Implementations stay outside it,
including production tools whose names end in `_test.py`, such as
[`mutation_test.py`](../scripts/mutation_test.py). Existing clang-tidy plugin
fixtures in `test/` and load checks in `smoke/` are also test-only directories.

Directory separation does not require a new Bazel package: the existing owning
BUILD file declares each `py_test` and its import roots. Run suites through
Bazel; shared implementation modules remain production code even when listed
in a test target's `srcs`.

Keep [Sonar's analysis scope](../sonar-project.properties) aligned with this
layout. The `//:sonar_scope_test` guard checks Python entry points, unittest
placement, and source/test classification using package-owned filegroups.
Include a new tooling package's Python files, BUILD file, and fixtures in its
guard inputs. See [tooling](tooling.md#sonarcloud) for verification.

### Mutation

**Prove a protocol correction with a mutation check:** without hardware
qualification, temporarily restore the defect, confirm a named regression
fails, then restore the fix.

**Treat a surviving mutant as a missing assertion.** The advisory PR `mutation`
job ([ADR 0022](adr/0022-advisory-mutation-testing-with-mull.md)) annotates
survivors on changed lines; add the assertion that kills each one. Log and
error text is observable behavior. A `Timeout` counts as killed. To run it, see
[tooling](tooling.md#mutation-testing).

**Suppress only an equivalent mutant, on its line, naming the mutator,** with
the reason on its own line above:

```cpp
// Equivalent as `>`: at offset == size the count below is zero.
// mull-ignore-next: cxx_ge_to_gt
if (offset >= out.size())
```

Mull keeps only letters, digits, `_` and `,` after the colon, so a reason on
that line suppresses nothing. Never use a bare or block suppression; they hide
real mutants added later. If a line mixes an equivalent and a real mutant of
the same mutator, simplify the expression in its own change instead. Mull
matches its annotations inside any comment, so do not spell them out in prose.

### Fuzzing

**Fuzz untrusted input** (ECU replies, ROM files, definitions, expressions,
terminal scripts) with a FuzzTest harness in `<unit>_fuzz_test.cpp` beside the
unit ([ADR 0021](adr/0021-fuzz-portable-code-with-fuzztest.md)). Assert
properties (round trips, output invariants, agreement between entry points),
not only "does not crash", and prefer typed domains (`VectorOf(...).WithMaxSize(255)`,
`InRange`).

**Seeds are small synthetic files** under `<package>/testdata/fuzz/<harness>/`,
in the target's `data` via `glob(["testdata/fuzz/**"])`, loaded with
`fuzztest::ReadFilesFromDirectory` and a workspace-relative path. Derive them
from existing fixtures; never commit stock ROM bytes or bench reads. Seeds are
at most 4 KiB. A minimized crash input becomes a seed in the fix's PR.

## Checks

Every header has `#pragma once` and no `using namespace`. `prek` runs
formatting, those header checks, Lizard and the link checker; clang-tidy (all findings are errors,
including the repository's custom checks) and SonarCloud gate the PR. Run them
locally before pushing; commands and CI details are in [tooling](tooling.md).
