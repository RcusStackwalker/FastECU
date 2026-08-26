# Desktop Quick Configurable CDBG Session Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare a dashboard document as one validated logging-session/CDBG-config pair, drive CDBG with explicit wire settings, and schedule reconnects by monotonic elapsed time.

**Architecture:** Add a portable `CdbgProtocolConfig` beside the CDBG driver, then make the driver and logging protocol consume it explicitly. Change generic logging retry policy to clock-based deadlines with an optional attempt limit, adapt the Widgets boundary without changing its effective behavior, and finally add a Qt-free dashboard builder that returns one move-only `PreparedDashboardSession`.

**Tech Stack:** C++23, Bazel, GoogleTest, Qt 6.8.3/QtTest, existing `Result`, `IClock`, `FakeClock`, scripted protocol and CAN transport fakes.

**Spec:** `docs/superpowers/specs/2026-08-26-desktop-quick-cdbg-session-foundation-design.md`

## Global Constraints

- Keep adapter discovery, raw-CAN setup, transport construction, hardware opening, and QtQuick presentation out of scope.
- Keep bitrate, selected identifier width, and preferred adapter outside `CdbgProtocolConfig` and `PreparedDashboardSession`.
- Preserve the Widgets application's effective reconnect schedule, unlimited retries, and default Colt request/reply IDs, stream instance `0`, and sampling interval `10` ms.
- Accept sampling intervals `1..65535` ms, or exact multiples of `10` in `65540..655350` ms; never round or truncate.
- Dashboard sessions use `reconnect-period-ms` for both initial delay and repeat spacing, and use `reconnect-attempts` as a finite budget.
- Only a valid sample resets reconnect state and attempt budget.
- All backend additions remain portable and covered by `//:portable_closure`.
- Preserve the user's unrelated working-tree changes in dashboard codec files and untracked artifacts.

---

## File Structure

- `src/backend/protocol/cdbg_protocol_config.{h,cpp}`: validated CDBG wire settings and Colt-default construction.
- `src/backend/protocol/cdbg_protocol_config_test.cpp`: range and exact-interval contract.
- `src/backend/protocol/mitsu_colt_can_cdbg_driver.{h,cpp}`: consumes configuration for all request/reply traffic and stream commands.
- `src/backend/logging/logging_types.h`: generic elapsed-time retry policy.
- `src/backend/logging/logging_session.cpp`: validates the new policy.
- `src/backend/logging/logging_use_case.{h,cpp}`: monotonic reconnect state machine.
- `src/platform/desktop/common/logging/logging_snapshot_adapter.{h,cpp}`: legacy count-to-time compatibility conversion.
- `src/platform/desktop/common/logging/logging_worker.{h,cpp}` and `logging_engine.{h,cpp}`: inject and own the desktop monotonic clock.
- `src/backend/dashboard/dashboard_session_builder.{h,cpp}`: document resolution and combined prepared-session boundary.
- Existing focused tests and `BUILD.bazel` files: compile and verify each contract without broad target merging.

---

### Task 1: Add validated CDBG wire configuration

**Files:**
- Create: `src/backend/protocol/cdbg_protocol_config.h`
- Create: `src/backend/protocol/cdbg_protocol_config.cpp`
- Create: `src/backend/protocol/cdbg_protocol_config_test.cpp`
- Modify: `src/backend/protocol/BUILD.bazel`
- Modify: `src/backend/dashboard/dashboard_validation.cpp`
- Modify: `src/backend/dashboard/dashboard_validation_test.cpp`

**Interfaces:**
- Consumes: Colt constants `MitsuColtCanCdbg::kRequestCanId` and `kReplyCanId`; `Result`/`ErrorKind::InvalidConfig`.
- Produces: `cdbg::CdbgProtocolConfig`; `Result<CdbgProtocolConfig> make_cdbg_protocol_config(std::uint32_t request_id, std::uint32_t reply_id, std::uint8_t stream_instance, std::uint32_t sampling_interval_ms)`; `Result<CdbgProtocolConfig> make_colt_cdbg_protocol_config()`; `bool valid_cdbg_sampling_interval(std::uint32_t)`.

- [ ] **Step 1: Write failing protocol-config tests**

Create table-driven tests that assert getters preserve `0x620`, `0x621`, instance `7`, and interval `25`; Colt defaults are `0x630`, `0x631`, `0`, and `10`; equal IDs and `0x20000000` fail; and interval cases have these results:

```cpp
struct IntervalCase { std::uint32_t value; bool valid; };
const IntervalCase cases[] = {
    {0, false}, {1, true}, {65535, true}, {65536, false},
    {65540, true}, {65541, false}, {655350, true}, {655360, false},
};
for (const auto& test : cases) {
    auto result = cdbg::make_cdbg_protocol_config(0x620, 0x621, 7, test.value);
    EXPECT_EQ(result.has_value(), test.valid) << test.value;
    if (!test.valid) EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}
```

- [ ] **Step 2: Run the new test and verify the target is absent**

Run: `bazel test //src/backend/protocol:cdbg_protocol_config_test --test_output=errors`

Expected: FAIL because the target and configuration type do not exist.

- [ ] **Step 3: Implement the validating value type**

Declare an immutable, privately constructed class with these getters:

```cpp
namespace cdbg {
bool valid_cdbg_sampling_interval(std::uint32_t interval_ms);

class CdbgProtocolConfig {
  public:
    std::uint32_t request_id() const;
    std::uint32_t reply_id() const;
    std::uint8_t stream_instance() const;
    std::uint32_t sampling_interval_ms() const;
  private:
    CdbgProtocolConfig(std::uint32_t, std::uint32_t, std::uint8_t, std::uint32_t);
    std::uint32_t request_id_;
    std::uint32_t reply_id_;
    std::uint8_t stream_instance_;
    std::uint32_t sampling_interval_ms_;
    friend fastecu::Result<CdbgProtocolConfig> make_cdbg_protocol_config(
        std::uint32_t, std::uint32_t, std::uint8_t, std::uint32_t);
};

fastecu::Result<CdbgProtocolConfig> make_cdbg_protocol_config(
    std::uint32_t request_id, std::uint32_t reply_id,
    std::uint8_t stream_instance, std::uint32_t sampling_interval_ms);
fastecu::Result<CdbgProtocolConfig> make_colt_cdbg_protocol_config();
} // namespace cdbg
```

Implement interval validation as:

```cpp
return interval_ms >= 1 &&
       (interval_ms <= 65535 ||
        (interval_ms <= 655350 && interval_ms % 10 == 0));
```

Reject IDs above `0x1fffffff`, equal IDs, and invalid intervals with stable details naming `request-id`, `reply-id`, or `sampling-interval-ms`. Construct Colt defaults through the same validating factory and return its `Result` rather than bypassing validation.

Add `cdbg_protocol_config` and `cdbg_protocol_config_test` Bazel targets. Keep the library dependent only on the Colt algorithm target and `//src/backend/ports`.

- [ ] **Step 4: Make dashboard validation enforce exact interval encoding**

Replace the positive-only sampling check with `cdbg::valid_cdbg_sampling_interval(...)`, retaining the path `connection.sampling-interval-ms`. Add the exact boundary table above to `dashboard_validation_test.cpp`. Add `//src/backend/protocol:cdbg_protocol_config` to `dashboard_validation` deps.

- [ ] **Step 5: Run focused tests**

Run: `bazel test //src/backend/protocol:cdbg_protocol_config_test //src/backend/dashboard:dashboard_validation_test --test_output=errors`

Expected: PASS.

- [ ] **Step 6: Commit the configuration contract**

```bash
git add src/backend/protocol/cdbg_protocol_config.h src/backend/protocol/cdbg_protocol_config.cpp src/backend/protocol/cdbg_protocol_config_test.cpp src/backend/protocol/BUILD.bazel src/backend/dashboard/dashboard_validation.cpp src/backend/dashboard/dashboard_validation_test.cpp src/backend/dashboard/BUILD.bazel
git commit -m "feat: validate configurable CDBG protocol settings"
```

---

### Task 2: Drive CDBG traffic from explicit configuration

**Files:**
- Modify: `src/backend/protocol/mitsu_colt_can_cdbg_driver.h`
- Modify: `src/backend/protocol/mitsu_colt_can_cdbg_driver.cpp`
- Modify: `src/backend/protocol/ican_transport.h`
- Modify: `src/backend/protocol/cdbg_driver_test.cpp`
- Modify: `src/backend/logging/protocols/portable_cdbg_logging_protocol.h`
- Modify: `src/backend/logging/protocols/portable_cdbg_logging_protocol.cpp`
- Modify: `src/backend/logging/protocols/cdbg_logging_protocol_test.cpp`
- Modify: `src/backend/logging/protocols/BUILD.bazel`
- Modify: `src/platform/desktop/common/logging/legacy_logging_protocol_factory.cpp`
- Modify: `src/platform/desktop/common/logging/legacy_logging_protocol_factory_test.cpp`

**Interfaces:**
- Consumes: `cdbg::CdbgProtocolConfig` from Task 1.
- Produces: `CdbgLogDriver(ICanTransport&, CdbgProtocolConfig)`; `startFreeFormLog(channels, cancellation)`; `CdbgLoggingProtocol(std::unique_ptr<ICanTransport>, std::vector<LoggingChannel>, CdbgProtocolConfig)`.

- [ ] **Step 1: Write failing configurable-driver tests**

Add a test config made with `make_cdbg_protocol_config(0x620, 0x621, 3, 25)`. Script every handshake write on `0x620`, every reply on `0x621`, `buildLogResetFrame(3)`, and `buildLogStartFrame(3, 1, 25)`. Queue one streamed frame on `0x621` and verify it decodes. Add a second poll test that queues the old `0x631` and expects `responded == false`.

- [ ] **Step 2: Run the driver test and verify signature failures**

Run: `bazel test //src/backend/protocol:test_cdbg_driver --test_output=errors`

Expected: FAIL because the driver has no configuration constructor and still hard-codes IDs.

- [ ] **Step 3: Thread configuration through the driver**

Store the configuration by value:

```cpp
explicit CdbgLogDriver(cdbg::ICanTransport& transport, cdbg::CdbgProtocolConfig config);
fastecu::Status startFreeFormLog(const std::vector<CdbgChannel>& channels,
                                 const fastecu::ICancellationToken& cancellation);

cdbg::ICanTransport& transport_;
cdbg::CdbgProtocolConfig config_;
```

Make `sendAndReceive` accept `const CdbgProtocolConfig&`, write to `request_id()`, and require `reply_id()`. Use the same reply ID in `pollOnce`. Remove instance and interval parameters from `startFreeFormLog`; use the config getters for reset and start frames.

Update `ICanTransport` comments to say arbitration IDs are explicit and their 11/29-bit mode is configured by the caller; do not change its methods.

- [ ] **Step 4: Run driver tests**

Run: `bazel test //src/backend/protocol:test_cdbg_driver --test_output=errors`

Expected: PASS, including existing default-wire tests updated to construct the validated Colt config.

- [ ] **Step 5: Write failing logging-protocol configuration test**

Update `makeProtocol` to require a `CdbgProtocolConfig`. Add a non-default test that scripts `0x620/0x621`, instance `3`, and interval `25`, then asserts `start()` consumes the script. Existing tests should use `*make_colt_cdbg_protocol_config()`.

- [ ] **Step 6: Run and observe constructor failures**

Run: `bazel test //src/backend/logging/protocols:test_cdbg_logging_protocol --test_output=errors`

Expected: FAIL until `CdbgLoggingProtocol` accepts and forwards configuration.

- [ ] **Step 7: Implement logging-protocol and legacy-factory wiring**

Change the constructor to:

```cpp
CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport,
                    std::vector<LoggingChannel> channels,
                    cdbg::CdbgProtocolConfig config);
```

Initialize `driver_(*transport_, std::move(config))` and call `driver_.startFreeFormLog(wire_channels_, cancellation)`. Add the protocol-config dependency to Bazel.

In the legacy factory's concrete CDBG builder lambda, create `auto config = cdbg::make_colt_cdbg_protocol_config();`, propagate its error, and pass `std::move(*config)` to `CdbgLoggingProtocol`. Keep the injected `ProtocolBuilder` test seam channel-only: focused CDBG protocol tests already prove the default wire values, while legacy factory tests continue to prove setup/open/build ordering and error propagation.

- [ ] **Step 8: Run protocol and desktop compatibility tests**

Run: `bazel test //src/backend/logging/protocols:test_cdbg_logging_protocol //src/platform/desktop/common/logging:test_legacy_logging_protocol_factory --test_output=errors`

Expected: PASS.

- [ ] **Step 9: Commit configurable wire traffic**

```bash
git add src/backend/protocol src/backend/logging/protocols src/platform/desktop/common/logging/legacy_logging_protocol_factory.cpp src/platform/desktop/common/logging/legacy_logging_protocol_factory_test.cpp
git commit -m "feat: drive CDBG logging from explicit configuration"
```

---

### Task 3: Replace poll-count reconnects with monotonic deadlines

**Files:**
- Modify: `src/backend/logging/logging_types.h`
- Modify: `src/backend/logging/logging_session.cpp`
- Modify: `src/backend/logging/logging_session_test.cpp`
- Modify: `src/backend/logging/logging_use_case.h`
- Modify: `src/backend/logging/logging_use_case.cpp`
- Modify: `src/backend/logging/logging_use_case_test.cpp`
- Modify: `src/backend/logging/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::IClock::now_ms()` and `FakeClock`.
- Produces: elapsed-time `LoggingPolicy`; `explicit LoggingUseCase(IClock&)`; unchanged `run(...)` arguments after construction.

- [ ] **Step 1: Replace policy fields in tests and add validation cases**

Define valid policy fixtures as:

```cpp
LoggingPolicy{
    .poll_timeout_ms = 10,
    .car_silence_miss_threshold = 2,
    .reconnect_initial_delay_ms = 20,
    .reconnect_period_ms = 10,
    .max_reconnect_attempts = 3,
};
```

Add session tests rejecting nonpositive poll timeout/silence threshold, negative initial delay, nonpositive repeat period, and a present zero attempt limit. Confirm `std::nullopt` is valid.

- [ ] **Step 2: Update the value type and session validation**

Replace `reconnect_attempt_threshold` and `reconnect_retry_period` with:

```cpp
int reconnect_initial_delay_ms;
int reconnect_period_ms;
std::optional<std::uint32_t> max_reconnect_attempts;
```

Include `<optional>` and validate exactly the cases in Step 1. Run:

`bazel test //src/backend/logging:logging_session_test --test_output=errors`

Expected: PASS.

- [ ] **Step 3: Write failing deadline and budget tests**

Use `FakeClock clock; LoggingUseCase use_case(clock);`. Make the scripted protocol advance the fake clock by its received poll timeout on each poll. Add tests proving:

- silence emits at miss 2, with no restart before `silence_time + 20`;
- later restarts are at least 10 ms apart even when polls advance by 3 ms;
- both a successful restart and a `BadResponse` restart consume attempts;
- a valid sample resets the attempt count and returns to `Running`;
- three attempts without a sample return `BadResponse` and detail `logging reconnect attempts exhausted`;
- `std::nullopt` continues beyond three attempts until cancellation;
- `Disconnected` from restart terminates immediately;
- a clock at `UINT64_MAX - 5` does not wrap a 20 ms deadline.

Record diagnostic messages and assert the most recent retry failure detail is logged when exhaustion occurs.

- [ ] **Step 4: Run use-case tests and verify compile/behavior failure**

Run: `bazel test //src/backend/logging:logging_use_case_test --test_output=errors`

Expected: FAIL because `LoggingUseCase` has no clock constructor and reconnects still use poll counts.

- [ ] **Step 5: Implement the monotonic reconnect state machine**

Store `IClock& clock_`. Add a saturating helper:

```cpp
std::uint64_t saturated_add(std::uint64_t base, std::uint64_t delta) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return delta > max - base ? max : base + delta;
}
```

On the miss that reaches the silence threshold, emit `CarNotResponding` and set the first deadline from `clock_.now_ms()`. When `now_ms() >= deadline`, call `start()`, increment attempts, and schedule the next deadline from the current monotonic time. Do not emit `Running` or reset misses/attempts after `start()`; do both only after `PollData::responded` is true and samples convert successfully. Return fatal restart errors immediately. On finite exhaustion, log the last `BadResponse` detail when present and return the stable exhaustion error.

- [ ] **Step 6: Run logging tests**

Run: `bazel test //src/backend/logging:logging_session_test //src/backend/logging:logging_use_case_test //src/backend/logging:logging_conversion_test --test_output=errors`

Expected: PASS.

- [ ] **Step 7: Commit elapsed-time policy**

```bash
git add src/backend/logging/logging_types.h src/backend/logging/logging_session.cpp src/backend/logging/logging_session_test.cpp src/backend/logging/logging_use_case.h src/backend/logging/logging_use_case.cpp src/backend/logging/logging_use_case_test.cpp src/backend/logging/BUILD.bazel
git commit -m "refactor(logging): schedule reconnects by elapsed time"
```

---

### Task 4: Adapt the desktop worker and Widgets compatibility boundary

**Files:**
- Modify: `src/platform/desktop/common/logging/logging_snapshot_adapter.h`
- Modify: `src/platform/desktop/common/logging/logging_snapshot_adapter.cpp`
- Modify: `src/platform/desktop/common/logging/logging_adapters_test.cpp`
- Modify: `src/platform/desktop/common/logging/logging_worker.h`
- Modify: `src/platform/desktop/common/logging/logging_worker.cpp`
- Modify: `src/platform/desktop/common/logging/logging_worker_test.cpp`
- Modify: `src/platform/desktop/common/logging/logging_engine.h`
- Modify: `src/platform/desktop/common/logging/logging_engine.cpp`
- Modify: `src/platform/desktop/common/logging/logging_engine_test.cpp`
- Modify: `src/platform/desktop/common/logging/BUILD.bazel`
- Modify: `src/ui/desktop/menu_actions.cpp`

**Interfaces:**
- Consumes: elapsed-time `LoggingPolicy` and `LoggingUseCase(IClock&)` from Task 3.
- Produces: `LoggingPolicy make_legacy_logging_policy(int poll_timeout_ms, int silence_misses, int first_reconnect_miss, int repeat_misses)`; `LoggingWorker(..., IClock&, ...)`; engine-owned `QtClock`.

- [ ] **Step 1: Write compatibility-conversion tests**

Add assertions for the exact legacy schedules:

```cpp
auto can = make_legacy_logging_policy(50, 20, 100, 20);
EXPECT_EQ(can.reconnect_initial_delay_ms, 4000);
EXPECT_EQ(can.reconnect_period_ms, 1000);
EXPECT_EQ(can.max_reconnect_attempts, std::nullopt);

auto ssm = make_legacy_logging_policy(300, 10, 30, 10);
EXPECT_EQ(ssm.reconnect_initial_delay_ms, 6000);
EXPECT_EQ(ssm.reconnect_period_ms, 3000);
EXPECT_EQ(ssm.max_reconnect_attempts, std::nullopt);
```

Add an overflow case using `INT_MAX` inputs and assert both computed durations saturate at `INT_MAX`.

- [ ] **Step 2: Implement the compatibility helper and update menu callers**

Implement checked multiplication in `logging_snapshot_adapter.cpp`, clamping to `INT_MAX`; clamp the initial miss difference at zero. Return the new policy with unlimited attempts. Replace all three aggregate policy literals in `menu_actions.cpp` with the helper calls above, preserving their protocol-specific inputs.

Run: `bazel test //src/platform/desktop/common/logging:test_logging_adapters --test_output=errors`

Expected: PASS.

- [ ] **Step 3: Write failing worker clock-injection test changes**

Use `FakeClock clock` in each worker test and construct:

```cpp
LoggingWorker worker(session(), &protocol, clock, diagnostics);
```

Configure the fake to advance with polls where the test needs a reconnect deadline. Keep the existing blocked-poll destruction test to prove cancellation and joined shutdown are unaffected.

- [ ] **Step 4: Inject the clock through worker and engine**

Change the worker signature to:

```cpp
LoggingWorker(LoggingSession session, LoggingProtocol *protocol,
              fastecu::IClock& clock, fastecu::IEventSink& diagnostics,
              QObject *parent = nullptr);
```

Construct `use_case_(clock)` in the worker initializer. Add `QtClock clock_;` to `LoggingEngine` before worker creation and pass it to every worker. Do not expose the clock in `LoggingRun` or protocol factories.

- [ ] **Step 5: Update engine/worker fixtures and run desktop logging tests**

Update every test policy to the elapsed-time fields. For tests that should never reconnect, use a large initial delay and `std::nullopt`; for reconnect-state tests, use `FakeClock` auto-advance. Run:

```bash
bazel test //src/platform/desktop/common/logging:test_logging_worker //src/platform/desktop/common/logging:test_logging_engine //src/platform/desktop/common/logging:test_logging_adapters //src/platform/desktop/common/logging:test_legacy_logging_coordinator --test_output=errors
```

Expected: PASS with the existing state, completion, cancellation, and diagnostic assertions unchanged except for explicit clock setup.

- [ ] **Step 6: Build the Widgets target**

Run: `bazel build --config=release //:fastecu`

Expected: PASS; no old retry-policy field remains (`rg -n "reconnect_attempt_threshold|reconnect_retry_period" src` prints only compatibility-helper parameter names or nothing).

- [ ] **Step 7: Commit desktop compatibility**

```bash
git add src/platform/desktop/common/logging src/ui/desktop/menu_actions.cpp
git commit -m "refactor(ui): preserve legacy logging retry timing"
```

---

### Task 5: Build the combined prepared dashboard session

**Files:**
- Create: `src/backend/dashboard/dashboard_session_builder.h`
- Create: `src/backend/dashboard/dashboard_session_builder.cpp`
- Create: `src/backend/dashboard/dashboard_session_builder_test.cpp`
- Modify: `src/backend/dashboard/BUILD.bazel`
- Modify: `BUILD.bazel`
- Modify: `scripts/check-portable-closure.py`

**Interfaces:**
- Consumes: `DashboardDocument`, `validate_dashboard_document`, `make_logging_session`, and `make_cdbg_protocol_config`.
- Produces: move-only `dashboard::PreparedDashboardSession`; `Result<PreparedDashboardSession> prepare_dashboard_session(const DashboardDocument&)`.

- [ ] **Step 1: Write failing happy-path and ordering tests**

Start from `test::valid_document()`. Add a second catalog channel and conversion, arrange cards in reverse display order, and assert preparation produces:

```cpp
ASSERT_TRUE(result);
EXPECT_EQ(result->session().protocol(), LoggingProtocolId::Cdbg);
ASSERT_EQ(result->session().channels().size(), 2U);
EXPECT_EQ(result->session().channels()[0].id, document.channels[0].id);
EXPECT_EQ(result->session().channels()[1].id, document.channels[1].id);
EXPECT_EQ(result->session().channels()[0].from_byte_expression,
          chosen_conversion.expression);
EXPECT_EQ(result->config().request_id(), document.connection.request_id);
EXPECT_EQ(result->config().reply_id(), document.connection.reply_id);
EXPECT_EQ(result->config().stream_instance(), document.connection.stream_instance);
EXPECT_EQ(result->config().sampling_interval_ms(),
          document.connection.sampling_interval_ms);
```

Add a document with an unused middle catalog channel and assert it is omitted without changing the relative catalog order of selected channels. Assert the policy maps poll timeout, silence threshold, both reconnect delays, and finite attempts exactly.

- [ ] **Step 2: Run and verify the builder target is absent**

Run: `bazel test //src/backend/dashboard:dashboard_session_builder_test --test_output=errors`

Expected: FAIL because the target and builder do not exist.

- [ ] **Step 3: Define the move-only combined contract**

Declare:

```cpp
namespace fastecu::dashboard {
class PreparedDashboardSession {
  public:
    PreparedDashboardSession(const PreparedDashboardSession&) = delete;
    PreparedDashboardSession& operator=(const PreparedDashboardSession&) = delete;
    PreparedDashboardSession(PreparedDashboardSession&&) = default;
    PreparedDashboardSession& operator=(PreparedDashboardSession&&) = default;
    const logging::LoggingSession& session() const;
    const cdbg::CdbgProtocolConfig& config() const;
    std::pair<logging::LoggingSession, cdbg::CdbgProtocolConfig>
        into_parts() &&;
  private:
    PreparedDashboardSession(logging::LoggingSession, cdbg::CdbgProtocolConfig);
    logging::LoggingSession session_;
    cdbg::CdbgProtocolConfig config_;
    friend fastecu::Result<PreparedDashboardSession>
        prepare_dashboard_session(const DashboardDocument&);
};

fastecu::Result<PreparedDashboardSession>
prepare_dashboard_session(const DashboardDocument& document);
} // namespace fastecu::dashboard
```

`into_parts()` moves both values out in one operation so the later connection layer cannot take only half and accidentally reuse the prepared object. Do not add document, adapter, transport, or presentation fields.

- [ ] **Step 4: Implement deterministic preparation**

First call `validate_dashboard_document(document)`. Build lookup maps for channel and conversion validation, and a set of referenced channel IDs. Iterate `document.channels`—not cards—to create `LoggingChannel`s. For each referenced channel, find its sole card and selected conversion, then map ID, address, length, unsigned raw assembly, expression, unit, and precision.

Build policy exactly as:

```cpp
logging::LoggingPolicy policy{
    .poll_timeout_ms = checked_int(document.connection.retry.poll_timeout_ms),
    .car_silence_miss_threshold = checked_int(document.connection.retry.silence_threshold),
    .reconnect_initial_delay_ms = checked_int(document.connection.retry.reconnect_period_ms),
    .reconnect_period_ms = checked_int(document.connection.retry.reconnect_period_ms),
    .max_reconnect_attempts = document.connection.retry.reconnect_attempts,
};
```

Reject any `uint32_t` that cannot fit the generic policy's `int` fields with its exact dashboard path. Call `make_logging_session(Cdbg, ...)` so CDBG frame-capacity validation remains centralized. Construct config through `make_cdbg_protocol_config`. Propagate errors with stable field-path context and return the pair only after both factories succeed.

- [ ] **Step 5: Add exhaustive failure tests**

Add one mutation per case and assert `InvalidConfig`, the stable path/detail, and no value: no cards, missing channel reference, missing conversion reference, duplicate card order/ID, one channel on two cards, unsupported raw assembly, over-`INT_MAX` retry values, invalid sampling interval, and 57 one-byte selected channels exceeding eight CDBG frames. Even where document validation catches a case, keep the builder test to pin defensive behavior.

- [ ] **Step 6: Run builder and dashboard tests**

Run: `bazel test //src/backend/dashboard:all --test_output=errors`

Expected: PASS.

- [ ] **Step 7: Add portable-closure coverage**

Add `dashboard_session_builder` and `dashboard_session_builder_test` to the root portable target expression/scope and dashboard package declarations in `scripts/check-portable-closure.py`, following the existing five dashboard production targets. Add `cdbg_protocol_config` and its test wherever the protocol package's required portable labels are enumerated.

Run: `bazel test //:portable_closure --test_output=errors`

Expected: PASS and report both new production targets without Qt/platform dependencies.

- [ ] **Step 8: Commit the preparation boundary**

```bash
git add src/backend/dashboard/dashboard_session_builder.h src/backend/dashboard/dashboard_session_builder.cpp src/backend/dashboard/dashboard_session_builder_test.cpp src/backend/dashboard/BUILD.bazel src/backend/protocol/BUILD.bazel BUILD.bazel scripts/check-portable-closure.py
git commit -m "feat: prepare dashboard CDBG logging sessions"
```

---

### Task 6: Run the complete regression and scope gates

**Files:**
- Verify only; modify a task-owned file only when a gate exposes a defect in this plan's scope.

**Interfaces:**
- Consumes: all deliverables from Tasks 1–5.
- Produces: verified portable preparation, desktop compatibility, and two buildable application targets.

- [ ] **Step 1: Run all focused tests**

```bash
bazel test --config=release \
  //src/backend/protocol:cdbg_protocol_config_test \
  //src/backend/protocol:test_cdbg_driver \
  //src/backend/logging:logging_session_test \
  //src/backend/logging:logging_use_case_test \
  //src/backend/logging:logging_conversion_test \
  //src/backend/logging/protocols:test_cdbg_logging_protocol \
  //src/backend/dashboard:all \
  //src/platform/desktop/common/logging:test_logging_adapters \
  //src/platform/desktop/common/logging:test_logging_worker \
  //src/platform/desktop/common/logging:test_logging_engine \
  //src/platform/desktop/common/logging:test_legacy_logging_protocol_factory \
  //src/platform/desktop/common/logging:test_legacy_logging_coordinator \
  //:portable_closure \
  --test_output=errors
```

Expected: every target PASS.

- [ ] **Step 2: Build both desktop applications**

Run: `bazel build --config=release //:fastecu //:fastecu-desktop-quick`

Expected: PASS.

- [ ] **Step 3: Run repository formatting and static checks**

Run: `prek run --all-files`

Expected: PASS. If a hook changes a task-owned file, inspect the diff, rerun its focused test, and rerun `prek run --all-files`.

- [ ] **Step 4: Audit scope and stale APIs**

```bash
rg -n "reconnect_attempt_threshold|reconnect_retry_period" src
rg -n "kRequestCanId|kReplyCanId" src/backend/protocol/mitsu_colt_can_cdbg_driver.cpp src/backend/logging/protocols/portable_cdbg_logging_protocol.cpp
rg -n "SerialPortActions|Qt|src/platform|adapter|transport" src/backend/dashboard/dashboard_session_builder.h src/backend/dashboard/dashboard_session_builder.cpp
git diff --check
```

Expected: old policy field names are absent except intentionally named compatibility-helper parameters; the driver/protocol do not hard-code Colt IDs; the dashboard builder has no Qt, platform, adapter, or transport dependency; diff check is silent.

- [ ] **Step 5: Review final diff and commit gate-only corrections**

Run: `git status --short && git diff --stat && git log -6 --oneline`

Expected: only this plan's implementation files plus the user's pre-existing unrelated changes appear; Tasks 1–5 each have a focused commit. If verification required a correction, add that correction to the task that owns the file, rerun that task's focused test, and amend that task's commit with `git commit --amend --no-edit` before repeating this gate.
