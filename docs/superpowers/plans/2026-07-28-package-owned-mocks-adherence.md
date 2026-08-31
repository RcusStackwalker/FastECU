# Package-owned Mocks Adherence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate every existing test double for an interface owned by `src/backend/**` or `src/algorithms/**` into the interface owner's `testing/` subpackage, with one Bazel library and one dedicated test per double.

**Architecture:** Reusable, behavior-configurable doubles replace duplicated consumer-local classes. Backend ports, logging, flash, and protocol packages each own focused header-only doubles under `testing/`; consumers depend on individual test-only targets. Existing scripts, failure modes, observable histories, and the SSM clock's automatic time advancement remain behaviorally equivalent.

**Tech Stack:** C++23, Bazel `rules_cc`, GoogleTest/QtTest, buildifier, prek.

## Global Constraints

- Scope is limited to current doubles for interfaces owned by `src/backend/**` and `src/algorithms/**`, including consumers under top-level `tests/**`.
- Interfaces owned outside those trees, production implementations, and helpers that do not substitute for an in-scope interface are excluded.
- Do not add lint, presubmit, or other future-enforcement machinery.
- Each concrete double has one header, one `cc_library`, and one independent `cc_test`.
- Every new `testing/BUILD.bazel` sets `package(default_testonly = True, ...)`; do not repeat `testonly = True` on individual targets.
- Consumer targets depend only on the individual doubles they use.
- Preserve all current assertions and test semantics, including the SSM clock's 10 ms `now_ms()` and `sleep()` auto-advancement.
- Use project `Status`, `Result`, and `ErrorKind` values for configured failures; reusable doubles must not throw or invoke test-framework assertions.
- Follow test-driven development: add a failing dedicated test, observe the expected failure, implement the minimum behavior, then migrate consumers.
- Preserve unrelated untracked files and user changes.

---

## File Structure

### Backend ports

- `src/backend/ports/testing/fake_cancellation_token.h`: configurable `ICancellationToken`.
- `src/backend/ports/testing/fake_cancellation_token_test.cpp`: fixed, mutable, check-count, and predicate tests.
- `src/backend/ports/testing/fake_clock.h`: existing clock plus optional automatic advancement.
- `src/backend/ports/testing/fake_clock_test.cpp`: existing behavior plus auto-advance coverage.
- `src/backend/ports/testing/in_memory_file_system.h`: explicit `create_directory` failure.
- `src/backend/ports/testing/in_memory_file_system_test.cpp`: failure-injection coverage.
- `src/backend/ports/testing/in_memory_file_repository.h`: queued read result and read/write recording.
- `src/backend/ports/testing/in_memory_file_repository_test.cpp`: recording and configured-result coverage.
- `src/backend/ports/testing/BUILD.bazel`: individual test-only targets.

### Backend logging

- `src/backend/logging/testing/scripted_logging_protocol.h`: union of both current scripted protocol behaviors.
- `src/backend/logging/testing/scripted_logging_protocol_test.cpp`: queue, history, stop, and blocking-cancellation coverage.
- `src/backend/logging/testing/recording_logging_event_sink.h`: state and sample-batch recorder.
- `src/backend/logging/testing/recording_logging_event_sink_test.cpp`: ordered recording coverage.
- `src/backend/logging/testing/BUILD.bazel`: package-level test-only declaration and individual targets.

### Backend protocol

- `src/backend/protocol/testing/scripted_can_transport.h`: moved CDBG CAN script.
- `src/backend/protocol/testing/scripted_kline_transport.h`: moved MUT-DMA K-line script.
- `src/backend/protocol/testing/scripted_ssm_transport.h`: moved SSM script.
- `src/backend/protocol/testing/scripted_mut_dma_init.h`: configurable `IMutDmaInit` result/call recorder.
- Corresponding `*_test.cpp` files: independent public-contract tests.
- `src/backend/protocol/testing/BUILD.bazel`: package-level test-only declaration and individual targets.

### Backend flash

- `src/backend/flash/testing/scripted_can_flash_transport.h`: moved framed CAN flash script.
- `src/backend/flash/testing/scripted_kline_flash_transport.h`: moved K-line flash script.
- `src/backend/flash/testing/stub_flash_transport.h`: minimal `IFlashTransport` with unblock recording.
- `src/backend/flash/testing/scripted_flash_executor.h`: configurable `IFlashExecutor` result/call recorder used by dialog tests.
- Corresponding `*_test.cpp` files: independent public-contract tests.
- `src/backend/flash/testing/BUILD.bazel`: package-level test-only declaration and individual targets.

### Consumers and build graph

- `src/backend/logging/logging_use_case_test.cpp`, `src/backend/logging/BUILD.bazel`
- `src/backend/flash/eeprom/*_executor_test.cpp`, `src/backend/flash/eeprom/BUILD.bazel`
- `tests/test_*.cpp`, `tests/BUILD.bazel`
- `bazel/mut_dma_test_suites.bzl`, `bazel/test_sources.bzl`
- Delete superseded `tests/scripted_{logging,can,kline,ssm,can_flash,kline_flash}_transport.h` files.

---

### Task 1: Configurable cancellation token

**Files:**
- Create: `src/backend/ports/testing/fake_cancellation_token.h`
- Create: `src/backend/ports/testing/fake_cancellation_token_test.cpp`
- Modify: `src/backend/ports/testing/BUILD.bazel`
- Modify: every in-scope test containing a local `ICancellationToken` implementation
- Modify: `bazel/mut_dma_test_suites.bzl`
- Modify: `tests/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::ICancellationToken::cancelled() const -> bool`
- Produces:
  - `explicit FakeCancellationToken(bool cancelled = false)`
  - `void set_cancelled(bool cancelled)`
  - `void cancel_on_check(std::size_t one_based_check)`
  - `void set_predicate(std::function<bool()> predicate)`
  - `std::size_t check_count() const`
  - `bool cancelled() const override`

- [ ] **Step 1: Write the failing public-contract test**

```cpp
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include <gtest/gtest.h>

TEST(FakeCancellationToken, SupportsFixedMutableAndCheckCountBehavior)
{
    fastecu::FakeCancellationToken token;
    EXPECT_FALSE(token.cancelled());
    token.set_cancelled(true);
    EXPECT_TRUE(token.cancelled());

    fastecu::FakeCancellationToken counted;
    counted.cancel_on_check(3);
    EXPECT_FALSE(counted.cancelled());
    EXPECT_FALSE(counted.cancelled());
    EXPECT_TRUE(counted.cancelled());
    EXPECT_EQ(counted.check_count(), 3u);
}

TEST(FakeCancellationToken, PredicateCanObserveAnotherDouble)
{
    int polls = 0;
    fastecu::FakeCancellationToken token;
    token.set_predicate([&polls] { return polls >= 2; });
    EXPECT_FALSE(token.cancelled());
    polls = 2;
    EXPECT_TRUE(token.cancelled());
}
```

- [ ] **Step 2: Add the Bazel targets and verify the test fails**

Add `:fake_cancellation_token` and `:fake_cancellation_token_test` to
`src/backend/ports/testing/BUILD.bazel`.

Run:

```bash
bazel test //src/backend/ports/testing:fake_cancellation_token_test
```

Expected: FAIL because `fake_cancellation_token.h` and its API do not exist.

- [ ] **Step 3: Implement the minimal configurable token**

Use `cancel_on_check(1)` to represent always-cancelled and the default
constructor to represent never-cancelled. `cancelled()` increments
`check_count_` once per call; a predicate takes precedence over
`cancel_on_check_`, which takes precedence over `cancelled_`.

```cpp
class FakeCancellationToken final : public ICancellationToken
{
  public:
    explicit FakeCancellationToken(bool cancelled = false) : cancelled_(cancelled) {}
    void set_cancelled(bool value) { cancelled_ = value; }
    void cancel_on_check(std::size_t one_based_check)
    {
        cancel_on_check_ = one_based_check;
    }
    void set_predicate(std::function<bool()> predicate)
    {
        predicate_ = std::move(predicate);
    }
    std::size_t check_count() const { return check_count_; }
    bool cancelled() const override
    {
        ++check_count_;
        if (predicate_)
            return predicate_();
        if (cancel_on_check_)
            return check_count_ >= *cancel_on_check_;
        return cancelled_;
    }

  private:
    bool cancelled_;
    std::optional<std::size_t> cancel_on_check_;
    std::function<bool()> predicate_;
    mutable std::size_t check_count_ = 0;
};
```

- [ ] **Step 4: Run the dedicated test**

```bash
bazel test //src/backend/ports/testing:fake_cancellation_token_test
```

Expected: PASS.

- [ ] **Step 5: Replace every local cancellation-token class**

Use:

```cpp
FakeCancellationToken active;
FakeCancellationToken cancelled(true);
FakeCancellationToken second_check;
second_check.cancel_on_check(2);
FakeCancellationToken after_polls;
after_polls.set_predicate([&protocol] { return protocol.polls_completed >= 1; });
```

Replace local implementations in:

```text
src/backend/ports/testing/fake_clock_test.cpp
src/backend/logging/logging_use_case_test.cpp
src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor_test.cpp
src/backend/flash/eeprom/denso_sh705x_eeprom_kline_executor_test.cpp
tests/test_cdbg_driver.cpp
tests/test_cdbg_logging_protocol.cpp
tests/test_desktop_can_flash_transport.cpp
tests/test_desktop_kline_flash_transport.cpp
tests/test_driver.cpp
tests/test_facade_threading.cpp
tests/test_mut_dma_logging_protocol.cpp
tests/test_ssm_logging_protocol.cpp
tests/test_transport.cpp
tests/tst_mut_dma_integration.cpp
```

Add `//src/backend/ports/testing:fake_cancellation_token` only to targets that
compile one of these files. For generated MUT-DMA suites, add the dependency
to the exact `SUITE_DEPS` entries rather than globally.

- [ ] **Step 6: Run all affected cancellation consumers**

```bash
bazel test \
  //src/backend/ports/testing:fake_clock_test \
  //src/backend/logging:logging_use_case_test \
  //src/backend/flash/eeprom:denso_sh705x_eeprom_can_executor_test \
  //src/backend/flash/eeprom:denso_sh705x_eeprom_kline_executor_test \
  //tests:test_cdbg_driver \
  //tests:test_cdbg_logging_protocol \
  //tests:test_driver \
  //tests:test_mut_dma_logging_protocol \
  //tests:test_ssm_logging_protocol \
  //tests:test_transport
```

Expected: PASS. Run the platform-compatible desktop targets separately if the
host supports them.

- [ ] **Step 7: Commit**

```bash
git add src/backend/ports/testing tests src/backend/logging/logging_use_case_test.cpp \
  src/backend/flash/eeprom bazel/mut_dma_test_suites.bzl
git commit -m "test: centralize cancellation token fake"
```

### Task 2: Preserve both FakeClock timing models

**Files:**
- Modify: `src/backend/ports/testing/fake_clock.h`
- Modify: `src/backend/ports/testing/fake_clock_test.cpp`
- Modify: `tests/test_ssm_logging_protocol.cpp`

**Interfaces:**
- Consumes: `fastecu::IClock`
- Produces:
  - `void set_now_auto_advance_ms(std::uint64_t step_ms)`
  - `void set_sleep_advance_ms(std::optional<std::uint64_t> step_ms)`
  - Existing `now_` and default `sleep(ms, token)` semantics unchanged

- [ ] **Step 1: Add failing auto-advance tests**

```cpp
TEST(FakeClock, OptionalAutoAdvancePreservesSsmTimingModel)
{
    FakeClock clock;
    clock.set_now_auto_advance_ms(10);
    clock.set_sleep_advance_ms(10);
    FakeCancellationToken active;

    EXPECT_EQ(clock.now_ms(), 0u);
    EXPECT_EQ(clock.now_ms(), 10u);
    ASSERT_TRUE(clock.sleep(999, active));
    EXPECT_EQ(clock.now_ms(), 30u);
}
```

- [ ] **Step 2: Verify the new test fails**

```bash
bazel test //src/backend/ports/testing:fake_clock_test
```

Expected: FAIL because the configuration methods do not exist.

- [ ] **Step 3: Implement optional advancement**

Store `mutable std::uint64_t now_auto_advance_ms_ = 0` and
`std::optional<std::uint64_t> sleep_advance_ms_`. `now_ms()` returns the
pre-increment value. Successful `sleep()` advances by `sleep_advance_ms_` when
set, otherwise by `max(ms, 0)`. Cancellation still prevents advancement.

- [ ] **Step 4: Configure the SSM protocol test and delete its local clock**

```cpp
fastecu::FakeClock clock;
clock.set_now_auto_advance_ms(10);
clock.set_sleep_advance_ms(10);
```

Do not manually advance time in the SSM tests.

- [ ] **Step 5: Run both clock and SSM tests**

```bash
bazel test \
  //src/backend/ports/testing:fake_clock_test \
  //tests:test_ssm_logging_protocol
```

Expected: PASS with the existing timeout and script assertions unchanged.

- [ ] **Step 6: Commit**

```bash
git add src/backend/ports/testing/fake_clock.h \
  src/backend/ports/testing/fake_clock_test.cpp \
  tests/test_ssm_logging_protocol.cpp
git commit -m "test: support auto-advancing fake clock"
```

### Task 3: Consolidate backend port storage doubles

**Files:**
- Modify: `src/backend/ports/testing/in_memory_file_system.h`
- Modify: `src/backend/ports/testing/in_memory_file_system_test.cpp`
- Modify: `src/backend/ports/testing/in_memory_file_repository.h`
- Modify: `src/backend/ports/testing/in_memory_file_repository_test.cpp`
- Modify: `src/backend/config/provisioning_test.cpp`
- Modify: `tests/test_flash_snapshot_adapter.cpp`
- Modify: `tests/BUILD.bazel`

**Interfaces:**
- Produces:
  - `std::optional<Error> create_directory_error`
  - `std::optional<Result<std::vector<std::uint8_t>>> next_read_result`
  - `std::vector<std::string> read_handles`
  - `std::vector<std::pair<std::string, std::vector<std::uint8_t>>> write_calls`

- [ ] **Step 1: Add failing failure-injection and recording tests**

```cpp
TEST(InMemoryFileSystem, ConfiguredCreateDirectoryFailureIsReturned)
{
    InMemoryFileSystem fs;
    fs.create_directory_error = Error{ErrorKind::Internal, "mkdir failed"};
    auto result = fs.create_directory("/config/");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), *fs.create_directory_error);
    EXPECT_FALSE(fs.exists("/config/"));
}

TEST(InMemoryFileRepository, RecordsAndCanOverrideNextRead)
{
    InMemoryFileRepository repository;
    repository.next_read_result =
        fail(ErrorKind::Internal, "disk error");
    auto result = repository.read("kernel");
    ASSERT_FALSE(result);
    EXPECT_EQ(repository.read_handles,
              std::vector<std::string>{"kernel"});
}
```

- [ ] **Step 2: Run the tests and verify failure**

```bash
bazel test \
  //src/backend/ports/testing:in_memory_file_system_test \
  //src/backend/ports/testing:in_memory_file_repository_test
```

Expected: FAIL because the new configuration/history fields do not exist.

- [ ] **Step 3: Implement minimal explicit configuration**

Check `create_directory_error` before mutating `directories`. Record every
read handle before returning a one-shot `next_read_result` when present; clear
the optional after consuming it. Record writes before updating `files`.

- [ ] **Step 4: Replace consumer-local subclasses**

- Replace `provisioning_test.cpp`'s `FailingFileSystem` with
  `fs.create_directory_error = Error{...}`.
- Replace `test_flash_snapshot_adapter.cpp`'s `RecordingFileRepository` with
  `InMemoryFileRepository`; seed `files["kernel-handle-1"]`, configure
  `next_read_result` for the failure test, and assert `read_handles`.
- Add `//src/backend/ports/testing:in_memory_file_repository` to
  `//tests:test_flash_snapshot_adapter`.

- [ ] **Step 5: Run dedicated and consumer tests**

```bash
bazel test \
  //src/backend/ports/testing:in_memory_file_system_test \
  //src/backend/ports/testing:in_memory_file_repository_test \
  //src/backend/config:provisioning_test \
  //tests:test_flash_snapshot_adapter
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/backend/ports/testing src/backend/config/provisioning_test.cpp \
  tests/test_flash_snapshot_adapter.cpp tests/BUILD.bazel
git commit -m "test: consolidate backend storage doubles"
```

### Task 4: Package-own logging doubles

**Files:**
- Create: `src/backend/logging/testing/BUILD.bazel`
- Create: `src/backend/logging/testing/scripted_logging_protocol.h`
- Create: `src/backend/logging/testing/scripted_logging_protocol_test.cpp`
- Create: `src/backend/logging/testing/recording_logging_event_sink.h`
- Create: `src/backend/logging/testing/recording_logging_event_sink_test.cpp`
- Modify: `src/backend/logging/logging_use_case_test.cpp`
- Modify: `src/backend/logging/BUILD.bazel`
- Modify: `tests/test_logging_engine.cpp`
- Modify: `tests/test_logging_worker.cpp`
- Modify: `bazel/mut_dma_test_suites.bzl`
- Modify: `bazel/test_sources.bzl`
- Delete: `tests/scripted_logging_protocol.h`

**Interfaces:**
- Produces `fastecu::logging::ScriptedLoggingProtocol` with:
  - `queue_start_result(Status)`, `queue_poll_result(Result<PollData>)`
  - `block_poll_until_cancelled()`, `wait_until_poll_entered(duration)`
  - public `start_result`, `stop_result`, `starts`, `stops`,
    `polls_completed`, `poll_timeouts`, and `start_call_poll_numbers`
- Produces `fastecu::logging::RecordingLoggingEventSink` with public `states`
  and `sample_batches`

- [ ] **Step 1: Write failing dedicated tests for the union API**

Cover queued/default start results, queued/default poll results, poll timeout
history, stop result/count, ordered sink recording, and a thread that calls
`poll()` in blocking mode until a `FakeCancellationToken` is cancelled.

```cpp
TEST(ScriptedLoggingProtocol, RecordsCallsAndConsumesQueuedResults)
{
    ScriptedLoggingProtocol protocol;
    protocol.queue_poll_result(PollData{.responded = true});
    FakeCancellationToken active;
    ASSERT_TRUE(protocol.start(active));
    auto poll = protocol.poll(25, active);
    ASSERT_TRUE(poll);
    EXPECT_TRUE(poll->responded);
    EXPECT_EQ(protocol.poll_timeouts, std::vector<int>{25});
    ASSERT_TRUE(protocol.stop());
    EXPECT_EQ(protocol.starts, 1);
    EXPECT_EQ(protocol.stops, 1);
}
```

- [ ] **Step 2: Add package-level Bazel targets and verify failure**

Use:

```starlark
package(
    default_testonly = True,
    default_visibility = [
        "//src/backend:__subpackages__",
        "//src/platform:__subpackages__",
        "//tests:__pkg__",
    ],
)
```

Run:

```bash
bazel test //src/backend/logging/testing:all
```

Expected: FAIL until the headers implement the tested API.

- [ ] **Step 3: Implement the canonical scripted protocol and recorder**

Merge the thread-safe blocking-poll behavior from
`tests/scripted_logging_protocol.h` with the queue/history fields from
`logging_use_case_test.cpp`. Preserve a non-responding default poll and a
successful default start/stop.

- [ ] **Step 4: Migrate consumers**

Update includes to:

```cpp
#include "src/backend/logging/testing/scripted_logging_protocol.h"
#include "src/backend/logging/testing/recording_logging_event_sink.h"
```

Replace the local classes in `logging_use_case_test.cpp`, retain its predicate
cancellation through `FakeCancellationToken`, and update method spellings.
Add the exact testing labels to the logging use-case target and the two
generated test-suite dependency entries.

Replace `test_logging_worker.cpp`'s local `NullDiagnostics` with the production
`fastecu::NullEventSink`; it is already the canonical no-op implementation and
is not a test double.

- [ ] **Step 5: Remove the old shared header wiring**

Delete `tests/scripted_logging_protocol.h`, remove it from
`MUT_DMA_TESTS_COMMON_HDRS`, and remove it from helper-header lists in
`bazel/mut_dma_test_suites.bzl`.

- [ ] **Step 6: Run dedicated and consumer tests**

```bash
bazel test \
  //src/backend/logging/testing:all \
  //src/backend/logging:logging_use_case_test \
  //tests:test_logging_engine \
  //tests:test_logging_worker
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/backend/logging tests/test_logging_engine.cpp \
  tests/test_logging_worker.cpp tests/scripted_logging_protocol.h \
  bazel/mut_dma_test_suites.bzl bazel/test_sources.bzl
git commit -m "test: move logging doubles to owner package"
```

### Task 5: Package-own backend protocol doubles

**Files:**
- Create: `src/backend/protocol/testing/BUILD.bazel`
- Move: `tests/scripted_can_transport.h` to `src/backend/protocol/testing/scripted_can_transport.h`
- Move: `tests/scripted_kline_transport.h` to `src/backend/protocol/testing/scripted_kline_transport.h`
- Move: `tests/scripted_ssm_transport.h` to `src/backend/protocol/testing/scripted_ssm_transport.h`
- Create: `src/backend/protocol/testing/scripted_mut_dma_init.h`
- Create: four corresponding `*_test.cpp` files
- Modify: protocol consumer tests
- Modify: `bazel/mut_dma_test_suites.bzl`
- Modify: `bazel/test_sources.bzl`

**Interfaces:**
- Preserve the three transport classes' existing public methods exactly.
- Produce `mutdma::ScriptedMutDmaInit`:
  - `Status result`
  - `int call_count`
  - `Status initialize(IKlineTransport&, const ICancellationToken&) override`

- [ ] **Step 1: Move transport headers and write independent contract tests**

For each transport, test:

- matching write succeeds and advances script;
- mismatched write returns `Internal` and sets `ok()` false where supported;
- queued frame/no-frame/error is returned in order;
- cancelled read returns `Cancelled`;
- exhausted read returns `Internal`;
- `scriptConsumed()` reflects unread/unwritten entries.

Example:

```cpp
TEST(ScriptedCanTransport, RejectsUnexpectedWrite)
{
    cdbg::ScriptedCanTransport transport;
    transport.expectWrite(0x700, bytes::Bytes{0x01});
    auto result = transport.write(0x701, bytes::Bytes{0x01});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_FALSE(transport.ok());
}
```

- [ ] **Step 2: Add and test `ScriptedMutDmaInit`**

Use a configurable `result`, increment `call_count` on every invocation, and
do not inspect or mutate the transport.

```cpp
TEST(ScriptedMutDmaInit, RecordsAndReturnsConfiguredResult)
{
    ScriptedMutDmaInit init;
    init.result = fail(ErrorKind::BadResponse, "init failed");
    // Invoke with the package-owned K-line transport and active token.
    auto result = init.initialize(transport, cancellation);
    ASSERT_FALSE(result);
    EXPECT_EQ(init.call_count, 1);
}
```

- [ ] **Step 3: Add package-level test-only Bazel targets**

Create one library and one test per header, with the same package-level
visibility pattern as logging. Run:

```bash
bazel test //src/backend/protocol/testing:all
```

Expected: PASS after the minimal implementations are present.

- [ ] **Step 4: Migrate includes and exact dependencies**

Update `test_cdbg_driver.cpp`, `test_cdbg_logging_protocol.cpp`,
`test_driver.cpp`, `test_init.cpp`, `test_mut_dma_logging_protocol.cpp`,
`test_ssm_logging_protocol.cpp`, and `test_transport.cpp` to include the
package-owned headers. Replace `test_driver.cpp`'s local `FailingInit` with
`ScriptedMutDmaInit`.

Remove scripted headers from `_MUT_DMA_GTEST_HELPER_HDRS` and
`MUT_DMA_TESTS_COMMON_HDRS`; add the exact library labels to each suite's
`SUITE_DEPS`.

- [ ] **Step 5: Run protocol-double and consumer tests**

```bash
bazel test \
  //src/backend/protocol/testing:all \
  //tests:test_cdbg_driver \
  //tests:test_cdbg_logging_protocol \
  //tests:test_driver \
  //tests:test_init \
  //tests:test_mut_dma_logging_protocol \
  //tests:test_ssm_logging_protocol \
  //tests:test_transport
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/backend/protocol/testing tests bazel/mut_dma_test_suites.bzl \
  bazel/test_sources.bzl
git commit -m "test: move protocol doubles to owner package"
```

### Task 6: Package-own flash doubles

**Files:**
- Create: `src/backend/flash/testing/BUILD.bazel`
- Move: `tests/scripted_can_flash_transport.h` to `src/backend/flash/testing/scripted_can_flash_transport.h`
- Move: `tests/scripted_kline_flash_transport.h` to `src/backend/flash/testing/scripted_kline_flash_transport.h`
- Create: `src/backend/flash/testing/stub_flash_transport.h`
- Create: `src/backend/flash/testing/scripted_flash_executor.h`
- Create: four corresponding `*_test.cpp` files
- Modify: `src/backend/flash/eeprom/denso_sh705x_eeprom_{can,kline}_executor_test.cpp`
- Modify: `src/backend/flash/eeprom/BUILD.bazel`
- Modify: flash worker and EEPROM dialog tests
- Modify: `bazel/mut_dma_test_suites.bzl`
- Modify: `tests/BUILD.bazel`

**Interfaces:**
- Preserve both scripted flash transports' existing public APIs.
- Produce `StubFlashTransport`:
  - `void request_unblock() noexcept override`
  - `bool unblock_requested() const`
- Produce `ScriptedFlashExecutor`:
  - `Result<FlashExecutionResult> result`
  - `int execute_call_count`
  - `std::function<void(const FlashPlan&)> on_execute`
  - `Result<FlashExecutionResult> execute(const FlashPlan&, IFlashTransport&, IClock&, const ICancellationToken&, IEventSink&) override`

- [ ] **Step 1: Move scripted transports and write dedicated tests**

For each transport cover configure/open/close results, configuration capture,
matching/mismatched writes, queued read/no-frame/error, cancellation,
blocking-read unblock, and `scriptConsumed()`.

```cpp
TEST(ScriptedKlineFlashTransport, BlockingReadEndsWhenUnblocked)
{
    ScriptedKlineFlashTransport transport;
    transport.queueBlockingRead();
    FakeCancellationToken active;
    std::future<Result<OptionalBytes>> read =
        std::async(std::launch::async, [&] { return transport.read(100, active); });
    transport.request_unblock();
    auto result = read.get();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
}
```

- [ ] **Step 2: Write tests for the base stub and executor**

```cpp
TEST(StubFlashTransport, RecordsUnblock)
{
    StubFlashTransport transport;
    EXPECT_FALSE(transport.unblock_requested());
    transport.request_unblock();
    EXPECT_TRUE(transport.unblock_requested());
}

TEST(ScriptedFlashExecutor, RecordsPlanAndReturnsConfiguredResult)
{
    ScriptedFlashExecutor executor;
    executor.result = fail(ErrorKind::Internal, "execute failed");
    const FlashPlan *seen_plan = nullptr;
    executor.on_execute = [&](const FlashPlan& value) { seen_plan = &value; };
    auto actual = executor.execute(plan, transport, clock, cancellation, events);
    ASSERT_FALSE(actual);
    EXPECT_EQ(executor.execute_call_count, 1);
    EXPECT_EQ(seen_plan, &plan);
}
```

- [ ] **Step 3: Add package-level targets and run dedicated tests**

```bash
bazel test //src/backend/flash/testing:all
```

Expected: PASS.

- [ ] **Step 4: Migrate executor and worker tests**

Use the package-owned CAN/K-line transports in both EEPROM executor tests and
`tests/test_flash_worker.cpp`. Replace the local `BareFlashTransport` with
`StubFlashTransport`. Update `src/backend/flash/eeprom/BUILD.bazel` and the
`test_flash_worker` suite dependency.

- [ ] **Step 5: Migrate EEPROM dialog test doubles**

Replace the local `NullTransport` and `ScriptedExecutor` classes in:

```text
tests/test_eeprom_ecu_subaru_denso_sh705x_can_dialog.cpp
tests/test_eeprom_ecu_subaru_denso_sh705x_kline_dialog.cpp
```

Configure `ScriptedFlashExecutor::result` per test and assert its recorded
call count. Configure `on_execute` to extract the CAN or K-line
`EepromReadMode` from each plan and append it to the dialog test's existing
`seenModes` vector, preserving the current observation across the fresh
executor created for each worker. Add only
`:stub_flash_transport` and `:scripted_flash_executor` to those suite deps.

- [ ] **Step 6: Remove top-level targets and headers**

Delete the two `tests/scripted_*_flash_transport.h` files and their
`cc_library` rules from `tests/BUILD.bazel`.

- [ ] **Step 7: Run dedicated and consumer tests**

```bash
bazel test \
  //src/backend/flash/testing:all \
  //src/backend/flash/eeprom:denso_sh705x_eeprom_can_executor_test \
  //src/backend/flash/eeprom:denso_sh705x_eeprom_kline_executor_test \
  //tests:test_flash_worker \
  //tests:test_eeprom_ecu_subaru_denso_sh705x_can_dialog \
  //tests:test_eeprom_ecu_subaru_denso_sh705x_kline_dialog
```

Expected: PASS on supported host platforms.

- [ ] **Step 8: Commit**

```bash
git add src/backend/flash/testing src/backend/flash/eeprom tests \
  bazel/mut_dma_test_suites.bzl
git commit -m "test: move flash doubles to owner package"
```

### Task 7: Remove remaining modern-interface doubles and verify full adherence

**Files:**
- Modify: any remaining consumer reported by the audit below
- Modify: affected Bazel files only when an exact dependency changes

**Interfaces:**
- Consumes: all package-owned doubles produced by Tasks 1-6
- Produces: no remaining consumer-local or top-level double for an in-scope interface

- [ ] **Step 1: Run the final inheritance audit**

```bash
rg -n --glob '*.{h,hpp,cc,cpp}' \
  '^\\s*(class|struct)\\s+[A-Za-z_][A-Za-z0-9_]*(\\s+final)?\\s*:\\s*(public|protected|private)\\s+' \
  src/backend src/algorithms tests
```

Classify every match by resolving its base declaration. A match is compliant
only if it is a production implementation, a non-double test fixture/helper,
or it resides under the owning modern package's `testing/` subpackage.
Specifically verify that no local `ICancellationToken`, `IClock`,
`IEventSink`, `IFileRepository`, `LoggingProtocol`, `ILoggingEventSink`,
backend transport, `IFlashTransport`, or `IFlashExecutor` implementation
remains.

- [ ] **Step 2: Replace any remaining in-scope local double**

For a missed interface, extend the already owning configurable double only
when a current consumer needs additional observable behavior. Add the failing
case to that double's dedicated test, run it to observe failure, implement the
minimum behavior, migrate the consumer, and rerun both targets. Do not create
a general enforcement rule.

- [ ] **Step 3: Verify every testing package and target shape**

```bash
rg -n 'default_testonly|name = ' \
  src/backend/ports/testing/BUILD.bazel \
  src/backend/logging/testing/BUILD.bazel \
  src/backend/protocol/testing/BUILD.bazel \
  src/backend/flash/testing/BUILD.bazel
```

Expected: every package has `default_testonly = True`; every header has one
same-named `cc_library` and one same-named `_test` `cc_test`.

- [ ] **Step 4: Verify no obsolete shared mock headers remain**

```bash
rg -n 'tests/scripted_|#include \"scripted_(logging|can|kline|ssm)' \
  src tests bazel
```

Expected: no matches for deleted top-level scripted headers.

- [ ] **Step 5: Run formatting and static checks**

```bash
prek run --all-files
git diff --check
```

Expected: PASS with no formatting or whitespace errors.

- [ ] **Step 6: Run all portable modern-source tests**

```bash
bazel test //src/backend/... //src/algorithms/...
```

Expected: PASS.

- [ ] **Step 7: Run the affected top-level tests**

```bash
bazel test \
  //tests:test_cdbg_driver \
  //tests:test_cdbg_logging_protocol \
  //tests:test_driver \
  //tests:test_init \
  //tests:test_logging_engine \
  //tests:test_logging_worker \
  //tests:test_mut_dma_logging_protocol \
  //tests:test_ssm_logging_protocol \
  //tests:test_transport \
  //tests:test_flash_snapshot_adapter \
  //tests:test_flash_worker
```

Run affected Qt/platform tests supported by the current host as well. Expected:
PASS.

- [ ] **Step 8: Review the final diff for scope**

```bash
git status --short
git diff --stat
git diff -- docs/adr/0008-use-package-owned-mocks.md
```

Expected: no ADR content change, no new enforcement mechanism, no legacy
interface migration, and no unrelated file modification.

- [ ] **Step 9: Commit final migration cleanup**

```bash
git add src/backend src/algorithms tests bazel
git commit -m "test: complete package-owned mocks migration"
```
