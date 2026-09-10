# Strong timeout types in backend ports and transports

**Date:** 2026-09-10
**Status:** Approved, ready for planning
**Scope:** `src/backend/ports`, `src/backend/protocol`, `src/backend/flash` transport
and UDS interfaces, `src/backend/logging` protocol interfaces, and the
`src/platform/desktop` adapters that implement them.

## Problem

Every timeout in the backend is an untyped integer, and the integer type varies by
layer: `int timeoutMs` on the transport ports, `int ms` on `IClock::sleep`,
`std::uint64_t` from `IClock::now_ms()`, `std::uint16_t` on the serial read
surface, `unsigned long` at the J2534 boundary. Three consequences:

1. **Silent truncation.** `FastEcuKlineTransport::read` narrows with an unchecked
   `quint16(timeoutMs)`. A timeout above 65535 ms would wrap to a short one and
   the code would report a timeout that never happened.
2. **Cast noise in deadline arithmetic.** `portable_ssm_logging_protocol.cpp`
   computes deadlines as `static_cast<int>(clock_.now_ms() - start) < timeout_ms`,
   mixing an unsigned instant with a signed duration and casting to make it
   compile.
3. **Unit ambiguity carried by naming convention alone.** `_ms` suffixes are the
   only thing stopping a microsecond or second value from being passed; the
   compiler enforces nothing.

## Decision

Backend ports, transport interfaces, and the timing config structs they consume
take `std::chrono::milliseconds` by value. `IClock` additionally reports the
current instant as a `std::chrono::steady_clock::time_point`, so an instant and a
duration become distinct types.

The serial layer (`ISerialBackend`, `SerialPortActions`, the QtRO `.rep` replica
surface) and the UI/legacy flash call sites are explicitly **out of scope** for
this pass.

## Type surface

Parameters are named for their role without a unit suffix; the type carries the
unit.

```cpp
// src/backend/ports/clock.h
class IClock
{
  public:
    virtual ~IClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
    virtual Status sleep(std::chrono::milliseconds duration, const ICancellationToken&) = 0;
};
```

```cpp
// src/backend/protocol/{ikline,ican,issm}_transport.h
virtual Result<OptionalBytes> read(std::chrono::milliseconds timeout,
                                   const ICancellationToken& cancellation) = 0;
```

The same substitution applies to `MutDmaDriver::pollOnce`,
`MitsuColtCanCdbgDriver::pollOnce`, `IUdsChannel::receive`,
`IKlineFlashTransport::read`, `IKlineFlashTransport::pulse_lec_2_line`, and
`ILoggingProtocol::poll`.

Timing config structs lose the `_ms` suffix and gain the type:

```cpp
struct uds::ExchangePolicy
{
    std::chrono::milliseconds pre_read_delay{0};
    std::chrono::milliseconds read_timeout{500};
    std::chrono::milliseconds pending_timeout{3000};
    int max_pending_repeats = 10;   // a count, not a duration
};

struct LoggingPolicy
{
    std::chrono::milliseconds poll_timeout;
    std::chrono::milliseconds reconnect_retry_period;
    int car_silence_miss_threshold;
    int reconnect_attempt_threshold;
};
```

`max_pending_repeats`, `car_silence_miss_threshold`, and
`reconnect_attempt_threshold` are counts and stay `int`.

### Literals

`using namespace std::chrono_literals` appears in `.cpp` files and test bodies
only, never at header scope, because a using-directive in a header leaks into
every translation unit that includes it. Headers spell the type out:
`std::chrono::milliseconds{500}`.

## The narrowing boundary

Three different integral targets sit below the ports: `std::uint16_t` for
`read_serial_data`, `int` for `pulse_lec_*`, and `unsigned long` for the J2534
vendor API. One shared helper replaces every ad-hoc cast:

```cpp
// src/backend/ports/duration_cast.h -- portable, header-only
namespace fastecu
{
// Clamps to [0, max of T]. Saturating rather than asserting: a too-long timeout
// should wait as long as the wire type allows, never abort a flash mid-write.
template <std::integral T>
constexpr T saturating_ms(std::chrono::milliseconds duration) noexcept;
}
```

Adapters call `saturating_ms<quint16>(timeout)`. The helper lives under
`src/backend/ports` so portable backend code and platform adapters share one
implementation; it depends on `<chrono>`, `<concepts>`, and `<limits>` only, so
`//:portable_closure` is unaffected.

## FakeClock

`FakeClock` stores elapsed time as a duration and derives the instant from it, so
tests never do `time_point` arithmetic:

```cpp
class FakeClock : public IClock
{
  public:
    std::chrono::steady_clock::time_point now() const override;  // epoch + elapsed_
    Status sleep(std::chrono::milliseconds, const ICancellationToken&) override;

    std::chrono::milliseconds elapsed() const;                   // replaces public now_
    void set_now_auto_advance(std::chrono::milliseconds step);
    void set_sleep_advance(std::optional<std::chrono::milliseconds> step);
};

FakeClock make_auto_advancing_clock(std::chrono::milliseconds step);
```

Eight tests currently assert on the public `now_` member
(`EXPECT_EQ(clock.now_, 2150U)`). They become `EXPECT_EQ(clock.elapsed(), 2150ms)`.
The public mutable member is removed; `elapsed()` is the read accessor and the
two `set_*_advance` setters remain the write path.

## Out of scope

- `ISerialBackend`, `SerialPortActions`, `SerialPortActionsDirect`, and
  `serial_port_actions.rep` keep integral signatures. Moving a chrono type across
  the QtRO replica boundary requires a registered metatype and is its own
  decision; this pass does not open it.
- `transport_legacy_compat.h`'s free functions keep `int timeout_ms` in their own
  signatures and construct `milliseconds` internally, so legacy flash operations
  and UI call sites compile unchanged.
- The named constants on `SerialPortActionsDirect`
  (`serial_read_short_timeout` and siblings) and their UI duplicates.
- `accurate_delay(double)` / `fast_delay(int)` / `delay(int)` on the serial layer.

## Sequencing

Four cutovers, each self-contained: every one takes a whole interface plus all its
implementers, fakes, and callers, so no interface is left half-typed between PRs.
Each compiles and passes `bazel test --config=release //...` on its own.

| # | Scope | Files | Contents |
|---|-------|-------|----------|
| 1 | Transports | ~40 | The three transport ports, `MutDmaDriver::pollOnce`, `MitsuColtCanCdbgDriver::pollOnce`, the three scripted transport fakes, the three `FastEcu*Transport` adapters, `transport_legacy_compat.h` internals. Introduces `duration_cast.h` and its test. |
| 2 | `IClock` | ~63 | `clock.h`, `FakeClock`, `QtClock`, and every `sleep()` / `now_ms()` caller — chiefly `src/backend/flash/ecu/*` executors and their tests. |
| 3 | UDS and flash channels | ~32 | `IUdsChannel::receive`, `ExchangePolicy`, `UdsClient`, `CanFlashUdsChannel`, `IKlineFlashTransport::read` and `pulse_lec_2_line`, the scripted flash transports. |
| 4 | Logging | ~17 | `ILoggingProtocol::poll`, the three portable protocols, `LoggingPolicy`, `logging_session` validation. |

PR 1 is first because it is the smallest complete unit and proves the pattern —
including `saturating_ms` at a real Qt boundary — before the 63-file PR 2.

## Testing

No behavior changes, so the existing suites are the regression net and the
compiler is the primary verifier: `std::chrono::milliseconds` does not
implicitly convert from `int`, so an unconverted call site is a build error
rather than a silent pass. A deprecation shim or transitional `int` overload
would defeat exactly that property and is not used.

New or changed tests:

- **`saturating_ms`** — new unit test covering zero, a negative duration clamped
  to 0, exactly the target type's maximum, a value above it, and
  `milliseconds::max()`.
- **`fake_clock_test.cpp`** — `elapsed()`, `advance` setters taking durations, and
  that `sleep` advances by the passed duration.
- **Scripted fakes** — recorded timeout vectors become
  `std::vector<std::chrono::milliseconds>`; assertions become
  `ElementsAre(500ms, 200ms)`. `scripted_kline_flash_transport.h`'s
  `if (timeout_ms == 10)` becomes `== 10ms`.

Per PR: `bazel test --config=release //...`, `prek run --all-files`,
`bazel run //:clang_tidy_report_changed`.

## Hardware safety

This pass is behavior-preserving by construction: every value stays numerically
identical, and `saturating_ms` differs from the current `quint16(...)` cast only
above 65535 ms. Every timeout on the converted paths is a compile-time literal,
and the largest is 3000 ms, so no live path changes behavior. (The serial layer's
`echo_check_timout = 5000` is larger but out of scope and still well under the
narrowing limit.)

Each PR verifies this rather than assuming it: sweep the converted paths for any
timeout value exceeding the narrowing target's maximum, and confirm the sweep
finds none. No bench re-qualification is required and
`docs/flash-qualification-matrix.md` is unchanged.

## Follow-up work, not committed here

- The serial surface (`ISerialBackend` / `SerialPortActions` / `.rep`), which
  requires deciding how a duration crosses QtRO.
- The `SerialPortActionsDirect` named timeout constants and their UI duplicates.
- The UI and legacy flash call sites still reaching the ports through
  `transport_legacy_compat.h`.

## A note on what "UI out of scope" means

Out of scope means the UI's *serial* call sites — `read_serial_data(200)` and
friends in `ecu_operations.cpp`, `dtc_operations.cpp`, `dataterminal.cpp` — keep
their integers. It does not mean no file under `src/ui/desktop` is touched:
converting `LoggingPolicy` requires editing the three aggregate initializers in
`menu_actions.cpp` that construct it (`{.poll_timeout_ms = 50, ...}` becomes
`{.poll_timeout = 50ms, ...}`). Those are constructions of a backend struct that
happen to live in a UI file, and they belong with PR 4.
