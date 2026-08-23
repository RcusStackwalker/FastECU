# Flash Transport Lifecycle Contract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move flash transport lifetime out of the executors and into a single caller-owned seam, and make the executor/transport pairing a compile-time property instead of a runtime `dynamic_cast`.

**Architecture:** `IFlashExecutor` splits into `IKlineFlashExecutor` and `ICanFlashExecutor`, each declaring its transport type and a pure `transport_setup(plan)`. A `BoundAttempt<Executor, Transport>` template — created only through `bind_flash_attempt`, whose `requires` clause rejects mismatched pairs — owns the configure/open/execute/close sequence and is type-erased behind `BoundFlashAttempt` for `FlashWorker`. Executors keep only protocol I/O.

**Tech Stack:** C++23 (`std::expected`, concepts), Bazel 9.1.1, GoogleTest via `fastecu_portable_gtest`, Qt 6.8.3 for the platform layer only.

**Spec:** [docs/superpowers/specs/2026-08-23-flash-transport-lifecycle-contract-design.md](../specs/2026-08-23-flash-transport-lifecycle-contract-design.md)

## Global Constraints

- **No wire-behavior change.** Every removed prologue or close line must reappear verbatim in `transport_setup()` or `BoundAttempt::run()`. No change to protocol sequences, timing, retry counts, delays, or bytes.
- **Portable closure.** Everything under `src/backend/flash` stays Qt-free, thread-free, filesystem-free. All new types go in the existing `flash_executor` `cc_library`, which is already a `PORTABLE_ROOTS` entry — do **not** create a new portable target (it would require edits to the `genquery` in `BUILD.bazel` and `PORTABLE_ROOTS` in `scripts/check-portable-closure.py`).
- **Exceptions never cross a port.** Use `std::get_if` or guard `std::get` with a preceding `check_family`.
- **`Result<T>` is checked with `.has_value()`**, never the implicit `operator bool`.
- **`#pragma once`** in every header (enforced by prek).
- **Cross-document Markdown references are links with human-readable text**, never backticked paths — lychee runs in prek and cannot see a path written as inline code.
- **PR boundaries:** Tasks 1-2 are PR 1. Tasks 3-16 are PR 2. Task 4 introduces a `bind_flash_attempt` overload taking the legacy `IFlashExecutor`; Task 16 deletes it. That overload is scaffolding internal to PR 2 and **must not survive the PR** — the spec explicitly rejects shipping it as a way to split PR 2 across merges.
- **Verify command for a single target:** `bazel test --config=release //src/backend/flash:<target>`
- **Full gate before either PR:** `bazel test --config=release //...`, `bazel build //:portable_closure`, `prek run --all-files`, `bazel run //:clang_tidy_report_changed`

---

## File Structure

**PR 1 — mechanism (nothing references the new interfaces yet):**

| File | Responsibility |
|---|---|
| `src/backend/flash/flash_executor.h` | Adds `IKlineFlashExecutor`, `ICanFlashExecutor`, `check_family`, `BoundFlashAttempt`, `bind_flash_attempt`. Keeps `IFlashExecutor`, `check_family_transport_match`, `open_can_iso15765_transport` until Task 16. |
| `src/backend/flash/flash_executor.cpp` | `check_family` body. |
| `src/backend/flash/bound_flash_attempt_test.cpp` | The lifecycle guarantees, asserted once for all families. |
| `docs/adr/0015-caller-owns-flash-transport-lifetime.md` | The decision record. |
| `docs/adr/README.md` | Index row. |

**PR 2 — migration:**

| File | Responsibility |
|---|---|
| `src/backend/flash/testing/scripted_*_flash_transport.h` | `start_open()` so a fake can begin configured-and-open. |
| `src/platform/desktop/common/flash/flash_worker.{h,cpp}` | Holds a `BoundFlashAttempt` instead of executor+transport. |
| `src/platform/desktop/common/flash/flash_workflow.{h,cpp}` | `FlashAttempt` carries a bound attempt; six workflow sites call `bind_flash_attempt`. |
| `src/ui/desktop/flash/common/flash_dialog.cpp` | Passes two arguments instead of four. |
| 11 × `*_executor.{h,cpp}` + `*_executor_test.cpp` | One task each: adopt the split interface, add `transport_setup()`, drop lifecycle calls. |

---

### Task 1: Split executor interfaces and record the decision

**Files:**
- Modify: `src/backend/flash/flash_executor.h`
- Modify: `src/backend/flash/flash_executor.cpp:5-18`
- Modify: `src/backend/flash/flash_executor_test.cpp`
- Create: `docs/adr/0015-caller-owns-flash-transport-lifetime.md`
- Modify: `docs/adr/README.md`

**Interfaces:**
- Consumes: existing `FlashPlan`, `KlineConfig`, `Iso15765Config`, `IKlineFlashTransport`, `ICanFlashTransport`.
- Produces: `IKlineFlashExecutor` and `ICanFlashExecutor` (each with `using TransportType`, `using ConfigType`, `transport_setup(const FlashPlan&) const`, and `execute(...)` taking the concrete transport); `Status check_family(const FlashPlan&, FlashFamily)`.

- [ ] **Step 1: Write the failing test**

Append to `src/backend/flash/flash_executor_test.cpp` (the file already defines `kline_read_fields()` at line 14, which builds a `DensoSh705xEepromKline` plan — reuse it):

```cpp
TEST(CheckFamilyTest, MatchingFamilyPasses)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_TRUE(plan.has_value());

    EXPECT_TRUE(check_family(*plan, FlashFamily::DensoSh705xEepromKline).has_value());
}

TEST(CheckFamilyTest, WrongFamilyFailsWithInvalidConfig)
{
    auto plan = validate_and_build(kline_read_fields());
    ASSERT_TRUE(plan.has_value());

    auto status = check_family(*plan, FlashFamily::MitsuColtM32rCan);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::InvalidConfig);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/flash:flash_executor_test`
Expected: compile FAIL — `use of undeclared identifier 'check_family'`.

- [ ] **Step 3: Add the two interfaces and `check_family`**

In `src/backend/flash/flash_executor.h`, after the existing `IFlashExecutor` declaration, add:

```cpp
// The caller owns transport lifetime. An executor never calls configure(),
// open(), or close() on the transport it is given: it receives a transport
// already configured per transport_setup() and open, uses it, and returns.
// Mid-session operations that belong to a protocol sequence -- setBaud(),
// set_add_iso14230_header(), the LEC line calls -- are not lifecycle and stay
// in the executor. See docs/adr/0015-caller-owns-flash-transport-lifetime.md.
class IKlineFlashExecutor
{
  public:
    using TransportType = IKlineFlashTransport;
    using ConfigType = KlineConfig;

    virtual ~IKlineFlashExecutor() = default;

    // Pure: validates `plan` and returns the configuration this executor
    // requires. Performs no I/O, so an invalid plan is rejected before the
    // caller touches hardware.
    virtual Result<KlineConfig> transport_setup(const FlashPlan& plan) const = 0;

    virtual Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport,
                                                 IClock& clock, const ICancellationToken& cancellation,
                                                 IEventSink& events) = 0;
};

// CAN sibling of IKlineFlashExecutor; the same contract applies.
class ICanFlashExecutor
{
  public:
    using TransportType = ICanFlashTransport;
    using ConfigType = Iso15765Config;

    virtual ~ICanFlashExecutor() = default;

    virtual Result<Iso15765Config> transport_setup(const FlashPlan& plan) const = 0;

    virtual Result<FlashExecutionResult> execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                                 const ICancellationToken& cancellation, IEventSink& events) = 0;
};

// Family half of check_family_transport_match. The transport half is
// unreachable for a constructed FlashPlan: validate_and_build already enforces
// family <-> transport-kind <-> variant consistency
// (flash_validation.cpp:24-60, checked at L133). Once this succeeds,
// std::get<PlanT>(plan.family_plan()) cannot throw.
Status check_family(const FlashPlan& plan, FlashFamily expected_family);
```

In `src/backend/flash/flash_executor.cpp`, add above `check_family_transport_match`:

```cpp
Status check_family(const FlashPlan& plan, FlashFamily expected_family)
{
    if (plan.family() != expected_family)
    {
        return fail(ErrorKind::InvalidConfig, "plan family does not match this executor");
    }
    return {};
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/flash:flash_executor_test`
Expected: PASS, including the pre-existing `CheckFamilyTransportMatchTest` and `OpenCanIso15765TransportTest` suites, which stay until Task 16.

- [ ] **Step 5: Write ADR 0015**

Create `docs/adr/0015-caller-owns-flash-transport-lifetime.md`:

```markdown
# 0015. The caller owns flash transport lifetime

Status: accepted (2026-08-23)

## Context

Whether a flash executor closed its transport varied by family with no stated
reason. Six executors closed — the two EEPROM executors, the two M32R K-Line
UDS executors, and the two Denso K-Line executors — and five did not, all of
them CAN UDS. The split tracked which porting wave produced a file, not any
protocol requirement. The step-5c "close exactly once" rule that the closers
resembled was scoped to the EEPROM mode-attempt sequence, never to
`IFlashExecutor` at large.

The divergence was invisible on hardware. Every GUI path builds its adapter
with the non-owning constructor over MainWindow's session-lifetime
`SerialPortActions`, where `close()` resets an already-null `owned_serial_`
and nulls a raw pointer on an adapter destroyed moments later. It mattered
only for the owning path used by `apps/bench` — which already configures and
opens before handing the transport off, i.e. already followed the contract
adopted here.

## Decision

The caller owns transport lifetime. An executor never calls `configure()`,
`open()`, or `close()`. It receives a transport already configured and open,
uses it, and returns. It declares its required setup through a pure
`transport_setup(plan)` that performs no I/O.

Operations that belong to a protocol sequence — `setBaud()`,
`set_add_iso14230_header()`, `disable_lec_lines()`, `pulse_lec_2_line()`,
`enable_programming_voltage_line()` — are not lifecycle and stay in the
executor. The dividing line: a lifecycle operation is one whose correct number
of calls depends on who else is using the transport; a protocol operation is
one whose correct number of calls is fixed by the ECU's state machine.

`IFlashExecutor` splits into `IKlineFlashExecutor` and `ICanFlashExecutor` so
the transport a given executor requires is carried by the signature.
`bind_flash_attempt` is the only way to build the executor/transport pair, and
its `requires` clause rejects a mismatch at compile time; `BoundFlashAttempt`
type-erases the pair only afterward.

## Consequences

Six `dynamic_cast`s and six "transport does not implement …" error strings are
deleted, along with `open_can_iso15765_transport`, `IFlashExecutor`, and
`check_family_transport_match`. A mismatched executor/transport pair becomes
unrepresentable rather than checked.

The step-5c close guarantee — exactly once past `open()`, main error wins over
close error, close-only error returned — moves to `BoundAttempt::run()`, where
it holds for all eleven families instead of the two that asserted it.

The five CAN UDS executors gain a `close()` they never had. This is a no-op in
every current wiring: `FlashWorker` only ever receives non-owning desktop
adapters.

Family <-> plan matching stays a runtime check. Which family a plan is comes
from a ROM chosen at runtime, so no type can carry it; `FlashPlan`'s
`validate_and_build` invariant already makes that check total and makes the
subsequent `std::get` non-throwing.
```

- [ ] **Step 6: Add the ADR index row**

In `docs/adr/README.md`, append to the Index table (leave the "Retired numbers" table and its closing line untouched — 0009-0014 are retired and not reused):

```markdown
| [0015](0015-caller-owns-flash-transport-lifetime.md) | The caller owns flash transport lifetime |
```

Then update the final line of the file from `The next new ADR is 0015.` to `The next new ADR is 0016.`

- [ ] **Step 7: Run prek and commit**

```bash
prek run --all-files
git add src/backend/flash/flash_executor.h src/backend/flash/flash_executor.cpp \
        src/backend/flash/flash_executor_test.cpp docs/adr/
git commit -m "feat(flash): split executor interfaces by transport, add check_family"
```

---

### Task 2: `BoundFlashAttempt` and `bind_flash_attempt`

**Files:**
- Modify: `src/backend/flash/flash_executor.h`
- Create: `src/backend/flash/bound_flash_attempt_test.cpp`
- Modify: `src/backend/flash/BUILD.bazel`

**Interfaces:**
- Consumes: `IKlineFlashExecutor`, `ICanFlashExecutor` (Task 1).
- Produces: `class BoundFlashAttempt` with `run(IClock&, const ICancellationToken&, IEventSink&) -> Result<FlashExecutionResult>` and `request_unblock() noexcept`; `bind_flash_attempt(FlashPlan, std::unique_ptr<Executor>, std::unique_ptr<Transport>) -> std::unique_ptr<BoundFlashAttempt>`.

- [ ] **Step 1: Write the failing test**

Create `src/backend/flash/bound_flash_attempt_test.cpp`. The fake executor records what it saw so the test can prove the transport arrived open and that `execute()` never touched lifecycle:

```cpp
#include "src/backend/flash/flash_executor.h"

#include <gtest/gtest.h>

#include <algorithm>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{
namespace
{

FlashPlanFields kline_read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromKline,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0xf000, .length = 0x1000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff2000, .bytes = {0x01}},
        .family_plan =
            DensoSh705xEepromKlinePlan{
                .mode = EepromReadMode::Mode2,
                .security = DensoSecurityVariant::Stock,
                .tester_id = 0xf0,
                .target_id = 0x10,
                .initial_baud = 4800,
                .kernel_baud = 15625,
            },
        .confirmations =
            {
                ConfirmationSpec{.id = ConfirmationSpec::Id::BeginEepromRead},
                ConfirmationSpec{.id = ConfirmationSpec::Id::InspectEepromBytes},
            },
    };
}

FlashPlan built_plan()
{
    auto plan = validate_and_build(kline_read_fields());
    EXPECT_TRUE(plan.has_value());
    return std::move(*plan);
}

constexpr KlineConfig kSetup{.baud = 4800, .iso14230 = false, .tester_id = 0xf0, .target_id = 0x10};

class FakeKlineExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan&) const override
    {
        if (!setup_ok)
        {
            return fail(ErrorKind::InvalidConfig, "bad plan");
        }
        return kSetup;
    }

    Result<FlashExecutionResult> execute(const FlashPlan&, IKlineFlashTransport& transport, IClock&,
                                         const ICancellationToken&, IEventSink&) override
    {
        ++execute_calls;
        saw_open_transport = transport.isOpen();
        if (!execute_ok)
        {
            return fail(ErrorKind::BadResponse, "execute failed");
        }
        return FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = bytes::Bytes{0x01}};
    }

    bool setup_ok = true;
    bool execute_ok = true;
    int execute_calls = 0;
    bool saw_open_transport = false;
};

struct Harness
{
    ScriptedKlineFlashTransport *transport = nullptr;
    FakeKlineExecutor *executor = nullptr;
    std::unique_ptr<BoundFlashAttempt> attempt;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    Harness()
    {
        auto owned_transport = std::make_unique<ScriptedKlineFlashTransport>();
        auto owned_executor = std::make_unique<FakeKlineExecutor>();
        transport = owned_transport.get();
        executor = owned_executor.get();
        attempt = bind_flash_attempt(built_plan(), std::move(owned_executor), std::move(owned_transport));
    }

    Result<FlashExecutionResult> run()
    {
        return attempt->run(clock, cancellation, events);
    }
};

TEST(BoundFlashAttemptTest, ConfiguresAndOpensBeforeExecuteAndClosesOnce)
{
    Harness h;

    auto result = h.run();

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(h.transport->last_config_.has_value());
    EXPECT_EQ(h.transport->last_config_->baud, kSetup.baud);
    EXPECT_EQ(h.transport->last_config_->tester_id, kSetup.tester_id);
    EXPECT_TRUE(h.executor->saw_open_transport);
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, InvalidPlanTouchesNoTransportCall)
{
    Harness h;
    h.executor->setup_ok = false;

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_FALSE(h.transport->last_config_.has_value());
    EXPECT_EQ(h.transport->close_call_count_, 0);
    EXPECT_EQ(h.executor->execute_calls, 0);
}

TEST(BoundFlashAttemptTest, ConfigureFailureSkipsOpenAndExecuteAndClose)
{
    Harness h;
    h.transport->configure_result_ = fail(ErrorKind::Disconnected, "no port");

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count_, 0);
}

TEST(BoundFlashAttemptTest, OpenFailureSkipsExecuteAndClose)
{
    Harness h;
    h.transport->open_result_ = fail(ErrorKind::Disconnected, "open failed");

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count_, 0);
}

TEST(BoundFlashAttemptTest, CancelledBeforeConfigureDoesNotConfigure)
{
    Harness h;
    h.cancellation.set_cancelled(true);

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_FALSE(h.transport->last_config_.has_value());
}

TEST(BoundFlashAttemptTest, CancelledAfterConfigureDoesNotOpen)
{
    Harness h;
    // Check 1 is the pre-configure check; check 2 is the post-configure one.
    h.cancellation.cancel_on_check(2);

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(h.transport->last_config_.has_value());
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count_, 0);
}

TEST(BoundFlashAttemptTest, ExecuteErrorIsReturnedAndTransportStillClosesOnce)
{
    Harness h;
    h.executor->execute_ok = false;

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, CloseOnlyErrorIsReturned)
{
    Harness h;
    h.transport->close_result_ = fail(ErrorKind::Internal, "close failed");

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorWinsOverCloseErrorAndCloseIsLogged)
{
    Harness h;
    h.executor->execute_ok = false;
    h.transport->close_result_ = fail(ErrorKind::Internal, "close failed");

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(h.transport->close_call_count_, 1);
    const bool warned = std::ranges::any_of(h.events.logs, [](const auto& entry)
                                            { return entry.first == LogLevel::Warning; });
    EXPECT_TRUE(warned);
}

TEST(BoundFlashAttemptTest, RequestUnblockReachesTheTransport)
{
    Harness h;
    h.transport->queueBlockingRead();

    h.attempt->request_unblock();

    // A blocking read released by request_unblock() reports Cancelled rather
    // than hanging; reaching this assertion at all is the guarantee.
    auto read = h.transport->read(10, h.cancellation);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().kind, ErrorKind::Cancelled);
}

} // namespace
} // namespace fastecu::flash
```

- [ ] **Step 2: Add the Bazel target**

In `src/backend/flash/BUILD.bazel`, after the `flash_executor_test` target:

```python
fastecu_portable_gtest(
    name = "bound_flash_attempt_test",
    srcs = ["bound_flash_attempt_test.cpp"],
    deps = [
        ":flash_executor",
        ":flash_validation",
        "//src/backend/flash/testing:scripted_flash_transports",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:recording_event_sink",
    ],
)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/flash:bound_flash_attempt_test`
Expected: compile FAIL — `unknown type name 'BoundFlashAttempt'`.

- [ ] **Step 4: Implement `BoundFlashAttempt` and `bind_flash_attempt`**

Add to `src/backend/flash/flash_executor.h` (needs `#include <concepts>`, `<memory>`, `<utility>`):

```cpp
// An executor already bound to a transport it is known to accept. FlashWorker
// holds this instead of the two halves, so no caller can pair an executor with
// the wrong transport: bind_flash_attempt is the only way to construct one and
// its requires-clause rejects a mismatch at compile time.
class BoundFlashAttempt
{
  public:
    virtual ~BoundFlashAttempt() = default;
    virtual Result<FlashExecutionResult> run(IClock& clock, const ICancellationToken& cancellation,
                                             IEventSink& events) = 0;
    virtual void request_unblock() noexcept = 0;
};

template <class Executor, class Transport> class BoundAttempt final : public BoundFlashAttempt
{
  public:
    BoundAttempt(FlashPlan plan, std::unique_ptr<Executor> executor, std::unique_ptr<Transport> transport)
        : plan_(std::move(plan)), executor_(std::move(executor)), transport_(std::move(transport))
    {
    }

    Result<FlashExecutionResult> run(IClock& clock, const ICancellationToken& cancellation,
                                     IEventSink& events) override
    {
        // Pure: validates the plan and derives config, touching no hardware, so
        // a bad plan is rejected before the adapter is configured or opened.
        Result<typename Executor::ConfigType> setup = executor_->transport_setup(plan_);
        if (!setup.has_value())
        {
            return std::unexpected(setup.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled before configure");
        }
        if (const Status configured = transport_->configure(*setup); !configured.has_value())
        {
            return std::unexpected(configured.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled after configure");
        }
        if (const Status opened = transport_->open(); !opened.has_value())
        {
            return std::unexpected(opened.error());
        }

        Result<FlashExecutionResult> outcome = executor_->execute(plan_, *transport_, clock, cancellation, events);

        // Exactly once on every exit path past open(). Main error wins over a
        // close error; a close-only error is returned. This was step 5c's
        // EEPROM-family rule; here it is the universal one.
        const Status closed = transport_->close();
        if (!outcome.has_value())
        {
            if (!closed.has_value())
            {
                events.log(LogLevel::Warning, "close failed after execution error");
            }
            return outcome;
        }
        if (!closed.has_value())
        {
            return std::unexpected(closed.error());
        }
        return outcome;
    }

    void request_unblock() noexcept override
    {
        transport_->request_unblock();
    }

  private:
    FlashPlan plan_;
    std::unique_ptr<Executor> executor_;
    std::unique_ptr<Transport> transport_;
};

template <class Executor, class Transport>
    requires std::derived_from<Transport, typename Executor::TransportType>
std::unique_ptr<BoundFlashAttempt> bind_flash_attempt(FlashPlan plan, std::unique_ptr<Executor> executor,
                                                      std::unique_ptr<Transport> transport)
{
    return std::make_unique<BoundAttempt<Executor, Transport>>(std::move(plan), std::move(executor),
                                                               std::move(transport));
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/flash:bound_flash_attempt_test`
Expected: PASS, all 10 tests.

- [ ] **Step 6: Confirm the portable closure is intact**

Run: `bazel build //:portable_closure`
Expected: PASS with no BUILD or `scripts/check-portable-closure.py` edits — the new types live in the already-registered `flash_executor` target.

- [ ] **Step 7: Commit — this completes PR 1**

```bash
git add src/backend/flash/flash_executor.h src/backend/flash/BUILD.bazel \
        src/backend/flash/bound_flash_attempt_test.cpp
git commit -m "feat(flash): add BoundFlashAttempt and compile-checked bind_flash_attempt"
```

---

### Task 3: Let the scripted fakes start configured-and-open

**Files:**
- Modify: `src/backend/flash/testing/scripted_kline_flash_transport.h`
- Modify: `src/backend/flash/testing/scripted_can_flash_transport.h`
- Modify: `src/backend/flash/bound_flash_attempt_test.cpp`

**Interfaces:**
- Produces: `void start_open()` on both scripted transports — marks the fake open without counting an `open()` call, for tests that call an executor directly now that executors no longer open.

- [ ] **Step 1: Write the failing test**

Append to `src/backend/flash/bound_flash_attempt_test.cpp`, inside the anonymous namespace:

```cpp
TEST(ScriptedKlineFlashTransportTest, StartOpenReportsOpenWithoutAnOpenCall)
{
    ScriptedKlineFlashTransport transport;
    EXPECT_FALSE(transport.isOpen());

    transport.start_open();

    EXPECT_TRUE(transport.isOpen());
    EXPECT_EQ(transport.close_call_count_, 0);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bazel test --config=release //src/backend/flash:bound_flash_attempt_test`
Expected: compile FAIL — `no member named 'start_open'`.

- [ ] **Step 3: Add `start_open()` to both fakes**

In `src/backend/flash/testing/scripted_kline_flash_transport.h`, next to the other script-setup helpers (after `queueBlockingRead()`):

```cpp
    // For tests that drive an executor directly. Executors no longer open
    // their transport (ADR 0015), so the fake must start in the state a
    // BoundAttempt would have left it in.
    void start_open()
    {
        open_ = true;
    }
```

Add the identical member to `src/backend/flash/testing/scripted_can_flash_transport.h` after its `queueBlockingRead()`. `ICanFlashTransport` has no `isOpen()`, so `open_` is unread there today; the member exists for symmetry and for the read/write guards to stay meaningful if one is added.

- [ ] **Step 4: Run the test to verify it passes**

Run: `bazel test --config=release //src/backend/flash:bound_flash_attempt_test`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/backend/flash/testing/ src/backend/flash/bound_flash_attempt_test.cpp
git commit -m "test(flash): let scripted flash transports start open"
```

---

### Task 4: Move `FlashWorker` onto `BoundFlashAttempt`

Introduces the temporary legacy bind overload so every later task keeps the build green. Task 16 deletes it; it must not leave this PR.

**Files:**
- Modify: `src/backend/flash/flash_executor.h`
- Modify: `src/platform/desktop/common/flash/flash_worker.h`
- Modify: `src/platform/desktop/common/flash/flash_worker.cpp:20-27,36-40,65`
- Modify: `src/platform/desktop/common/flash/flash_worker_test.cpp`
- Modify: `src/platform/desktop/common/flash/flash_workflow.h:66-72`
- Modify: `src/platform/desktop/common/flash/flash_workflow.cpp` (seven transport constructions across six workflow sites: 169-173, 277, 374, 463, 551, 645-659)
- Modify: `src/ui/desktop/flash/common/flash_dialog.cpp:90-91`

**Interfaces:**
- Consumes: `bind_flash_attempt` (Task 2).
- Produces: `FlashAttempt{std::unique_ptr<BoundFlashAttempt> attempt; std::unique_ptr<IClock> clock;}`; `FlashWorker(FlashAttempt, QObject*)`; `bind_legacy_flash_attempt(FlashPlan, std::unique_ptr<IFlashExecutor>, std::unique_ptr<IFlashTransport>)` — **temporary**.

- [ ] **Step 1: Add the temporary legacy bind**

In `src/backend/flash/flash_executor.h`, after `bind_flash_attempt`:

```cpp
// TEMPORARY migration scaffolding (ADR 0015). Wraps an unmigrated
// IFlashExecutor, which still configures/opens/closes for itself, so families
// can move to the split interfaces one at a time with a green build in
// between. Deleted together with IFlashExecutor in the final migration commit;
// it must not outlive the migration PR.
std::unique_ptr<BoundFlashAttempt> bind_legacy_flash_attempt(FlashPlan plan, std::unique_ptr<IFlashExecutor> executor,
                                                             std::unique_ptr<IFlashTransport> transport);
```

In `src/backend/flash/flash_executor.cpp`:

```cpp
namespace
{

class LegacyBoundAttempt final : public BoundFlashAttempt
{
  public:
    LegacyBoundAttempt(FlashPlan plan, std::unique_ptr<IFlashExecutor> executor,
                       std::unique_ptr<IFlashTransport> transport)
        : plan_(std::move(plan)), executor_(std::move(executor)), transport_(std::move(transport))
    {
    }

    Result<FlashExecutionResult> run(IClock& clock, const ICancellationToken& cancellation,
                                     IEventSink& events) override
    {
        // No setup and no close: an unmigrated executor still does its own.
        return executor_->execute(plan_, *transport_, clock, cancellation, events);
    }

    void request_unblock() noexcept override
    {
        transport_->request_unblock();
    }

  private:
    FlashPlan plan_;
    std::unique_ptr<IFlashExecutor> executor_;
    std::unique_ptr<IFlashTransport> transport_;
};

} // namespace

std::unique_ptr<BoundFlashAttempt> bind_legacy_flash_attempt(FlashPlan plan, std::unique_ptr<IFlashExecutor> executor,
                                                             std::unique_ptr<IFlashTransport> transport)
{
    return std::make_unique<LegacyBoundAttempt>(std::move(plan), std::move(executor), std::move(transport));
}
```

- [ ] **Step 2: Write the failing test**

In `src/platform/desktop/common/flash/flash_worker_test.cpp`, replace the fake executor (line 45) with a fake bound attempt and update the worker construction:

```cpp
class FakeBoundAttempt final : public fastecu::flash::BoundFlashAttempt
{
  public:
    fastecu::Result<fastecu::flash::FlashExecutionResult> run(fastecu::IClock&, const fastecu::ICancellationToken&,
                                                              fastecu::IEventSink&) override
    {
        ++run_calls;
        return fastecu::flash::FlashExecutionResult{.operation = fastecu::flash::FlashOperation::Read,
                                                    .read_bytes = fastecu::bytes::Bytes{0x01}};
    }

    void request_unblock() noexcept override
    {
        ++unblock_calls;
    }

    int run_calls = 0;
    int unblock_calls = 0;
};
```

Keep every existing assertion about emitted signals and teardown; only the construction changes to pass a `FlashAttempt`.

- [ ] **Step 3: Run the test to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/flash:all`
Expected: compile FAIL — `FlashWorker` still takes four constructor arguments.

- [ ] **Step 4: Change `FlashAttempt`, `FlashWorker`, and the call sites**

In `src/platform/desktop/common/flash/flash_workflow.h:66-72`:

```cpp
struct FlashAttempt
{
    std::unique_ptr<BoundFlashAttempt> attempt;
    std::unique_ptr<IClock> clock;
};
```

In `flash_worker.h`, replace the `executor_`/`transport_`/`plan_` members with `std::unique_ptr<BoundFlashAttempt> attempt_;` and `std::unique_ptr<IClock> clock_;`, and change the constructor to `FlashWorker(FlashAttempt attempt, QObject *parent = nullptr)`.

In `flash_worker.cpp`, line 36-40 becomes:

```cpp
void FlashWorker::requestStop()
{
    cancellation_.cancel();
    attempt_->request_unblock();
}
```

and line 65 becomes:

```cpp
    Result<FlashExecutionResult> result = attempt_->run(*clock_, cancellation_, events);
```

Leave `kTeardownWaitMs`, the destructor, and the `Qt::DirectConnection` wiring with its thread-affinity comment exactly as they are.

In `flash_workflow.cpp`, wrap all 7 construction sites with `bind_legacy_flash_attempt`. For example, lines 169-173 become:

```cpp
            std::unique_ptr<IFlashExecutor> executor =
                hitachi_ ? std::unique_ptr<IFlashExecutor>(std::make_unique<SubaruHitachiM32rKlineExecutor>())
                         : std::unique_ptr<IFlashExecutor>(std::make_unique<SubaruMitsuM32rKlineExecutor>());
            return FlashAttempt{bind_legacy_flash_attempt(std::move(*plan_), std::move(executor),
                                                          std::make_unique<DesktopKlineFlashTransport>(
                                                              request_.serial)),
                                std::make_unique<QtClock>()};
```

Apply the same wrapping at lines 277, 374, 463, 551, and 645-659. The
`EepromWorkflow` site (645-659) builds `executor` and `adapter` as base-typed
`unique_ptr`s across an if/else and joins them at one `FlashAttempt`; wrap that
single join. The site at 551 lives inside the
`SimpleCanFlashWorkflow<ExecutorT, BuildPlan>` template (declared at line 517),
so wrapping it once covers all four of its instantiations (lines 598-604).

In `src/ui/desktop/flash/common/flash_dialog.cpp:90-91`:

```cpp
    worker_ = std::make_unique<FlashWorker>(std::move(attempt));
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bazel test --config=release //src/platform/desktop/common/flash:all //src/ui/desktop/flash/...`
Expected: PASS. Behavior is unchanged — the legacy wrapper calls `execute()` exactly as `FlashWorker` did.

- [ ] **Step 6: Commit**

```bash
git add src/backend/flash/flash_executor.h src/backend/flash/flash_executor.cpp \
        src/platform/desktop/common/flash/ src/ui/desktop/flash/common/flash_dialog.cpp
git commit -m "refactor(flash): FlashWorker holds a BoundFlashAttempt"
```

---

### Tasks 5-15: Migrate one executor per task

Each task is the same four-part transformation applied to one family. **Run every task's own test target before committing** — these are hardware paths, and the per-file check that no protocol line moved is the safety net.

**The transformation, in full:**

1. **Header** — change the base class from `IFlashExecutor` to `IKlineFlashExecutor` or `ICanFlashExecutor`, and add the `transport_setup` declaration:
   ```cpp
   Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;   // or Iso15765Config
   ```
   Change `execute()`'s second parameter from `IFlashTransport&` to `IKlineFlashTransport&` / `ICanFlashTransport&`.
2. **`transport_setup()`** — new function containing, in order: `check_family(plan, FlashFamily::X)`, the family's `validate_*_plan(plan)` call if it has one, `std::get<XPlan>(plan.family_plan())`, and a `return` of the config the old prologue built. Copy the config expression verbatim.
3. **`execute()`** — delete the `dynamic_cast` and its null check and error string; delete the `configure()`/`open()` pair or the `open_can_iso15765_transport()` call; delete any close block. Replace `check_family_transport_match(...)` with `check_family(plan, FlashFamily::X)`. Rename the local transport variable to the parameter, or bind a reference, so the body below is untouched.
4. **Test** — call `transport.start_open()` before `execute()`; replace configure/open assertions with `transport_setup()` assertions; delete open/close failure-injection cases (now covered by `bound_flash_attempt_test`); construct the executor through its concrete type.
5. **Workflow** — switch the family's `flash_workflow.cpp` site from
   `bind_legacy_flash_attempt` to `bind_flash_attempt`, **but only once every
   family sharing that site is migrated.** A site that serves several families
   cannot produce both shapes at once: the site at 169-173 builds its executor
   from a ternary whose two arms must unify to one type, the site at 551 is a
   template with four instantiations, and the site at 645-659 joins an if/else
   at a single `FlashAttempt`. Migrating one family of such a group leaves its
   site on `bind_legacy_flash_attempt` until the last of the group lands.

**The six CAN families are uniform.** Each replaces its `open_can_iso15765_transport(transport, Iso15765Config{...})` call with a `transport_setup()` returning exactly that config, then uses the `ICanFlashTransport&` parameter directly:

```cpp
Result<Iso15765Config> XExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::X); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_x_plan(plan); !valid.has_value())   // omit where the family has none
    {
        return std::unexpected(valid.error());
    }
    const auto& family = std::get<XPlan>(plan.family_plan());
    return Iso15765Config{
        .bitrate = family.bitrate,
        .request_id = family.request_id,
        .response_id = family.response_id,
        .extended_id = family.extended_id,
    };
}
```

| Task | Executor | Family enum | Plan type | Validator | Prologue at | Close at |
|---|---|---|---|---|---|---|
| 5 | `MitsuColtM32rCanExecutor` | `MitsuColtM32rCan` | `MitsuColtM32rCanPlan` | `validate_mitsu_colt_m32r_can_plan` | `mitsu_colt_m32r_can_executor.cpp:670-694` | — (also delete the "Legacy never closes the port" comment at 680-682, replacing it with a pointer to ADR 0015) |
| 6 | `SubaruHitachiM32rCanExecutor` | `SubaruHitachiM32rCan` | `SubaruHitachiM32rCanPlan` | `validate_subaru_hitachi_m32r_can_plan` | `subaru_hitachi_m32r_can_executor.cpp:494-502` | — |
| 7 | `SubaruTcuCvtHitachiM32rCanExecutor` | `SubaruTcuCvtHitachiM32rCan` | `SubaruTcuCvtHitachiM32rCanPlan` | `validate_subaru_tcu_cvt_hitachi_m32r_can_plan` | `subaru_tcu_cvt_hitachi_m32r_can_executor.cpp:569-577` | — |
| 8 | `SubaruTcuCvtMitsuMh8104CanExecutor` | `SubaruTcuCvtMitsuMh8104Can` | `SubaruTcuCvtMitsuMh8104CanPlan` | `validate_subaru_tcu_cvt_mitsu_mh8104_can_plan` | `subaru_tcu_cvt_mitsu_mh8104_can_executor.cpp:672-680` | — |
| 9 | `SubaruTcuCvtMitsuMh8111CanExecutor` | `SubaruTcuCvtMitsuMh8111Can` | `SubaruTcuCvtMitsuMh8111CanPlan` | `validate_subaru_tcu_cvt_mitsu_mh8111_can_plan` | `subaru_tcu_cvt_mitsu_mh8111_can_executor.cpp:566-574` | — |
| 10 | `DensoSh705xEepromCanExecutor` | `DensoSh705xEepromCan` | `DensoSh705xEepromCanPlan` | none — validation is inline | `denso_sh705x_eeprom_can_executor.cpp:345-369` | `ScopedClose` at 370-382 plus every early `can_transport.close(); scoped_close.done = true;` block |

**The five K-Line families differ in their config expression.** Each returns the `KlineConfig` its old prologue built:

| Task | Executor | Family enum | Plan type | `transport_setup()` returns | Prologue at | Close at |
|---|---|---|---|---|---|---|
| 11 | `SubaruHitachiM32rKlineExecutor` | `SubaruHitachiM32rKline` | `SubaruHitachiM32rKlinePlan` | `KlineConfig{p.initial_baud, false, p.tester_id, p.target_id}` | `subaru_hitachi_m32r_kline_executor.cpp:434-460` | 494-502 |
| 12 | `SubaruMitsuM32rKlineExecutor` | `SubaruMitsuM32rKline` | `SubaruMitsuM32rKlinePlan` | `KlineConfig{p.initial_baud, false, p.tester_id, p.target_id}` | `subaru_mitsu_m32r_kline_executor.cpp:336-362` | 364-372 |
| 13 | `SubaruDensoSh7055_02Executor` | `SubaruDensoSh7055_02` | `SubaruDensoSh7055_02Plan` | `KlineConfig{.baud = 62500, .iso14230 = false, .tester_id = family_plan.tester_id, .target_id = family_plan.target_id}` — the literal 62500 is deliberate, not a plan field | `subaru_denso_sh7055_02_executor.cpp:885-931` | 1006-1018 |
| 14 | `SubaruDensoMc68hc16y5_02Executor` | `SubaruDensoMc68hc16y5_02` | `SubaruDensoMc68hc16y5_02Plan` | `KlineConfig{.baud = family_plan.connect_baud, .iso14230 = false, .tester_id = 0, .target_id = 0}` | `subaru_denso_mc68hc16y5_02_executor.cpp:805-845` | 915-928 |
| 15 | `DensoSh705xEepromKlineExecutor` | `DensoSh705xEepromKline` | `DensoSh705xEepromKlinePlan` | `KlineConfig{.baud = kline_plan.initial_baud, .iso14230 = false, .tester_id = kline_plan.tester_id, .target_id = kline_plan.target_id}` | `denso_sh705x_eeprom_kline_executor.cpp:294-328` | `ScopedClose` at 331-343 plus every early `kline_transport.close(); scoped_close.done = true;` block |

**Two details that are easy to get wrong:**

- **Tasks 13 and 14 have cancellation checks inside the prologue.** `sh7055_02` checks before configure, after configure, and again before open; `mc68hc16y5_02` checks after configure. These are already covered by `BoundAttempt::run()`'s two checks — delete them with the prologue rather than moving them into `transport_setup()`, which must stay pure and does not receive a cancellation token.
- **Task 15 keeps `set_add_iso14230_header(false)`.** It is a protocol operation, not lifecycle. It stays as the first statement of `execute()`, and its comment about a `true` leaking from an earlier attempt stays with it. Its error path currently also closes; that close goes away, and the function just returns the error.

**Per-task step sequence (apply to each of Tasks 5-15):**

- [ ] **Step 1:** Update the executor's `_test.cpp` — add `transport.start_open()` before each `execute()` call, and add a `transport_setup()` assertion replacing the old configure assertion, e.g.:

```cpp
TEST(XExecutorTest, TransportSetupReturnsThePlansWireParameters)
{
    XExecutor executor;
    auto plan = built_plan();   // the file's existing plan helper

    auto setup = executor.transport_setup(plan);

    ASSERT_TRUE(setup.has_value());
    EXPECT_EQ(setup->bitrate, 500000);
    EXPECT_EQ(setup->request_id, 0x7E0u);
    EXPECT_EQ(setup->response_id, 0x7E8u);
}
```

Assert the values that file's own plan-building helper sets — open it and read
them, do not carry over the numbers above. They are the values the old prologue
passed to `configure()`, so the pre-change test's configure assertion (if the
file has one) is where to find them; where it has none, the plan helper is the
source. For a K-Line family the fields are `baud`, `iso14230`, `tester_id`, and
`target_id` instead.

- [ ] **Step 2:** Run the test to verify it fails. Run: `bazel test --config=release //src/backend/flash/ecu:<target>` (or `//src/backend/flash/eeprom:<target>`). Expected: compile FAIL — `no member named 'transport_setup'`.
- [ ] **Step 3:** Apply parts 1-3 of the transformation to the header and the `.cpp`.
- [ ] **Step 4:** Delete the now-dead open/close failure-injection tests from the executor's test file (`set_open_result`, `set_close_result`, and `close_call_count_` assertions). They are covered by `bound_flash_attempt_test`.
- [ ] **Step 5:** Switch the family's `flash_workflow.cpp` site to
  `bind_flash_attempt` **if this task is the last of its site group** (see the
  table below); otherwise leave the site on `bind_legacy_flash_attempt` and note
  in the commit message which task will flip it.

| Workflow site | Serves | Flip it in |
|---|---|---|
| `ColtWorkflow`, line 463 | Task 5 | Task 5 |
| `SimpleCanFlashWorkflow` template, line 551 (instantiated at 598-604) | Tasks 6, 7, 8, 9 | Task 9 |
| `EepromWorkflow`, lines 645-659 | Tasks 10, 15 | Task 15 |
| `SubaruM32rKlineWorkflow`, lines 169-173 | Tasks 11, 12 | Task 12 |
| `SubaruDensoSh7055_02Workflow`, line 374 | Task 13 | Task 13 |
| `SubaruDensoMc68hc16y5_02Workflow`, line 277 | Task 14 | Task 14 |

  The `EepromWorkflow` site needs both branches to call `bind_flash_attempt`
  separately, since the K-Line and CAN arms now have different executor and
  transport types and can no longer be joined as base-typed `unique_ptr`s before
  the `FlashAttempt`.
- [ ] **Step 6:** Run the test to verify it passes. Run: `bazel test --config=release //src/backend/flash/ecu:<target> //src/platform/desktop/common/flash:all`. Expected: PASS.
- [ ] **Step 7:** Verify no protocol line moved:

```bash
git diff -U0 -- src/backend/flash/**/<family>_executor.cpp | grep '^-' | grep -v 'configure\|->open()\|\.open()\|close\|dynamic_cast\|does not implement\|open_can_iso15765_transport\|check_family_transport_match\|ScopedClose\|scoped_close\|cancelled'
```

Expected: only the lines that reappear in `transport_setup()`. Anything else in the output is a protocol line you deleted by mistake — restore it.

- [ ] **Step 8:** Commit.

```bash
git add src/backend/flash/ src/platform/desktop/common/flash/flash_workflow.cpp
git commit -m "refactor(flash): <family> owns no transport lifecycle"
```

---

### Task 16: Delete the old interface and close out the docs

**Files:**
- Modify: `src/backend/flash/flash_executor.h`
- Modify: `src/backend/flash/flash_executor.cpp`
- Modify: `src/backend/flash/flash_executor_test.cpp`
- Modify: `docs/superpowers/specs/2026-07-22-step5c-flash-preflight-execution-seam-design.md`

**Interfaces:**
- Consumes: all eleven executors migrated (Tasks 5-15).
- Produces: nothing new. Removes `IFlashExecutor`, `bind_legacy_flash_attempt`, `LegacyBoundAttempt`, `open_can_iso15765_transport`, `check_family_transport_match`.

- [ ] **Step 1: Confirm nothing references the old symbols**

```bash
grep -rn "IFlashExecutor\|bind_legacy_flash_attempt\|open_can_iso15765_transport\|check_family_transport_match" \
  src apps tests --include="*.cpp" --include="*.h"
```

Expected: only the declarations/definitions in `flash_executor.{h,cpp}` and the tests in `flash_executor_test.cpp`. If a `flash_workflow.cpp` site still appears, a task in 5-15 was skipped — finish it first.

- [ ] **Step 2: Delete the old declarations**

From `src/backend/flash/flash_executor.h`, delete `class IFlashExecutor`, `check_family_transport_match`, `open_can_iso15765_transport`, and `bind_legacy_flash_attempt` — including the comment at lines 117-119 that describes the close divergence ("Callers keep owning close()/lifecycle -- some never close … one closes on every exit path"), which no longer describes anything true.

From `src/backend/flash/flash_executor.cpp`, delete `check_family_transport_match`, `open_can_iso15765_transport`, `LegacyBoundAttempt`, and `bind_legacy_flash_attempt`.

Keep `IFlashTransport` — both `IKlineFlashTransport` and `ICanFlashTransport` still derive from it.

- [ ] **Step 3: Delete the obsolete tests**

From `src/backend/flash/flash_executor_test.cpp`, delete the three `CheckFamilyTransportMatchTest` tests and the four `OpenCanIso15765TransportTest` tests. Keep the two `CheckFamilyTest` tests from Task 1 and the `kline_read_fields()` helper they use.

- [ ] **Step 4: Run the full suite**

Run: `bazel test --config=release //...`
Expected: PASS.

- [ ] **Step 5: Point the step-5c spec at the promoted rule**

In `docs/superpowers/specs/2026-07-22-step5c-flash-preflight-execution-seam-design.md`, after the cleanup paragraph at L526-529, add:

```markdown
This close rule was scoped to the EEPROM mode attempt. It was later promoted to
the universal flash executor contract — see
[the flash transport lifecycle contract design](2026-08-23-flash-transport-lifecycle-contract-design.md)
and ADR 0015; the guarantee now lives in `BoundAttempt::run()` and holds for
every family.
```

- [ ] **Step 6: Run the full gate**

```bash
bazel test --config=release //...
bazel build //:portable_closure
prek run --all-files
bazel run //:clang_tidy_report_changed
```

Expected: all PASS. `prek` includes lychee, which checks the new cross-document links.

- [ ] **Step 7: Commit — this completes PR 2**

```bash
git add src/backend/flash/ docs/superpowers/specs/
git commit -m "refactor(flash): delete IFlashExecutor and the transport downcast helpers"
```

- [ ] **Step 8: Close issue #208**

When both PRs are merged, close [issue #208](https://github.com/RcusStackwalker/FastECU/issues/208) with a comment noting the two things the investigation got wrong: the inventory was six closers rather than two (`subaru_denso_sh7055_02_executor.cpp` and `subaru_denso_mc68hc16y5_02_executor.cpp` were missed), and `close()` was never wire-visible in the GUI, where every adapter is non-owning.

**Not done, deliberately:** no entry is added to the [tech-debt roadmap](../../tech-debt.md) — the issue proposed one on the assumption the divergence would be documented rather than removed. No rows change in the [flash qualification matrix](../../flash-qualification-matrix.md): `portable` stays `yes` for all eleven families and every test-label name is unchanged.
