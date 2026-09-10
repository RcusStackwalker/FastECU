# Chrono Timeouts in Ports and Transports Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace untyped integer timeouts on the backend ports, transport interfaces, and their timing config structs with `std::chrono::milliseconds`, and report `IClock` instants as a `std::chrono::steady_clock::time_point`.

**Architecture:** Seven hard cutovers, each converting one whole interface plus every implementer, fake, and caller of it, so no interface is ever left half-typed. No transitional `int` overload exists anywhere — `std::chrono::milliseconds` does not implicitly convert from `int`, so an unconverted call site is a build error, and that property is the primary verification. A single `saturating_ms<T>()` helper replaces the unchecked narrowing casts where a duration meets an integral wire API.

**Tech Stack:** C++23, Bazel 9.1.1, GoogleTest via `fastecu_portable_gtest` / `fastecu_gtest`, QtTest via `fastecu_qttest`, Qt 6.8.3.

**Spec:** [docs/superpowers/specs/2026-09-10-chrono-timeouts-ports-transports-design.md](../specs/2026-09-10-chrono-timeouts-ports-transports-design.md)

## Global Constraints

- Parameters are named for their role with **no `_ms` suffix** — the type carries the unit. `read(std::chrono::milliseconds timeout, ...)`, not `timeout_ms`.
- `using namespace std::chrono_literals` appears in `.cpp` files and test bodies **only, never at header scope**. Headers spell out `std::chrono::milliseconds{500}`.
- Counts stay `int`: `max_pending_repeats`, `car_silence_miss_threshold`, `reconnect_attempt_threshold`. Only durations convert.
- Timeouts are passed **by value**, never by const reference.
- Out of scope, must not be touched: `ISerialBackend`, `SerialPortActions`, `SerialPortActionsDirect`, `serial_port_actions.rep`, the `serial_read_*_timeout` named constants, `accurate_delay` / `fast_delay` / `delay`, and the UI's own serial call sites (`read_serial_data(200)` in `ecu_operations.cpp`, `dtc_operations.cpp`, `dataterminal.cpp`).
- `transport_legacy_compat.h`'s free functions keep `int timeout_ms` in their own signatures and construct `milliseconds` internally.
- No behavior changes. Every numeric value stays identical.
- Every header needs `#pragma once` (enforced by prek).
- Per task, before committing: `bazel test --config=release //...`, `prek run --all-files`, `bazel run //:clang_tidy_report_changed`.
- Branch per PR group; never commit to `master` (a prek hook rejects it).

## PR grouping

Tasks map onto the spec's four PRs:

| PR | Branch | Tasks |
|----|--------|-------|
| 1 | `refactor/chrono-transports` | 1, 2, 3, 4 |
| 2 | `refactor/chrono-clock` | 5 |
| 3 | `refactor/chrono-uds` | 6 |
| 4 | `refactor/chrono-logging` | 7 |

Each task is its own commit. Open the PR after the last task in its group.

## A note on method

Tasks 2 through 7 are hard cutovers of an existing interface. The reliable procedure is: change the interface, then run `bazel build --config=release //...` and let the compiler enumerate every remaining site. Each task lists the files known to need changes, but **the compiler is the authority, not the list** — if the build names a file the task does not mention, convert it and note it in the commit message.

---

### Task 1: The `saturating_ms` boundary helper

Genuinely new code, so genuine TDD. It replaces three unchecked narrowing casts in the Qt transport adapters, where a timeout above 65535 ms currently wraps to a short one and reports a timeout that never happened.

**Files:**
- Create: `src/backend/ports/duration_cast.h`
- Create: `src/backend/ports/duration_cast_test.cpp`
- Modify: `src/backend/ports/BUILD.bazel`

**Interfaces:**
- Consumes: nothing.
- Produces: `template <std::integral T> constexpr T fastecu::saturating_ms(std::chrono::milliseconds duration) noexcept` — clamps to `[0, std::numeric_limits<T>::max()]`. Used by Tasks 2, 3, 4, and 6.

- [ ] **Step 1: Write the failing test**

Create `src/backend/ports/duration_cast_test.cpp`:

```cpp
#include "src/backend/ports/duration_cast.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

using namespace std::chrono_literals;
using fastecu::saturating_ms;

TEST(SaturatingMs, ConvertsAValueInRange)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(500ms), 500);
    EXPECT_EQ(saturating_ms<int>(3000ms), 3000);
    EXPECT_EQ(saturating_ms<unsigned long>(200ms), 200UL);
}

TEST(SaturatingMs, ZeroStaysZero)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(0ms), 0);
}

TEST(SaturatingMs, NegativeClampsToZero)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(-1ms), 0);
    EXPECT_EQ(saturating_ms<int>(-5000ms), 0);
}

TEST(SaturatingMs, ExactMaximumIsPreserved)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(std::chrono::milliseconds{65535}), 65535);
}

TEST(SaturatingMs, AboveMaximumSaturatesRatherThanWrapping)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(std::chrono::milliseconds{65536}), 65535);
    EXPECT_EQ(saturating_ms<std::uint16_t>(70000ms), 65535);
}

TEST(SaturatingMs, DurationMaxSaturates)
{
    EXPECT_EQ(saturating_ms<std::uint16_t>(std::chrono::milliseconds::max()), 65535);
    EXPECT_EQ(saturating_ms<int>(std::chrono::milliseconds::max()), std::numeric_limits<int>::max());
}

TEST(SaturatingMs, IsUsableInAConstantExpression)
{
    static_assert(saturating_ms<std::uint16_t>(70000ms) == 65535);
    SUCCEED();
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/ports:duration_cast_test`
Expected: FAIL — the target does not exist yet.

- [ ] **Step 3: Write the header**

Create `src/backend/ports/duration_cast.h`:

```cpp
#pragma once

#include <chrono>
#include <concepts>
#include <limits>
#include <utility>

namespace fastecu
{

// Converts a duration to the integral millisecond count an integral wire API
// wants, clamping to [0, max of T].
//
// Saturating rather than asserting: a too-long timeout should wait as long as
// the wire type allows, never abort a flash mid-write. Clamping negatives to
// zero matches every caller's existing "no wait" reading of a non-positive
// timeout.
template <std::integral T>
constexpr T saturating_ms(std::chrono::milliseconds duration) noexcept
{
    using Limits = std::numeric_limits<T>;
    const auto count = duration.count();
    if (count <= 0)
    {
        return T{0};
    }
    // count is a signed 64-bit rep; compare in that width so a 64-bit T
    // maximum does not itself overflow the comparison.
    if (std::cmp_greater(count, Limits::max()))
    {
        return Limits::max();
    }
    return static_cast<T>(count);
}

} // namespace fastecu
```

`<utility>` is there for `std::cmp_greater`, which compares across signedness without the sign-conversion warning a bare `>` would raise.

- [ ] **Step 4: Register the header and the test**

In `src/backend/ports/BUILD.bazel`, add `"duration_cast.h"` to the `hdrs` list of the `ports` target, keeping the list alphabetical — it goes between `"clock.h"` and `"error.h"`. Then append:

```python
fastecu_portable_gtest(
    name = "duration_cast_test",
    srcs = ["duration_cast_test.cpp"],
    deps = [":ports"],
)
```

No `//:portable_closure` registration is needed: the header joins the existing `:ports` target rather than creating a new portable root, and `<chrono>` is standard library, not Qt.

- [ ] **Step 5: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/ports:duration_cast_test`
Expected: PASS, 7 tests.

- [ ] **Step 6: Verify the portable closure is still clean**

Run: `bazel test --config=release //:portable_closure`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git checkout -b refactor/chrono-transports
git add src/backend/ports/duration_cast.h src/backend/ports/duration_cast_test.cpp src/backend/ports/BUILD.bazel
git commit -m "feat(ports): add saturating_ms duration-to-integral helper

Replaces unchecked narrowing at the integral wire boundaries. A timeout
above the target type's maximum saturates instead of wrapping, so a long
timeout can no longer become a short one."
```

---

### Task 2: Convert `ISsmTransport`

The smallest of the three transports and the one with no driver attached — it proves the pattern end to end, including `saturating_ms` at a real Qt boundary.

**Files:**
- Modify: `src/backend/protocol/issm_transport.h:29`
- Modify: `src/backend/protocol/testing/scripted_ssm_transport.h:71`
- Modify: `src/backend/protocol/transport_legacy_compat.h:31-39`
- Modify: `src/platform/desktop/common/transport/fastecu_ssm_transport.h:13`
- Modify: `src/platform/desktop/common/transport/fastecu_ssm_transport.cpp:32,46`
- Modify: `src/backend/logging/protocols/portable_ssm_logging_protocol.cpp` (the `read_and_append` lambda)
- Modify: `src/backend/protocol/transport_test.cpp` (the `t.read(20, token)` / `t.read(50, token)` call sites)

**Interfaces:**
- Consumes: `fastecu::saturating_ms` from Task 1.
- Produces: `ISsmTransport::read(std::chrono::milliseconds timeout, const fastecu::ICancellationToken&)`. Task 7 relies on this when converting the SSM logging protocol's own signature.

- [ ] **Step 1: Change the interface**

In `src/backend/protocol/issm_transport.h`, add `#include <chrono>` to the include block and change the read declaration:

```cpp
    virtual fastecu::Result<OptionalBytes> read(std::chrono::milliseconds timeout,
                                                const fastecu::ICancellationToken& cancellation) = 0;
```

- [ ] **Step 2: Run the build to enumerate the call sites**

Run: `bazel build --config=release //...`
Expected: FAIL, with errors naming each implementer and caller. That list is the authoritative worklist for this task.

- [ ] **Step 3: Convert the Qt adapter, replacing the unchecked cast**

In `src/platform/desktop/common/transport/fastecu_ssm_transport.h`, add `#include <chrono>` and change the override to match the interface. In `fastecu_ssm_transport.cpp`, add `#include "src/backend/ports/duration_cast.h"`, change the definition's parameter to `std::chrono::milliseconds timeout`, and replace line 46:

```cpp
        const QByteArray raw = serial_->read_serial_data(fastecu::saturating_ms<std::uint16_t>(timeout));
```

That replaces `static_cast<uint16_t>(timeoutMs)`, which truncated silently above 65535 ms.

- [ ] **Step 4: Convert the scripted fake**

In `src/backend/protocol/testing/scripted_ssm_transport.h`, add `#include <chrono>` and change the override's unnamed parameter:

```cpp
    fastecu::Result<OptionalBytes> read(std::chrono::milliseconds, const fastecu::ICancellationToken& cancellation) override
```

The fake ignores the timeout, so nothing else changes.

- [ ] **Step 5: Keep the legacy compat shim integral**

In `src/backend/protocol/transport_legacy_compat.h`, add `#include <chrono>`. The free function keeps its `int` signature per the spec; only the forwarded call changes:

```cpp
inline bytes::Bytes read(ISsmTransport& transport, int timeout_ms)
{
    auto result = transport.read(std::chrono::milliseconds{timeout_ms}, detail::never_cancelled());
```

- [ ] **Step 6: Convert the SSM logging protocol's call site**

`portable_ssm_logging_protocol.cpp` keeps its own `int timeout_ms` signature until Task 7. Only the lambda that touches the transport changes — add `#include <chrono>`, put `using namespace std::chrono_literals;` in the file's anonymous namespace or at function scope, and change:

```cpp
    const auto read_and_append = [&](std::chrono::milliseconds read_timeout) -> fastecu::Status
```

Its three call sites become `read_and_append(std::chrono::milliseconds{timeout_ms})`, `read_and_append(10ms)` (twice), and `read_and_append(std::chrono::milliseconds{remaining})`. Leave the `static_cast<int>(clock_.now_ms() - start)` deadline arithmetic exactly as it is; Task 5 rewrites it.

- [ ] **Step 7: Convert the test call sites**

In `src/backend/protocol/transport_test.cpp`, add `using namespace std::chrono_literals;` and change `t.read(20, token)` to `t.read(20ms, token)` and `t.read(50, token)` to `t.read(50ms, token)`.

- [ ] **Step 8: Verify**

Run: `bazel test --config=release //...`
Expected: PASS. Then `prek run --all-files` and `bazel run //:clang_tidy_report_changed`.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor(protocol): take std::chrono::milliseconds in ISsmTransport::read

Replaces the unchecked static_cast<uint16_t> in FastEcuSsmTransport with
saturating_ms. The legacy compat shim keeps its int signature and wraps
internally, so UI and legacy flash callers are unaffected."
```

---

### Task 3: Convert `IKlineTransport` and `MutDmaDriver`

The largest of the three, because `IKlineFlashTransport` inherits `mutdma::IKlineTransport` (`flash_executor.h:146`) — the flash K-Line implementers override the same `read`, so they convert here rather than in Task 6.

**Files:**
- Modify: `src/backend/protocol/ikline_transport.h:21`
- Modify: `src/backend/protocol/mut_dma_driver.h:30`, `mut_dma_driver.cpp:43,57,88,101,108`
- Modify: `src/backend/protocol/testing/scripted_kline_transport.h:83`
- Modify: `src/backend/protocol/transport_legacy_compat.h:52-60`
- Modify: `src/platform/desktop/common/transport/fastecu_kline_transport.h:15`, `fastecu_kline_transport.cpp:63,76`
- Modify: `src/platform/desktop/common/transport/desktop_kline_flash_transport.h:53`, `.cpp` (the `read` definition only)
- Modify: `src/backend/flash/testing/scripted_kline_flash_transport.h:155-158,197`
- Modify: flash executors' K-Line read call sites — `subaru_denso_mc68hc16y5_02_executor.cpp`, `subaru_denso_sh7055_02_executor.cpp`, `subaru_hitachi_m32r_kline_executor.cpp`, `subaru_mitsu_m32r_kline_executor.cpp`, `denso_sh705x_eeprom_kline_executor.cpp`
- Modify: `src/backend/service_functions/relearn_session.cpp`, `read_parameters_session.cpp`, `set_parameters_session.cpp`
- Modify: `src/backend/logging/protocols/portable_mut_dma_logging_protocol.cpp:72`
- Modify: `src/backend/protocol/driver_test.cpp:72,184`, `src/platform/desktop/common/transport/desktop_kline_flash_transport_test.cpp`, `tests/tst_mut_dma_integration.cpp:413`
- Modify: `src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor_test.cpp`, `subaru_denso_sh7055_02_executor_test.cpp` (the `read_timeouts_` assertions)

**Interfaces:**
- Consumes: `fastecu::saturating_ms` from Task 1.
- Produces: `mutdma::IKlineTransport::read(std::chrono::milliseconds timeout, const ICancellationToken&)`; `MutDmaDriver::pollOnce(std::chrono::milliseconds timeout, const ICancellationToken&)`; `ScriptedKlineFlashTransport::read_timeouts_` as `std::vector<std::chrono::milliseconds>`.

- [ ] **Step 1: Change the interface and the driver**

In `src/backend/protocol/ikline_transport.h`, add `#include <chrono>` and:

```cpp
    virtual fastecu::Result<OptionalBytes> read(std::chrono::milliseconds timeout,
                                                const fastecu::ICancellationToken& cancellation) = 0;
```

In `src/backend/protocol/mut_dma_driver.h`, add `#include <chrono>` and:

```cpp
    fastecu::Result<std::vector<std::uint32_t>> pollOnce(std::chrono::milliseconds timeout,
                                                         const fastecu::ICancellationToken& cancellation);
```

- [ ] **Step 2: Run the build to enumerate the call sites**

Run: `bazel build --config=release //...`
Expected: FAIL. Work the error list; the Files section above is the expected shape of it.

- [ ] **Step 3: Convert `mut_dma_driver.cpp`**

Add `using namespace std::chrono_literals;` inside the file's existing anonymous namespace. The three handshake reads `t_.read(50, cancellation)` become `t_.read(50ms, cancellation)`; the `pollOnce` definition takes `std::chrono::milliseconds timeout` and forwards it to `t_.read(timeout, cancellation)`.

- [ ] **Step 4: Convert the Qt adapter, replacing the unchecked cast**

In `fastecu_kline_transport.cpp`, add `#include "src/backend/ports/duration_cast.h"` and replace line 76:

```cpp
        const QByteArray raw = serial_->read_serial_data(fastecu::saturating_ms<quint16>(timeout));
```

That replaces `quint16(timeoutMs)` — the truncation named in the spec's problem statement.

- [ ] **Step 5: Convert the flash K-Line transports**

`DesktopKlineFlashTransport::read` and `ScriptedKlineFlashTransport::read` are overrides of the inherited `IKlineTransport::read`; convert both signatures. In `desktop_kline_flash_transport.cpp`, use `saturating_ms<quint16>(timeout)` wherever the read forwards to the serial facade. Leave `pulse_lec_2_line(int)` alone — that is Task 6.

In `scripted_kline_flash_transport.h`, change the recorder member and the sentinel:

```cpp
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        read_timeouts_.push_back(timeout);
        if (timeout == 10ms)
        {
            operation_trace_.push_back(Operation::Read10);
        }
```

and `std::vector<int> read_timeouts_;` becomes `std::vector<std::chrono::milliseconds> read_timeouts_;`. `lec_2_pulse_timeouts_` stays `std::vector<int>` until Task 6.

- [ ] **Step 6: Convert the executor and session call sites**

Add `using namespace std::chrono_literals;` to each affected `.cpp` (file scope inside the anonymous namespace where one exists, otherwise just below the includes — never in a header) and suffix the literals: `read(3000, ...)` becomes `read(3000ms, ...)`, `read(50, ...)` becomes `read(50ms, ...)`, and so on. Values do not change.

- [ ] **Step 7: Convert the test assertions**

In the two executor test files, the `read_timeouts_` assertions become duration comparisons:

```cpp
    EXPECT_EQ(transport.read_timeouts_, (std::vector<std::chrono::milliseconds>{10ms}));
    EXPECT_EQ(std::count(transport.read_timeouts_.begin(), transport.read_timeouts_.end(), 200ms), 12);
```

Add `using namespace std::chrono_literals;` to each test file. `lec_2_pulse_timeouts_` assertions stay `(std::vector<int>{200})` for now.

- [ ] **Step 8: Verify**

Run: `bazel test --config=release //...`
Expected: PASS, including `//src/backend/flash/ecu:all` and `//tests:all`. Then `prek run --all-files` and `bazel run //:clang_tidy_report_changed`.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "refactor(protocol): take std::chrono::milliseconds in IKlineTransport::read

IKlineFlashTransport inherits IKlineTransport, so the desktop and
scripted flash transports override the same read and convert here.
FastEcuKlineTransport's quint16(timeoutMs) truncation becomes
saturating_ms."
```

---

### Task 4: Convert `ICanTransport` and `CdbgLogDriver`

**Files:**
- Modify: `src/backend/protocol/ican_transport.h:31`
- Modify: `src/backend/protocol/mitsu_colt_can_cdbg_driver.h:50`, `.cpp:25,123,131`
- Modify: `src/backend/protocol/testing/scripted_can_transport.h:63`
- Modify: `src/backend/protocol/transport_legacy_compat.h:68-77`
- Modify: `src/platform/desktop/common/transport/fastecu_can_transport.h:17`, `.cpp:39,53`
- Modify: `src/backend/logging/protocols/portable_cdbg_logging_protocol.cpp:70`
- Modify: `src/backend/protocol/cdbg_driver_test.cpp:45,166,175,188`
- Modify: `src/ui/desktop/log_operations_ssm.cpp:556`

**Interfaces:**
- Consumes: `fastecu::saturating_ms` from Task 1.
- Produces: `cdbg::ICanTransport::read(std::chrono::milliseconds timeout, const ICancellationToken&)`; `CdbgLogDriver::pollOnce(std::chrono::milliseconds timeout, const ICancellationToken&)`.

- [ ] **Step 1: Change the interface and the driver**

In `src/backend/protocol/ican_transport.h`, add `#include <chrono>` and:

```cpp
    virtual fastecu::Result<std::optional<CanFrame>> read(std::chrono::milliseconds timeout,
                                                          const fastecu::ICancellationToken& cancellation) = 0;
```

In `mitsu_colt_can_cdbg_driver.h`, add `#include <chrono>` and change `pollOnce` to take `std::chrono::milliseconds timeout`.

- [ ] **Step 2: Run the build to enumerate the call sites**

Run: `bazel build --config=release //...`
Expected: FAIL, naming the files listed above.

- [ ] **Step 3: Convert the driver, the adapter, and the fake**

In `mitsu_colt_can_cdbg_driver.cpp`, add `using namespace std::chrono_literals;` to the anonymous namespace; `transport.read(250, cancellation)` becomes `transport.read(250ms, cancellation)`, and `pollOnce` forwards its `timeout` unchanged.

In `fastecu_can_transport.cpp`, add `#include "src/backend/ports/duration_cast.h"` and replace line 53:

```cpp
        const bytes::Bytes raw = bytes::fromQByteArray(serial_->read_serial_data(fastecu::saturating_ms<quint16>(timeout)));
```

In `scripted_can_transport.h`, change the unnamed parameter to `std::chrono::milliseconds`.

- [ ] **Step 4: Convert the remaining callers**

`transport_legacy_compat.h`'s CAN `read` keeps its `int` signature and wraps: `transport.read(std::chrono::milliseconds{timeout_ms}, detail::never_cancelled())`.

`portable_cdbg_logging_protocol.cpp:70` keeps its own `int timeout_ms` signature until Task 7; only the forwarded call changes to `driver_.pollOnce(std::chrono::milliseconds{timeout_ms}, cancellation)`.

`src/ui/desktop/log_operations_ssm.cpp:556` becomes `d.pollOnce(50ms, cancellation)` with `using namespace std::chrono_literals;` at the top of the function. This is a backend driver call that happens to live in a UI file — it is in scope; the UI's own `read_serial_data(...)` calls are not.

`cdbg_driver_test.cpp`'s four `d.pollOnce(50, cancellation)` calls become `50ms`.

- [ ] **Step 5: Verify**

Run: `bazel test --config=release //...`
Expected: PASS. Then `prek run --all-files` and `bazel run //:clang_tidy_report_changed`.

- [ ] **Step 6: Sweep for surviving truncation**

Run: `grep -rn "quint16(\|static_cast<uint16_t>(\|static_cast<std::uint16_t>(" src/platform/desktop/common/transport/`
Expected: no hits on a timeout argument. Any remaining hit is either a non-timeout conversion or a missed site.

- [ ] **Step 7: Commit and open PR 1**

```bash
git add -A
git commit -m "refactor(protocol): take std::chrono::milliseconds in ICanTransport::read

Completes the transport-port cutover: all three ports, both drivers,
the scripted fakes, and the three Qt adapters now take durations, with
saturating_ms at every narrowing boundary."
git push -u origin refactor/chrono-transports
gh pr create --title "refactor(protocol): chrono timeouts on the transport ports" --body "$(cat <<'BODY'
Converts ISsmTransport, IKlineTransport, and ICanTransport (plus MutDmaDriver
and CdbgLogDriver) to std::chrono::milliseconds, and adds the saturating_ms
helper that replaces three unchecked narrowing casts in the Qt adapters.

IKlineFlashTransport inherits IKlineTransport, so the desktop and scripted
flash K-Line transports convert here too.

Behavior-preserving: every numeric value is unchanged, and saturating_ms
differs from the old cast only above 65535 ms — the largest timeout on these
paths is 3000 ms.

Spec: docs/superpowers/specs/2026-09-10-chrono-timeouts-ports-transports-design.md

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01FHF3HcucUP9pbXoxRw1BW1
BODY
)"
```

---

### Task 5: Convert `IClock`

The largest task by file count (~63), and the only one that changes a return type. `sleep` takes a duration; `now()` returns an instant, so the two can no longer be confused.

**Files:**
- Modify: `src/backend/ports/clock.h`
- Modify: `src/backend/ports/testing/fake_clock.h`
- Modify: `src/backend/ports/testing/fake_clock_test.cpp`
- Modify: `src/platform/desktop/common/ports/qt_clock.h`, `qt_clock.cpp`
- Modify: `src/platform/desktop/common/ports/qt_port_adapters_test.cpp`
- Modify: every `sleep()` caller — chiefly `src/backend/flash/ecu/*.cpp` and `src/backend/flash/eeprom/*.cpp` and their `_test.cpp` siblings
- Modify: `src/backend/protocol/uds/uds_client.cpp:52`
- Modify: `src/backend/logging/protocols/portable_ssm_logging_protocol.cpp` (deadline arithmetic)
- Modify: `apps/bench/bench_session.cpp:93,95,217,220`
- Modify: the eight tests asserting `clock.now_` — `uds_client_test.cpp:65`, `subaru_denso_mc68hc16y5_02_executor_test.cpp:563,623,993,1130,1171`, `subaru_denso_sh72543_can_diesel_executor_test.cpp:575`, `subaru_denso_sh72531_can_executor_test.cpp:504`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `IClock::now()` returning `std::chrono::steady_clock::time_point`; `IClock::sleep(std::chrono::milliseconds duration, const ICancellationToken&)`; `FakeClock::elapsed()` returning `std::chrono::milliseconds`; `FakeClock::set_now_auto_advance(std::chrono::milliseconds)`; `FakeClock::set_sleep_advance(std::optional<std::chrono::milliseconds>)`; `make_auto_advancing_clock(std::chrono::milliseconds)`.

- [ ] **Step 1: Change the port**

Replace the body of `src/backend/ports/clock.h`:

```cpp
#pragma once
#include <chrono>

#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

namespace fastecu
{

// Monotonic time source and cancellable delay. Replaces QElapsedTimer /
// QThread::msleep in backend code.
class IClock
{
  public:
    virtual ~IClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
    // Returns Error{Cancelled} if the token trips before the delay elapses.
    virtual Status sleep(std::chrono::milliseconds duration, const ICancellationToken&) = 0;
};

} // namespace fastecu
```

- [ ] **Step 2: Write the failing FakeClock test**

Rewrite `src/backend/ports/testing/fake_clock_test.cpp` against the new shape. `elapsed()` replaces the public `now_` member, so the timing model is asserted as a duration:

```cpp
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include <gtest/gtest.h>

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::Status;

TEST(FakeClock, OptionalAutoAdvancePreservesSsmTimingModel)
{
    FakeClock clock;
    clock.set_now_auto_advance(10ms);
    clock.set_sleep_advance(10ms);
    fastecu::FakeCancellationToken active;

    EXPECT_EQ(clock.elapsed(), 0ms);
    const auto first = clock.now();
    const auto second = clock.now();
    EXPECT_EQ(second - first, 10ms);
    EXPECT_EQ(clock.elapsed(), 20ms);
    // set_sleep_advance overrides the requested duration, so 999ms advances by 10ms.
    ASSERT_TRUE(clock.sleep(999ms, active));
    EXPECT_EQ(clock.elapsed(), 30ms);
}

TEST(FakeClock, MakeAutoAdvancingClockConfiguresBothTimingModels)
{
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken active;

    const auto first = clock.now();
    const auto second = clock.now();
    EXPECT_EQ(second - first, 10ms);
    ASSERT_TRUE(clock.sleep(999ms, active));
    EXPECT_EQ(clock.elapsed(), 30ms);
}

TEST(Clock, SleepAdvancesAndSucceeds)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    Status s = c.sleep(10ms, t);
    EXPECT_TRUE(s.has_value());
    EXPECT_EQ(c.elapsed(), 10ms);
}

TEST(Clock, SleepReturnsCancelledWhenTokenSet)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    t.set_cancelled(true);
    Status s = c.sleep(10ms, t);
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(c.elapsed(), 0ms);
}

TEST(Clock, NegativeSleepDoesNotRewindTheClock)
{
    FakeClock c;
    fastecu::FakeCancellationToken t;
    ASSERT_TRUE(c.sleep(-5ms, t));
    EXPECT_EQ(c.elapsed(), 0ms);
}
```

These preserve the original file's timing model exactly — the old assertions were `now_ms()` returning 0, then 10, then 30 after a `sleep(999)` capped by `set_sleep_advance_ms(10)`. The difference is that the auto-advance step is now observed as a `10ms` duration between two instants rather than as a bare integer.

- [ ] **Step 3: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/ports/testing:fake_clock_test`
Expected: FAIL — `set_now_auto_advance` and `elapsed` do not exist.

- [ ] **Step 4: Rewrite FakeClock**

Replace `src/backend/ports/testing/fake_clock.h`:

```cpp
#pragma once
#include <chrono>
#include <optional>

#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"

namespace fastecu
{

// A deterministic clock for tests: time advances only when told; sleep is
// instantaneous (no real wall-clock wait) but honours the cancellation token.
//
// Elapsed time is stored as a duration and the instant derived from it, so
// tests assert on elapsed() and never do time_point arithmetic.
class FakeClock : public IClock
{
  public:
    std::chrono::steady_clock::time_point now() const override
    {
        const auto value = elapsed_;
        elapsed_ += now_auto_advance_;
        return std::chrono::steady_clock::time_point{} + value;
    }

    Status sleep(std::chrono::milliseconds duration, const ICancellationToken& t) override
    {
        if (t.cancelled())
        {
            return fail(ErrorKind::Cancelled);
        }
        elapsed_ += sleep_advance_.value_or(duration < std::chrono::milliseconds::zero()
                                                ? std::chrono::milliseconds::zero()
                                                : duration);
        return {};
    }

    std::chrono::milliseconds elapsed() const
    {
        return elapsed_;
    }

    void set_now_auto_advance(std::chrono::milliseconds step)
    {
        now_auto_advance_ = step;
    }

    void set_sleep_advance(std::optional<std::chrono::milliseconds> step)
    {
        sleep_advance_ = step;
    }

  private:
    mutable std::chrono::milliseconds elapsed_{0};
    mutable std::chrono::milliseconds now_auto_advance_{0};
    std::optional<std::chrono::milliseconds> sleep_advance_;
};

inline FakeClock make_auto_advancing_clock(std::chrono::milliseconds step)
{
    FakeClock clock;
    clock.set_now_auto_advance(step);
    clock.set_sleep_advance(step);
    return clock;
}

} // namespace fastecu
```

Note the public mutable `now_` member is gone; `elapsed()` is the read path.

- [ ] **Step 5: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/ports/testing:fake_clock_test`
Expected: PASS.

- [ ] **Step 6: Rewrite QtClock**

`src/platform/desktop/common/ports/qt_clock.h` declares the new signatures; `qt_clock.cpp` becomes:

```cpp
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/backend/ports/duration_cast.h"
#include <QElapsedTimer>
#include <QThread>

using namespace std::chrono_literals;

std::chrono::steady_clock::time_point QtClock::now() const
{
    static QElapsedTimer base = []
    {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{base.elapsed()};
}

fastecu::Status QtClock::sleep(std::chrono::milliseconds duration, const fastecu::ICancellationToken& t)
{
    constexpr auto slice = 10ms;
    auto remaining = duration;
    while (remaining > 0ms)
    {
        if (t.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled);
        }
        const auto step = remaining < slice ? remaining : slice;
        QThread::msleep(fastecu::saturating_ms<unsigned long>(step));
        remaining -= step;
    }
    return {};
}
```

- [ ] **Step 7: Run the build to enumerate the remaining call sites**

Run: `bazel build --config=release //...`
Expected: FAIL, naming every `sleep()` and `now_ms()` caller. Work the list.

- [ ] **Step 8: Convert the sleep call sites**

Mechanical: add `using namespace std::chrono_literals;` per `.cpp` (never a header) and suffix the literal — `ctx.clock.sleep(500, ctx.cancellation)` becomes `ctx.clock.sleep(500ms, ctx.cancellation)`. In `uds_client.cpp:52` the argument is the variable `delay_ms`, which stays an `int` until Task 6; wrap it as `clock_.sleep(std::chrono::milliseconds{delay_ms}, cancellation)`.

- [ ] **Step 9: Rewrite the SSM deadline arithmetic**

This is the payoff site. In `portable_ssm_logging_protocol.cpp`, `readFramedResponse` keeps its `int timeout_ms` signature until Task 7, but the arithmetic becomes typed:

```cpp
    bytes::Bytes received;
    const auto deadline = clock_.now() + std::chrono::milliseconds{timeout_ms};
```

The two loop conditions become `clock_.now() < deadline`, and the trailing remainder:

```cpp
    if (const auto remaining = deadline - clock_.now(); remaining > 0ms)
    {
        if (auto status = read_and_append(std::chrono::duration_cast<std::chrono::milliseconds>(remaining)); !status)
```

Every `static_cast<int>` in this function disappears.

- [ ] **Step 10: Convert the `now_` assertions**

The eight tests asserting on the removed public member become duration assertions:

```cpp
    EXPECT_EQ(clock.elapsed(), 2150ms);
```

with `using namespace std::chrono_literals;` added to each file. `apps/bench/bench_session.cpp` computes a span from two `now()` calls; its `finished - started` is already a duration, so convert the variables to `auto` and `duration_cast<std::chrono::milliseconds>` only where an integral count is actually reported.

- [ ] **Step 11: Verify**

Run: `bazel test --config=release //...`
Expected: PASS. Then `prek run --all-files` and `bazel run //:clang_tidy_report_changed`.

- [ ] **Step 12: Sweep for leftovers**

Run: `grep -rn "now_ms\|\.now_\b" src/ apps/ tests/ | grep -v "\.claude"`
Expected: no hits.

- [ ] **Step 13: Commit and open PR 2**

```bash
git checkout master && git pull && git checkout -b refactor/chrono-clock
git add -A
git commit -m "refactor(ports): chrono types on IClock

sleep() takes std::chrono::milliseconds; now() returns a
steady_clock::time_point, so an instant and a duration are distinct
types. FakeClock exposes elapsed() in place of the public now_ member.
Removes the static_cast<int> deadline arithmetic from the SSM logging
protocol."
git push -u origin refactor/chrono-clock
```

Open the PR with the same body shape as Task 4, citing the spec and the attribution footer.

---

### Task 6: Convert UDS and the flash channels

**Files:**
- Modify: `src/backend/protocol/uds/iuds_channel.h:34`
- Modify: `src/backend/protocol/uds/uds_client.h:19-29`, `uds_client.cpp:41-77`
- Modify: `src/backend/protocol/uds/testing/scripted_uds_channel.h:66-84`
- Modify: `src/backend/flash/flash_executor.h:158,192`
- Modify: `src/backend/flash/can_flash_uds_channel.h:34`, `.cpp`
- Modify: `src/backend/flash/testing/scripted_kline_flash_transport.h:108-112,196` (`pulse_lec_2_line`, `lec_2_pulse_timeouts_`)
- Modify: `src/backend/flash/testing/scripted_can_flash_transport.h` (`read`)
- Modify: `src/platform/desktop/common/transport/desktop_kline_flash_transport.h:45`, `.cpp:137` (`pulse_lec_2_line`)
- Modify: `src/platform/desktop/common/transport/desktop_can_flash_transport.h:39`, `.cpp` (`read`)
- Modify: every `ExchangePolicy` construction site and the executors calling `pulse_lec_2_line(200)` / `receive(2000)` — `subaru_denso_mc68hc16y5_02_executor.cpp:228`, `subaru_denso_sh7055_02_executor.cpp:278`, `subaru_tcu_cvt_hitachi_m32r_can_executor.cpp`, `subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp`, `mitsu_colt_m32r_can_executor.cpp`, `denso_sh705x_eeprom_can_executor.cpp`, `apps/bench/bench_commands.cpp`
- Modify: `src/backend/protocol/uds/uds_client_test.cpp`, `scripted_uds_channel_test.cpp`, `desktop_can_flash_transport_test.cpp`, `desktop_kline_flash_transport_test.cpp`, and the executor tests asserting `lec_2_pulse_timeouts_`

**Interfaces:**
- Consumes: `saturating_ms` (Task 1), the converted `IClock` (Task 5).
- Produces: `IUdsChannel::receive(std::chrono::milliseconds timeout, const ICancellationToken&)`; `ExchangePolicy{pre_read_delay, read_timeout, pending_timeout, max_pending_repeats}`; `IKlineFlashTransport::pulse_lec_2_line(std::chrono::milliseconds)`; `ICanFlashTransport::read(std::chrono::milliseconds, const ICancellationToken&)`.

- [ ] **Step 1: Change the interfaces and the policy**

In `iuds_channel.h`, add `#include <chrono>` and change `receive` to take `std::chrono::milliseconds timeout`. In `flash_executor.h`, change `pulse_lec_2_line(std::chrono::milliseconds timeout)` and `ICanFlashTransport::read(std::chrono::milliseconds timeout, const ICancellationToken&)`.

In `uds_client.h`, `ExchangePolicy` becomes:

```cpp
struct ExchangePolicy
{
    // Quiet period between the write and the first read. Several families
    // need one; zero skips the sleep entirely.
    std::chrono::milliseconds pre_read_delay{0};

    std::chrono::milliseconds read_timeout{500};

    // Read timeout used once the ECU has reported responsePending. Separate
    // from read_timeout because "busy, wait" legitimately takes much longer
    // than a normal reply.
    std::chrono::milliseconds pending_timeout{3000};

    // Guard against an ECU that reports responsePending forever.
    int max_pending_repeats = 10;
};
```

- [ ] **Step 2: Run the build to enumerate the call sites**

Run: `bazel build --config=release //...`
Expected: FAIL. Designated initializers named `.read_timeout_ms` will be flagged as unknown members — that is the intended way to find every construction site.

- [ ] **Step 3: Convert `uds_client.cpp`**

The two locals become durations and the `> 0` test becomes a duration comparison:

```cpp
    auto delay = policy.pre_read_delay;
    auto timeout = policy.read_timeout;

    for (int attempt = 0; attempt <= policy.max_pending_repeats; ++attempt)
    {
        if (delay > 0ms)
        {
            const fastecu::Status slept = clock_.sleep(delay, cancellation);
```

and in the pending branch, `delay = 0ms; timeout = policy.pending_timeout;`. Add `using namespace std::chrono_literals;` to the file's anonymous namespace.

- [ ] **Step 4: Convert the channels and transports**

`CanFlashUdsChannel::receive` forwards its duration to the underlying transport read. `DesktopCanFlashTransport::read` and `DesktopKlineFlashTransport::pulse_lec_2_line` use `saturating_ms<quint16>(timeout)` / `saturating_ms<int>(timeout)` where they reach the serial facade — match the facade parameter's existing type at each site.

In `scripted_uds_channel.h`, `last_timeout_ms_` becomes `last_timeout_` of type `std::chrono::milliseconds` and `timeouts_` becomes `std::vector<std::chrono::milliseconds>`. In `scripted_kline_flash_transport.h`, `lec_2_pulse_timeouts_` becomes `std::vector<std::chrono::milliseconds>`.

- [ ] **Step 5: Convert the construction and call sites**

Every `ExchangePolicy` initializer loses the `_ms` suffix and gains a literal suffix: `{.read_timeout_ms = 3000}` becomes `{.read_timeout = 3000ms}`. Every `pulse_lec_2_line(200)` becomes `pulse_lec_2_line(200ms)`; every `receive(2000, ...)` becomes `receive(2000ms, ...)`.

- [ ] **Step 6: Convert the test assertions**

`EXPECT_EQ(transport.lec_2_pulse_timeouts_, (std::vector<int>{200}));` becomes `(std::vector<std::chrono::milliseconds>{200ms})`. `scripted_uds_channel_test.cpp`'s recorded-timeout assertions become duration comparisons.

- [ ] **Step 7: Verify**

Run: `bazel test --config=release //...`
Expected: PASS. Then `prek run --all-files` and `bazel run //:clang_tidy_report_changed`.

- [ ] **Step 8: Commit and open PR 3**

```bash
git checkout master && git pull && git checkout -b refactor/chrono-uds
git add -A
git commit -m "refactor(uds): chrono timeouts on IUdsChannel and ExchangePolicy

ExchangePolicy's duration fields drop their _ms suffix and take
std::chrono::milliseconds; max_pending_repeats stays an int because it
is a count. ICanFlashTransport::read and pulse_lec_2_line convert with
them."
git push -u origin refactor/chrono-uds
```

---

### Task 7: Convert the logging protocol

**Files:**
- Modify: `src/backend/logging/logging_protocol.h:23`
- Modify: `src/backend/logging/logging_types.h:34-40` (`LoggingPolicy`)
- Modify: `src/backend/logging/protocols/portable_ssm_logging_protocol.h:25,30`, `.cpp`
- Modify: `src/backend/logging/protocols/portable_mut_dma_logging_protocol.h:21`, `.cpp:72`
- Modify: `src/backend/logging/protocols/portable_cdbg_logging_protocol.h:19`, `.cpp:70`
- Modify: `src/backend/logging/logging_session.cpp:355` (validation)
- Modify: `src/backend/logging/testing/scripted_logging_protocol.h`
- Modify: `src/ui/desktop/menu_actions.cpp:555,565,575` (the three `LoggingPolicy` initializers)
- Modify: `src/backend/logging/logging_session_test.cpp`, `logging_conversion_test.cpp`, `logging_use_case_test.cpp`, `src/platform/desktop/common/logging/logging_worker_test.cpp`, `logging_engine_test.cpp`, `logging_adapters_test.cpp`, `src/backend/logging/protocols/ssm_logging_protocol_test.cpp`

**Interfaces:**
- Consumes: the converted transports (Tasks 2–4) and `IClock` (Task 5).
- Produces: `LoggingProtocol::poll(std::chrono::milliseconds timeout, const ICancellationToken&)`; `LoggingPolicy{poll_timeout, car_silence_miss_threshold, reconnect_attempt_threshold, reconnect_retry_period}`.

- [ ] **Step 1: Change the interface and the policy**

In `logging_protocol.h`, add `#include <chrono>` and:

```cpp
    virtual fastecu::Result<PollData> poll(std::chrono::milliseconds timeout, const fastecu::ICancellationToken&) = 0;
```

In `logging_types.h`, add `#include <chrono>` and:

```cpp
struct LoggingPolicy
{
    std::chrono::milliseconds poll_timeout;
    int car_silence_miss_threshold;
    int reconnect_attempt_threshold;
    std::chrono::milliseconds reconnect_retry_period;
};
```

Keep the declaration order the existing designated initializers use, so the aggregate initializers stay valid once renamed — C++ requires designated initializers to appear in declaration order.

- [ ] **Step 2: Run the build to enumerate the call sites**

Run: `bazel build --config=release //...`
Expected: FAIL, naming the three protocols, the scripted fake, `logging_session.cpp`, `menu_actions.cpp`, and the seven test files.

- [ ] **Step 3: Convert the three protocols**

Each `poll` takes `std::chrono::milliseconds timeout` and forwards it directly — the `std::chrono::milliseconds{timeout_ms}` wrappers introduced in Tasks 2 and 4 disappear, since the inbound value is already a duration. In the SSM protocol, `readFramedResponse` takes `std::chrono::milliseconds timeout` and its `deadline` line becomes `clock_.now() + timeout`.

- [ ] **Step 4: Convert the validation**

`logging_session.cpp:355`'s `policy.poll_timeout_ms <= 0` becomes `policy.poll_timeout <= 0ms`, with `using namespace std::chrono_literals;` in the file. The two threshold checks stay integral.

- [ ] **Step 5: Convert the construction sites**

`menu_actions.cpp`'s three initializers become `{.poll_timeout = 50ms, ...}` and `{.poll_timeout = 300ms, ...}`, with `using namespace std::chrono_literals;` at function scope. The seven test files convert the same way.

- [ ] **Step 6: Verify**

Run: `bazel test --config=release //...`
Expected: PASS. Then `prek run --all-files` and `bazel run //:clang_tidy_report_changed`.

- [ ] **Step 7: Final sweep — confirm the hardware-safety claim**

The spec asserts every timeout on the converted paths is a compile-time literal no larger than 3000 ms. Verify rather than assume:

```bash
grep -rnoE "[0-9]{4,}ms" src/backend src/platform/desktop/common/transport apps | grep -v "\.claude" | sort -u
```

Expected: nothing above `3000ms`. Any larger value found is still far below the 65535 ms saturation point, but note it in the PR description if one appears.

- [ ] **Step 8: Confirm no half-typed interface survives**

```bash
grep -rn "int timeout_ms\|int timeoutMs\|_timeout_ms\b" src/backend src/platform/desktop/common/transport | grep -v "\.claude" | grep -v transport_legacy_compat
```

Expected: no hits. `transport_legacy_compat.h` is the one deliberate exception and is excluded above.

- [ ] **Step 9: Commit and open PR 4**

```bash
git checkout master && git pull && git checkout -b refactor/chrono-logging
git add -A
git commit -m "refactor(logging): chrono timeouts on LoggingProtocol and LoggingPolicy

Completes the ports-and-transports cutover. poll() takes a duration and
LoggingPolicy's poll_timeout / reconnect_retry_period drop their _ms
suffixes; the miss and attempt thresholds stay ints because they are
counts."
git push -u origin refactor/chrono-logging
```

---

## Follow-up work, explicitly not in this plan

Recorded in the spec and deliberately deferred:

- The serial surface (`ISerialBackend` / `SerialPortActions` / `serial_port_actions.rep`), which needs a decision about how a duration crosses the QtRO replica boundary.
- The `SerialPortActionsDirect` named timeout constants (`serial_read_short_timeout` and siblings) and their UI duplicates.
- The UI and legacy flash call sites still reaching the ports through `transport_legacy_compat.h`.
