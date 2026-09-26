# Step 6g: Diagnostic Tools Off the Serial Facade — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the BIU, DTC, and DataTerminal dialogs from `serial_port_actions.h` onto a backend-owned `IDiagnosticLink` port, move the uncalled MUT memory helpers into a portable backend target, and delete `hexcommander`, removing `//src/ui/desktop/biu` from the `serial_qt_compat` allowlist and the GRANDFATHERED UI edge into the `transport` package.

**Architecture:** A Qt-free `IDiagnosticLink` port lives in `src/backend/protocol`. A platform adapter, `SerialDiagnosticLink`, in a new `//src/platform/desktop/common/diagnostics` package, implements it over the facade through `serial_platform_api`. `MainWindow` constructs the adapter and passes the dialogs an `IDiagnosticLink&`. DTC protocol logic becomes a portable `run_dtc_session` in a new `//src/backend/diagnostics` package, run on a `DtcWorker` thread. BIU and DataTerminal stay synchronous on the UI thread.

**Tech Stack:** C++23 (`std::expected`, `std::format`), Qt 6, Bazel 9 (Bzlmod), GoogleTest/GoogleMock, QtTest.

**Spec:** [docs/superpowers/specs/2026-09-26-step6g-diagnostic-tools-design.md](../specs/2026-09-26-step6g-diagnostic-tools-design.md). Read it before starting. The "Current state" section records today's exact wire behavior; this plan reproduces it.

## Global Constraints

- Bazel is the only build graph; every build/test command uses `--config=release`.
- Backend code returns `fastecu::Result<T>` / `fastecu::Status`, checked with `.has_value()`, never implicit `operator bool`. Exceptions never cross a port.
- No new `ErrorKind` value. Setter `false` → `InvalidConfig`; empty `open_serial_port`, non-zero `fast_init`/`set_j2534_ioctl`, null facade → `Disconnected`; cancelled read → `Cancelled`; NRC, short frame, wrong response ID → `BadResponse`.
- Pure protocol logic uses `bytes::Byte` / `bytes::Bytes` / `bytes::ByteView` (`src/algorithms/protocol/bytes.h`); `QByteArray` only at the boundary via `src/algorithms/protocol/qt_compat/qt_bytes.h`.
- `//:serial_compat_allowlist` may only shrink. Never add an entry to `serial_qt_compat`'s visibility or to `FROZEN`.
- New portable targets are registered in `PORTABLE_PACKAGES` (`bazel/portable_targets.bzl`).
- The MUT write guard `0x4000–0xBFFF` must not be relaxed.
- Tests are co-located (`foo.cpp` + `foo_test.cpp`); use `fastecu_portable_gtest` for Qt-free tests and `fastecu_qttest` for QtTest suites. A QtTest `main` that uses Google Mock must return non-zero on `::testing::Test::HasFailure()`.
- Platform guards, if ever unavoidable, are spelled `_WIN32`.
- Markdown cross-references are links, not backticked paths.
- Every PR passes: `bazel test --config=release //...`, `prek run --all-files`, `bazel run //:clang_tidy_report_changed`.
- Work lands as a `gh stack` of five PRs (branches listed per chunk). Commit messages end with the session's attribution trailer.

## Review Focus

1. **Five-baud response shorter than the offsets it inspects** (J2534 needs 10 bytes, direct needs 3) — expect a clean "five baud init failed" and `BadResponse`, never an out-of-range read. Pinned in Task 8 (`five_baud_header` short cases) and Task 9 (`shortFiveBaudResponseFailsCleanly`).
2. **Closing the DTC dialog mid-run** — expect Close to return within a sleep slice, the session epilogue (`set_header None`, `reset`) to run, then the dialog's own `reset`. Pinned in Task 11.
3. **Adapter opens nothing (empty `open_serial_port`)** — expect `Disconnected`, the epilogue still runs, and no write reaches the link. Pinned in Task 3 and Task 9 (`openFailureEndsRunAfterEpilogue`).
4. **Frame shorter than response index + 1 during a PID request** — expect "Wrong response from ECU" and the loop to stop, not an out-of-range PID read. Pinned in Task 8 (`check_response` short-PID case).
5. **MUT write at the guard boundaries** `0x3FFF`/`0xC000` — expect `InvalidConfig` with zero transport writes. Pinned in Task 4.

---

## Chunk 0 — PR 1: spec and plan (branch `docs/step6g-diagnostic-tools-spec`)

### Task 1: Land the spec and plan

**Files:**
- Already present: `docs/superpowers/specs/2026-09-26-step6g-diagnostic-tools-design.md`, `docs/superpowers/plans/2026-09-26-step6g-diagnostic-tools.md`

- [ ] **Step 1: Run the link and format hooks**

Run: `prek run --all-files`
Expected: all hooks Passed or Skipped.

- [ ] **Step 2: Commit the plan and spec corrections**

```bash
git add docs/superpowers
git commit -m "docs: step 6g implementation plan and spec corrections"
```

---

## Chunk 1 — PR 2: 6g-1 port, adapter, MUT memory, dead code (branch `refactor/step6g-1-diagnostic-link`, stacked on the spec branch)

### Task 2: `IDiagnosticLink` port and `FakeDiagnosticLink`

**Files:**
- Create: `src/backend/protocol/idiagnostic_link.h`
- Modify: `src/backend/protocol/BUILD.bazel` (add header to `:protocol` `hdrs`)
- Create: `src/backend/protocol/testing/fake_diagnostic_link.h`
- Create: `src/backend/protocol/testing/fake_diagnostic_link_test.cpp`
- Modify: `src/backend/protocol/testing/BUILD.bazel`

**Interfaces:**
- Produces: namespace `fastecu::diagnostics` — `enum class KlineHeader`, `to_string(KlineHeader)`, `struct KlineLinkConfig`, `struct CanLinkConfig`, `class IDiagnosticLink`; test double `fastecu::diagnostics::FakeDiagnosticLink` with public `calls`, `j2534`, `queue_open`, `queue_five_baud`, `queue_fast_init`, `queue_read`, `queue_no_frame`, `queue_read_error`, `script_consumed()`.

- [ ] **Step 1: Write the port header**

`src/backend/protocol/idiagnostic_link.h`:

```cpp
#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fastecu::diagnostics
{

// Which K-Line header the adapter adds to outgoing frames.
enum class KlineHeader
{
    None,
    Ssm,
    Iso9141,
    Iso14230,
};

constexpr std::string_view to_string(KlineHeader header) noexcept
{
    switch (header)
    {
    case KlineHeader::None:
        return "None";
    case KlineHeader::Ssm:
        return "Ssm";
    case KlineHeader::Iso9141:
        return "Iso9141";
    case KlineHeader::Iso14230:
        return "Iso14230";
    }
    return "None";
}

struct KlineLinkConfig
{
    KlineHeader header = KlineHeader::None;
    bool iso14230_connection = false;
    int baud = 10400;
    std::uint8_t start_byte = 0;
    std::uint8_t tester_id = 0;
    std::uint8_t target_id = 0;
};

struct CanLinkConfig
{
    bool iso15765 = true; // false: raw CAN
    int bitrate = 500000;
    bool extended_id = false; // 29-bit identifiers
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
};

// The diagnostic tools' view of the adapter: byte-faithful and thin. Bytes
// pass through exactly as the adapter expects them -- CAN writes carry their
// 4-byte ID prefix, headers are added only when set_header() asked for one.
class IDiagnosticLink
{
  public:
    using OptionalBytes = std::optional<bytes::Bytes>;

    virtual ~IDiagnosticLink() = default;

    // Reset, apply every field of the config, open. Disconnected if the
    // adapter reports no opened port.
    virtual Status open(const KlineLinkConfig& config) = 0;
    virtual Status open(const CanLinkConfig& config) = 0;
    virtual Status reset() = 0;

    virtual Status set_header(KlineHeader header) = 0;
    virtual Status set_p1_max(std::chrono::milliseconds p1_max) = 0;
    // The raw adapter response, uninterpreted; empty when nothing came back.
    virtual Result<bytes::Bytes> five_baud_init(std::uint8_t address) = 0;
    virtual Status fast_init(bytes::ByteView wakeup) = 0;

    // Echo-checked write; returns what the adapter returned.
    virtual Result<bytes::Bytes> write(bytes::ByteView data) = 0;
    // A deadline is a successful empty optional.
    virtual Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) = 0;
    // The adapter's OBD-framed read (direct serial K-Line).
    virtual Result<OptionalBytes> read_obd(std::chrono::milliseconds timeout,
                                           const ICancellationToken& cancellation) = 0;

    virtual bool uses_j2534() const = 0;
};

} // namespace fastecu::diagnostics
```

- [ ] **Step 2: Register the header**

In `src/backend/protocol/BUILD.bazel`, add `"idiagnostic_link.h",` to `:protocol`'s `hdrs`, keeping the list sorted (it goes after `"ican_transport.h",`).

- [ ] **Step 3: Write the failing fake test**

`src/backend/protocol/testing/fake_diagnostic_link_test.cpp`:

```cpp
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::diagnostics::CanLinkConfig;
using fastecu::diagnostics::FakeDiagnosticLink;
using fastecu::diagnostics::KlineHeader;
using fastecu::diagnostics::KlineLinkConfig;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAre;
using ::testing::Optional;
using namespace std::chrono_literals;

TEST(FakeDiagnosticLink, RecordsEveryCallInOrder)
{
    FakeDiagnosticLink link;
    FakeCancellationToken token;
    ASSERT_THAT(link.open(KlineLinkConfig{.header = KlineHeader::Iso14230,
                                          .iso14230_connection = true,
                                          .baud = 10400,
                                          .start_byte = 0xC0,
                                          .tester_id = 0xF1,
                                          .target_id = 0x33}),
                IsOk());
    ASSERT_THAT(link.open(CanLinkConfig{.iso15765 = true,
                                        .bitrate = 500000,
                                        .extended_id = false,
                                        .source_id = 0x7E0,
                                        .destination_id = 0x7E8}),
                IsOk());
    ASSERT_THAT(link.set_header(KlineHeader::Iso9141), IsOk());
    ASSERT_THAT(link.set_p1_max(35ms), IsOk());
    ASSERT_THAT(link.five_baud_init(0x33), IsOk());
    ASSERT_THAT(link.fast_init(bytes::Bytes{0x81}), IsOk());
    ASSERT_THAT(link.write(bytes::Bytes{0x01, 0x00}), IsOk());
    ASSERT_THAT(link.read(200ms, token), IsOk());
    ASSERT_THAT(link.read_obd(200ms, token), IsOk());
    ASSERT_THAT(link.reset(), IsOk());

    EXPECT_THAT(link.calls,
                ElementsAre("open kline header=Iso14230 iso14230=true baud=10400 start=C0 tester=F1 target=33",
                            "open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "set_header Iso9141", "p1 35", "five_baud 33", "fast_init 81", "write 01 00", "read 200",
                            "read_obd 200", "reset"));
}

TEST(FakeDiagnosticLink, ServesQueuedOutcomesThenNoFrame)
{
    FakeDiagnosticLink link;
    FakeCancellationToken token;
    link.queue_read(bytes::Bytes{0x41, 0x00});
    link.queue_read_error(ErrorKind::Disconnected);
    link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08});
    link.queue_fast_init(fastecu::fail(ErrorKind::Disconnected));
    link.queue_open(fastecu::fail(ErrorKind::Disconnected));

    EXPECT_THAT(link.read(200ms, token), IsOkAnd(Optional(ElementsAre(0x41, 0x00))));
    EXPECT_THAT(link.read_obd(200ms, token), IsErr(ErrorKind::Disconnected));
    EXPECT_THAT(link.read(200ms, token), IsOkAnd(std::nullopt));
    EXPECT_THAT(link.five_baud_init(0x33), IsOkAnd(ElementsAre(0x55, 0x08, 0x08)));
    EXPECT_THAT(link.five_baud_init(0x33), IsOkAnd(::testing::IsEmpty()));
    EXPECT_THAT(link.fast_init(bytes::Bytes{0x81}), IsErr(ErrorKind::Disconnected));
    EXPECT_THAT(link.open(KlineLinkConfig{}), IsErr(ErrorKind::Disconnected));
    EXPECT_TRUE(link.script_consumed());
}

TEST(FakeDiagnosticLink, ReadHonoursCancellation)
{
    FakeDiagnosticLink link;
    FakeCancellationToken token(true);
    link.queue_read(bytes::Bytes{0x41});
    EXPECT_THAT(link.read(200ms, token), IsErr(ErrorKind::Cancelled));
    EXPECT_FALSE(link.script_consumed());
}
```

Add to `src/backend/protocol/testing/BUILD.bazel` (add the `load` line at the top):

```python
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")

cc_library(
    name = "fake_diagnostic_link",
    testonly = True,
    hdrs = ["fake_diagnostic_link.h"],
    deps = ["//src/backend/protocol"],
)

fastecu_portable_gtest(
    name = "fake_diagnostic_link_test",
    srcs = ["fake_diagnostic_link_test.cpp"],
    deps = [
        ":fake_diagnostic_link",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:result_matchers",
    ],
)
```

- [ ] **Step 4: Run it to see it fail**

Run: `bazel test --config=release //src/backend/protocol/testing:fake_diagnostic_link_test`
Expected: FAIL — `fake_diagnostic_link.h` not found.

- [ ] **Step 5: Write the fake**

`src/backend/protocol/testing/fake_diagnostic_link.h`:

```cpp
#pragma once
#include "src/backend/protocol/idiagnostic_link.h"

#include <deque>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace fastecu::diagnostics
{

// Test double: records every call as a readable line in `calls`, and serves
// queued outcomes. An empty read queue answers "no frame"; empty open,
// five-baud and fast-init queues answer success / an empty response.
class FakeDiagnosticLink final : public IDiagnosticLink
{
  public:
    std::vector<std::string> calls;
    bool j2534 = false;

    void queue_open(Status outcome)
    {
        opens_.push_back(std::move(outcome));
    }
    void queue_five_baud(bytes::Bytes response)
    {
        five_bauds_.push_back(std::move(response));
    }
    void queue_fast_init(Status outcome)
    {
        fast_inits_.push_back(std::move(outcome));
    }
    void queue_read(bytes::Bytes frame)
    {
        reads_.emplace_back(OptionalBytes{std::move(frame)});
    }
    void queue_no_frame()
    {
        reads_.emplace_back(OptionalBytes{});
    }
    void queue_read_error(ErrorKind kind)
    {
        reads_.emplace_back(fail(kind, "scripted read error"));
    }
    bool script_consumed() const
    {
        return opens_.empty() && five_bauds_.empty() && fast_inits_.empty() && reads_.empty();
    }

    Status open(const KlineLinkConfig& c) override
    {
        calls.push_back(std::format("open kline header={} iso14230={} baud={} start={:02X} tester={:02X} target={:02X}",
                                    to_string(c.header), c.iso14230_connection, c.baud, c.start_byte, c.tester_id,
                                    c.target_id));
        return next(opens_);
    }
    Status open(const CanLinkConfig& c) override
    {
        calls.push_back(std::format("open can iso15765={} bitrate={} extended={} source={:03X} destination={:03X}",
                                    c.iso15765, c.bitrate, c.extended_id, c.source_id, c.destination_id));
        return next(opens_);
    }
    Status reset() override
    {
        calls.emplace_back("reset");
        return {};
    }
    Status set_header(KlineHeader header) override
    {
        calls.push_back(std::format("set_header {}", to_string(header)));
        return {};
    }
    Status set_p1_max(std::chrono::milliseconds p1_max) override
    {
        calls.push_back(std::format("p1 {}", p1_max.count()));
        return {};
    }
    Result<bytes::Bytes> five_baud_init(std::uint8_t address) override
    {
        calls.push_back(std::format("five_baud {:02X}", address));
        if (five_bauds_.empty())
        {
            return bytes::Bytes{};
        }
        auto response = std::move(five_bauds_.front());
        five_bauds_.pop_front();
        return response;
    }
    Status fast_init(bytes::ByteView wakeup) override
    {
        calls.push_back("fast_init " + hex(wakeup));
        return next(fast_inits_);
    }
    Result<bytes::Bytes> write(bytes::ByteView data) override
    {
        calls.push_back("write " + hex(data));
        return bytes::Bytes(data.begin(), data.end());
    }
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        calls.push_back(std::format("read {}", timeout.count()));
        return next_read(cancellation);
    }
    Result<OptionalBytes> read_obd(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        calls.push_back(std::format("read_obd {}", timeout.count()));
        return next_read(cancellation);
    }
    bool uses_j2534() const override
    {
        return j2534;
    }

    static std::string hex(bytes::ByteView data)
    {
        std::string out;
        for (const bytes::Byte b : data)
        {
            out += std::format("{}{:02X}", out.empty() ? "" : " ", b);
        }
        return out;
    }

  private:
    static Status next(std::deque<Status>& queue)
    {
        if (queue.empty())
        {
            return {};
        }
        auto outcome = std::move(queue.front());
        queue.pop_front();
        return outcome;
    }
    Result<OptionalBytes> next_read(const ICancellationToken& cancellation)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "scripted read cancelled");
        }
        if (reads_.empty())
        {
            return OptionalBytes{};
        }
        auto outcome = std::move(reads_.front());
        reads_.pop_front();
        return outcome;
    }

    std::deque<Status> opens_;
    std::deque<bytes::Bytes> five_bauds_;
    std::deque<Status> fast_inits_;
    std::deque<Result<OptionalBytes>> reads_;
};

} // namespace fastecu::diagnostics
```

- [ ] **Step 6: Run the test**

Run: `bazel test --config=release //src/backend/protocol/testing:fake_diagnostic_link_test //src/backend/protocol:all`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/backend/protocol
git commit -m "feat(protocol): IDiagnosticLink port and FakeDiagnosticLink (step 6g-1)"
```

### Task 3: `SerialDiagnosticLink` platform adapter

**Files:**
- Create: `src/platform/desktop/common/diagnostics/BUILD.bazel`
- Create: `src/platform/desktop/common/diagnostics/serial_diagnostic_link.h`
- Create: `src/platform/desktop/common/diagnostics/serial_diagnostic_link.cpp`
- Test: `src/platform/desktop/common/diagnostics/serial_diagnostic_link_test.cpp`

**Interfaces:**
- Consumes: `IDiagnosticLink`, `KlineLinkConfig`, `CanLinkConfig`, `KlineHeader` (Task 2).
- Produces: `fastecu::diagnostics::SerialDiagnosticLink(SerialPortActions *serial)`, target `//src/platform/desktop/common/diagnostics:serial_diagnostic_link`. Its header forward-declares `SerialPortActions`.

- [ ] **Step 1: Write the BUILD file**

`src/platform/desktop/common/diagnostics/BUILD.bazel`:

```python
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_DEPS", "fastecu_qttest", "qt_cc_library")

package(default_visibility = [
    "//apps/desktop:__pkg__",
    "//src/platform:__subpackages__",
    "//src/ui:__subpackages__",
    "//tests:__pkg__",
])

# IDiagnosticLink over the serial facade. Reaches serial_port_actions.h
# through serial_platform_api, as service_functions does, so the frozen
# serial_qt_compat allowlist does not grow. The header forward-declares
# SerialPortActions, so UI callers never see the facade header.
qt_cc_library(
    name = "serial_diagnostic_link",
    srcs = ["serial_diagnostic_link.cpp"],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["serial_diagnostic_link.h"],
    deps = QT_DEPS + [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/qt_compat",
        "//src/backend/ports",
        "//src/backend/protocol",
        "//src/platform/desktop/common/serial:serial_platform_api",
    ],
)

fastecu_qttest(
    name = "serial_diagnostic_link_test",
    src = "serial_diagnostic_link_test.cpp",
    deps = [
        ":serial_diagnostic_link",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/platform/desktop/common/serial/testing:fake_serial_backend",
        "//src/platform/desktop/common/transport:fake_backed_serial",
    ],
)
```

- [ ] **Step 2: Write the header**

`src/platform/desktop/common/diagnostics/serial_diagnostic_link.h`:

```cpp
#pragma once
#include "src/backend/protocol/idiagnostic_link.h"

class SerialPortActions;

namespace fastecu::diagnostics
{

// IDiagnosticLink over a non-owning SerialPortActions. The caller keeps the
// facade alive for the link's lifetime; MainWindow's facade outlives every
// diagnostic dialog.
class SerialDiagnosticLink final : public IDiagnosticLink
{
  public:
    explicit SerialDiagnosticLink(SerialPortActions *serial) : serial_(serial)
    {
    }

    Status open(const KlineLinkConfig& config) override;
    Status open(const CanLinkConfig& config) override;
    Status reset() override;
    Status set_header(KlineHeader header) override;
    Status set_p1_max(std::chrono::milliseconds p1_max) override;
    Result<bytes::Bytes> five_baud_init(std::uint8_t address) override;
    Status fast_init(bytes::ByteView wakeup) override;
    Result<bytes::Bytes> write(bytes::ByteView data) override;
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override;
    Result<OptionalBytes> read_obd(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override;
    bool uses_j2534() const override;

  private:
    SerialPortActions *serial_;
};

} // namespace fastecu::diagnostics
```

- [ ] **Step 3: Write the failing tests**

`src/platform/desktop/common/diagnostics/serial_diagnostic_link_test.cpp`:

```cpp
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"

#include <QCoreApplication>
#include <QTest>

#include <gmock/gmock.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/platform/desktop/common/serial/serial_facade_codes.h"
#include "src/platform/desktop/common/serial/testing/fake_backend.h"
#include "src/platform/desktop/common/transport/fake_backed_serial.h"

using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::diagnostics::CanLinkConfig;
using fastecu::diagnostics::KlineHeader;
using fastecu::diagnostics::KlineLinkConfig;
using fastecu::diagnostics::SerialDiagnosticLink;
using ::testing::InSequence;
using ::testing::Return;
using namespace std::chrono_literals;

class TestSerialDiagnosticLink : public QObject
{
    Q_OBJECT

  private slots:

    void klineOpenResetsAppliesEverySetterThenOpens()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), reset_connection());
            EXPECT_CALL(serial.fake(), set_is_iso14230_connection(true)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_ssm_header(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_iso9141_header(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_iso14230_header(true)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_serial_port_baudrate(QString("10400"))).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_kline_startbyte(0xC0)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_kline_tester_id(0xF1)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_kline_target_id(0x33)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_can_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_29_bit_id(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("ttyUSB0")));
        }
        SerialDiagnosticLink link(serial.get());
        QVERIFY(link.open(KlineLinkConfig{.header = KlineHeader::Iso14230,
                                          .iso14230_connection = true,
                                          .baud = 10400,
                                          .start_byte = 0xC0,
                                          .tester_id = 0xF1,
                                          .target_id = 0x33})
                    .has_value());
    }

    void canOpenResetsAppliesEverySetterThenOpens()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), reset_connection());
            EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_ssm_header(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_iso9141_header(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_can_connection(true)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_iso15765_connection(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_is_29_bit_id(true)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_can_speed(QString("250000"))).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_iso15765_source_address(0x7E0U)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_iso15765_destination_address(0x7E8U)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString("j2534")));
        }
        SerialDiagnosticLink link(serial.get());
        QVERIFY(link.open(CanLinkConfig{.iso15765 = false,
                                        .bitrate = 250000,
                                        .extended_id = true,
                                        .source_id = 0x7E0,
                                        .destination_id = 0x7E8})
                    .has_value());
    }

    void failingSetterIsInvalidConfigAndStopsTheSequence()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), set_is_iso14230_connection(false)).WillOnce(Return(false));
        EXPECT_CALL(serial.fake(), set_serial_port_baudrate(::testing::_)).Times(0);
        EXPECT_CALL(serial.fake(), open_serial_port()).Times(0);
        SerialDiagnosticLink link(serial.get());
        const auto result = link.open(KlineLinkConfig{});
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::InvalidConfig);
    }

    void emptyOpenedPortIsDisconnected()
    {
        FakeBackedSerial serial;
        ON_CALL(serial.fake(), set_is_iso14230_connection(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_add_ssm_header(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_add_iso9141_header(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_add_iso14230_header(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_serial_port_baudrate(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_kline_startbyte(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_kline_tester_id(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_kline_target_id(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_is_can_connection(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_is_iso15765_connection(::testing::_)).WillByDefault(Return(true));
        ON_CALL(serial.fake(), set_is_29_bit_id(::testing::_)).WillByDefault(Return(true));
        EXPECT_CALL(serial.fake(), open_serial_port()).WillOnce(Return(QString()));
        SerialDiagnosticLink link(serial.get());
        const auto result = link.open(KlineLinkConfig{});
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Disconnected);
    }

    void setHeaderSetsAllThreeFlags()
    {
        FakeBackedSerial serial;
        {
            InSequence order;
            EXPECT_CALL(serial.fake(), set_add_ssm_header(false)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_iso9141_header(true)).WillOnce(Return(true));
            EXPECT_CALL(serial.fake(), set_add_iso14230_header(false)).WillOnce(Return(true));
        }
        SerialDiagnosticLink link(serial.get());
        QVERIFY(link.set_header(KlineHeader::Iso9141).has_value());
    }

    void p1UsesTheJ2534IoctlOnOpenPort()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillRepeatedly(Return(true));
        EXPECT_CALL(serial.fake(), set_j2534_ioctl(kJ2534IoctlP1Max, 35)).WillOnce(Return(STATUS_SUCCESS));
        EXPECT_CALL(serial.fake(), set_kline_timings(::testing::_, ::testing::_)).Times(0);
        SerialDiagnosticLink link(serial.get());
        QVERIFY(link.set_p1_max(35ms).has_value());
    }

    void p1UsesKlineTimingsOnDirectSerial()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), get_use_openport2_adapter()).WillRepeatedly(Return(false));
        EXPECT_CALL(serial.fake(), set_kline_timings(SERIAL_P1_MAX, 25)).WillOnce(Return(true));
        EXPECT_CALL(serial.fake(), set_j2534_ioctl(::testing::_, ::testing::_)).Times(0);
        SerialDiagnosticLink link(serial.get());
        QVERIFY(link.set_p1_max(25ms).has_value());
    }

    void initCallsPassBytesThrough()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), five_baud_init(QByteArray::fromHex("33")))
            .WillOnce(Return(QByteArray::fromHex("550808")));
        EXPECT_CALL(serial.fake(), fast_init(QByteArray::fromHex("81"))).WillOnce(Return(STATUS_ERROR));
        SerialDiagnosticLink link(serial.get());
        const auto response = link.five_baud_init(0x33);
        QVERIFY(response.has_value());
        QCOMPARE(response->size(), std::size_t{3});
        const auto fast = link.fast_init(bytes::Bytes{0x81});
        QVERIFY(!fast.has_value());
        QCOMPARE(fast.error().kind, ErrorKind::Disconnected);
    }

    void writeIsEchoCheckedAndReadsSelectTheFacadeCall()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), write_serial_data_echo_check(QByteArray::fromHex("0100")))
            .WillOnce(Return(QByteArray::fromHex("0100")));
        EXPECT_CALL(serial.fake(), read_serial_data(200)).WillOnce(Return(QByteArray::fromHex("4100")));
        EXPECT_CALL(serial.fake(), read_serial_obd_data(200)).WillOnce(Return(QByteArray()));
        SerialDiagnosticLink link(serial.get());
        FakeCancellationToken token;
        QVERIFY(link.write(bytes::Bytes{0x01, 0x00}).has_value());
        const auto frame = link.read(200ms, token);
        QVERIFY(frame.has_value() && frame->has_value());
        QCOMPARE((*frame)->size(), std::size_t{2});
        const auto none = link.read_obd(200ms, token);
        QVERIFY(none.has_value() && !none->has_value());
    }

    void cancelledReadNeverReachesTheFacade()
    {
        FakeBackedSerial serial;
        EXPECT_CALL(serial.fake(), read_serial_data(::testing::_)).Times(0);
        SerialDiagnosticLink link(serial.get());
        FakeCancellationToken token(true);
        const auto result = link.read(200ms, token);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error().kind, ErrorKind::Cancelled);
    }

    void nullFacadeIsDisconnected()
    {
        SerialDiagnosticLink link(nullptr);
        QCOMPARE(link.open(KlineLinkConfig{}).error().kind, ErrorKind::Disconnected);
        QCOMPARE(link.write(bytes::Bytes{0x01}).error().kind, ErrorKind::Disconnected);
        QVERIFY(!link.uses_j2534());
    }
};

int main(int argc, char **argv)
{
    ::testing::InitGoogleMock(&argc, argv);
    QCoreApplication application(argc, argv);
    TestSerialDiagnosticLink test;
    const int result = QTest::qExec(&test, argc, argv);
    // QtTest does not include Google Mock failures in its exit status.
    return result != 0 || ::testing::Test::HasFailure() ? 1 : 0;
}
#include "serial_diagnostic_link_test.moc"
```

- [ ] **Step 4: Run it to see it fail**

Run: `bazel test --config=release //src/platform/desktop/common/diagnostics:serial_diagnostic_link_test`
Expected: FAIL — link error, `SerialDiagnosticLink::open` undefined.

- [ ] **Step 5: Implement the adapter**

`src/platform/desktop/common/diagnostics/serial_diagnostic_link.cpp`:

```cpp
#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"

#include <QString>

#include <exception>
#include <functional>
#include <initializer_list>
#include <string>

#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/ports/duration_cast.h"
#include "src/platform/desktop/common/serial/serial_facade_codes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::diagnostics
{
namespace
{

struct Setter
{
    const char *name;
    std::function<bool()> call;
};

// Setters only record state (serial_port_actions_direct.h), so a false return
// is a configuration problem, never a lost adapter. Stops at the first
// failure so later setters are not reached.
Status run_setters(std::initializer_list<Setter> setters)
{
    for (const Setter& setter : setters)
    {
        if (!setter.call())
        {
            return fail(ErrorKind::InvalidConfig, std::string(setter.name) + " failed");
        }
    }
    return {};
}

template <class F> auto guarded(F&& body) -> decltype(body())
{
    try
    {
        return body();
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "diagnostic link driver exception");
    }
}

Status no_facade()
{
    return fail(ErrorKind::Disconnected, "no serial facade");
}

} // namespace

Status SerialDiagnosticLink::open(const KlineLinkConfig& c)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            serial_->reset_connection();
            if (auto applied = run_setters({
                    {"set_is_iso14230_connection",
                     [&] { return serial_->set_is_iso14230_connection(c.iso14230_connection); }},
                    {"set_add_ssm_header", [&] { return serial_->set_add_ssm_header(c.header == KlineHeader::Ssm); }},
                    {"set_add_iso9141_header",
                     [&] { return serial_->set_add_iso9141_header(c.header == KlineHeader::Iso9141); }},
                    {"set_add_iso14230_header",
                     [&] { return serial_->set_add_iso14230_header(c.header == KlineHeader::Iso14230); }},
                    {"set_serial_port_baudrate",
                     [&] { return serial_->set_serial_port_baudrate(QString::number(c.baud)); }},
                    {"set_kline_startbyte", [&] { return serial_->set_kline_startbyte(c.start_byte); }},
                    {"set_kline_tester_id", [&] { return serial_->set_kline_tester_id(c.tester_id); }},
                    {"set_kline_target_id", [&] { return serial_->set_kline_target_id(c.target_id); }},
                    {"set_is_can_connection", [&] { return serial_->set_is_can_connection(false); }},
                    {"set_is_iso15765_connection", [&] { return serial_->set_is_iso15765_connection(false); }},
                    {"set_is_29_bit_id", [&] { return serial_->set_is_29_bit_id(false); }},
                });
                !applied.has_value())
            {
                return applied;
            }
            if (serial_->open_serial_port().isEmpty())
            {
                return fail(ErrorKind::Disconnected, "adapter did not open a port");
            }
            return {};
        });
}

Status SerialDiagnosticLink::open(const CanLinkConfig& c)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            serial_->reset_connection();
            if (auto applied = run_setters({
                    {"set_is_iso14230_connection", [&] { return serial_->set_is_iso14230_connection(false); }},
                    {"set_add_ssm_header", [&] { return serial_->set_add_ssm_header(false); }},
                    {"set_add_iso9141_header", [&] { return serial_->set_add_iso9141_header(false); }},
                    {"set_add_iso14230_header", [&] { return serial_->set_add_iso14230_header(false); }},
                    {"set_is_can_connection", [&] { return serial_->set_is_can_connection(!c.iso15765); }},
                    {"set_is_iso15765_connection", [&] { return serial_->set_is_iso15765_connection(c.iso15765); }},
                    {"set_is_29_bit_id", [&] { return serial_->set_is_29_bit_id(c.extended_id); }},
                    {"set_can_speed", [&] { return serial_->set_can_speed(QString::number(c.bitrate)); }},
                    {"set_iso15765_source_address",
                     [&] { return serial_->set_iso15765_source_address(c.source_id); }},
                    {"set_iso15765_destination_address",
                     [&] { return serial_->set_iso15765_destination_address(c.destination_id); }},
                });
                !applied.has_value())
            {
                return applied;
            }
            if (serial_->open_serial_port().isEmpty())
            {
                return fail(ErrorKind::Disconnected, "adapter did not open a port");
            }
            return {};
        });
}

Status SerialDiagnosticLink::reset()
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            serial_->reset_connection();
            return {};
        });
}

Status SerialDiagnosticLink::set_header(KlineHeader header)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            return run_setters({
                {"set_add_ssm_header", [&] { return serial_->set_add_ssm_header(header == KlineHeader::Ssm); }},
                {"set_add_iso9141_header",
                 [&] { return serial_->set_add_iso9141_header(header == KlineHeader::Iso9141); }},
                {"set_add_iso14230_header",
                 [&] { return serial_->set_add_iso14230_header(header == KlineHeader::Iso14230); }},
            });
        });
}

Status SerialDiagnosticLink::set_p1_max(std::chrono::milliseconds p1_max)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            const int value = saturating_ms<int>(p1_max);
            if (serial_->get_use_openport2_adapter())
            {
                if (serial_->set_j2534_ioctl(kJ2534IoctlP1Max, value) != STATUS_SUCCESS)
                {
                    return fail(ErrorKind::Disconnected, "set_j2534_ioctl(P1_MAX) failed");
                }
                return {};
            }
            if (!serial_->set_kline_timings(SERIAL_P1_MAX, value))
            {
                return fail(ErrorKind::InvalidConfig, "set_kline_timings(P1_MAX) failed");
            }
            return {};
        });
}

Result<bytes::Bytes> SerialDiagnosticLink::five_baud_init(std::uint8_t address)
{
    if (serial_ == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }
    return guarded([&]() -> Result<bytes::Bytes>
                   { return bytes::fromQByteArray(serial_->five_baud_init(QByteArray(1, static_cast<char>(address)))); });
}

Status SerialDiagnosticLink::fast_init(bytes::ByteView wakeup)
{
    if (serial_ == nullptr)
    {
        return no_facade();
    }
    return guarded(
        [&]() -> Status
        {
            if (serial_->fast_init(bytes::toQByteArray(wakeup)) != STATUS_SUCCESS)
            {
                return fail(ErrorKind::Disconnected, "fast_init failed");
            }
            return {};
        });
}

Result<bytes::Bytes> SerialDiagnosticLink::write(bytes::ByteView data)
{
    if (serial_ == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }
    return guarded([&]() -> Result<bytes::Bytes>
                   { return bytes::fromQByteArray(serial_->write_serial_data_echo_check(bytes::toQByteArray(data))); });
}

namespace
{
template <class ReadCall>
Result<IDiagnosticLink::OptionalBytes> guarded_read(SerialPortActions *serial, const ICancellationToken& cancellation,
                                                    ReadCall read_call)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "diagnostic read cancelled before driver call");
    }
    if (serial == nullptr)
    {
        return fail(ErrorKind::Disconnected, "no serial facade");
    }
    return guarded(
        [&]() -> Result<IDiagnosticLink::OptionalBytes>
        {
            const QByteArray raw = read_call();
            if (cancellation.cancelled())
            {
                return fail(ErrorKind::Cancelled, "diagnostic read cancelled");
            }
            if (raw.isEmpty())
            {
                return IDiagnosticLink::OptionalBytes{};
            }
            return IDiagnosticLink::OptionalBytes{bytes::fromQByteArray(raw)};
        });
}
} // namespace

Result<IDiagnosticLink::OptionalBytes> SerialDiagnosticLink::read(std::chrono::milliseconds timeout,
                                                                  const ICancellationToken& cancellation)
{
    return guarded_read(serial_, cancellation,
                        [&] { return serial_->read_serial_data(saturating_ms<quint16>(timeout)); });
}

Result<IDiagnosticLink::OptionalBytes> SerialDiagnosticLink::read_obd(std::chrono::milliseconds timeout,
                                                                      const ICancellationToken& cancellation)
{
    return guarded_read(serial_, cancellation,
                        [&] { return serial_->read_serial_obd_data(saturating_ms<quint16>(timeout)); });
}

bool SerialDiagnosticLink::uses_j2534() const
{
    try
    {
        return serial_ != nullptr && serial_->get_use_openport2_adapter();
    }
    catch (...)
    {
        return false;
    }
}

} // namespace fastecu::diagnostics
```

- [ ] **Step 6: Run the tests**

Run: `bazel test --config=release //src/platform/desktop/common/diagnostics:all`
Expected: PASS. If `saturating_ms<int>` is not instantiable for `int`, check `src/backend/ports/duration_cast.h` and use the integer type it supports for `set_j2534_ioctl`'s `int` parameter.

- [ ] **Step 7: Commit**

```bash
git add src/platform/desktop/common/diagnostics
git commit -m "feat(platform): SerialDiagnosticLink adapter over the serial facade (step 6g-1)"
```

### Task 4: Portable MUT memory helpers

**Files:**
- Create: `src/backend/protocol/mut_memory.h`, `src/backend/protocol/mut_memory.cpp`
- Test: `src/backend/protocol/mut_memory_test.cpp`
- Modify: `src/backend/protocol/BUILD.bazel`, `bazel/portable_targets.bzl`
- Modify: `src/ui/desktop/log_operations_ssm.cpp` (delete `mut_write_memory`, `mut_read_memory`, their comment block, the `mut_dma_memory.h` / `transport_legacy_compat.h` includes and `using namespace` lines if unused)
- Modify: `src/ui/desktop/mainwindow.h` (delete the two declarations and the `fastecu_kline_transport.h` include)
- Modify: `src/ui/desktop/BUILD.bazel` (drop `"//src/platform/desktop/common/transport"` from `:desktop` deps)
- Modify: `src/platform/desktop/common/transport/BUILD.bazel` (drop the GRANDFATHERED `//src/ui/desktop:__pkg__` default-visibility entry and its comment)

**Interfaces:**
- Produces: `mutdma::write_memory(IKlineTransport&, std::uint16_t, bytes::ByteView, const fastecu::ICancellationToken&) -> fastecu::Status`; `mutdma::read_memory(IKlineTransport&, std::uint16_t, std::size_t, const fastecu::ICancellationToken&) -> fastecu::Result<bytes::Bytes>`; target `//src/backend/protocol:mut_memory`.

- [ ] **Step 1: Write the failing tests**

`src/backend/protocol/mut_memory_test.cpp`:

```cpp
#include "src/backend/protocol/mut_memory.h"

#include <algorithm>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/testing/scripted_kline_transport.h"

using namespace mutdma;
using fastecu::ErrorKind;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAreArray;

namespace
{
// One read chunk: handshake for `len` one-byte channels, then a stream frame
// carrying `data`.
void script_chunk(ScriptedKlineTransport& t, std::uint16_t addr, const bytes::Bytes& data)
{
    const auto channels = planReadChannels(addr, static_cast<int>(data.size()));
    t.expectWrite(buildSetupFrame(0xA0, static_cast<bytes::Byte>(channels.size())));
    t.queueRead(buildCommandFrame(0xA5, bytes::Bytes{}, TRAILER_STD));
    t.expectWrite(buildIdListFrame(0xA1, channels));
    t.queueRead(buildCommandFrame(0x05, bytes::Bytes{}, TRAILER_STD));
    bytes::Bytes frame{0x51};
    frame.insert(frame.end(), data.begin(), data.end());
    frame.push_back(sum8(frame));
    frame.push_back(TRAILER_STD);
    t.queueRead(frame);
}

bytes::Bytes counting(std::size_t n, bytes::Byte first = 0)
{
    bytes::Bytes out(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        out[i] = static_cast<bytes::Byte>(first + i);
    }
    return out;
}
} // namespace

TEST(MutMemory, WriteBelowTheWindowIsRefusedWithoutIo)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    EXPECT_THAT(write_memory(t, 0x3FFF, bytes::Bytes{0x01}, token), IsErr(ErrorKind::InvalidConfig));
    EXPECT_TRUE(t.scriptConsumed());
}

TEST(MutMemory, WriteAboveTheWindowIsRefusedWithoutIo)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    EXPECT_THAT(write_memory(t, 0xC000, bytes::Bytes{0x01}, token), IsErr(ErrorKind::InvalidConfig));
    EXPECT_TRUE(t.scriptConsumed());
}

TEST(MutMemory, WriteAtBothWindowEdgesReachesTheDriver)
{
    for (const std::uint16_t addr : {std::uint16_t{0x4000}, std::uint16_t{0xBFFF}})
    {
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        const bytes::Bytes data{0xAB};
        t.expectWrite(buildWriteFrames(addr, data).at(0));
        t.queueRead(buildCommandFrame(0x87, bytes::Bytes{0x80, 0x00}, TRAILER_STD));
        EXPECT_THAT(write_memory(t, addr, data, token), IsOk()) << std::hex << addr;
        EXPECT_TRUE(t.scriptConsumed());
    }
}

TEST(MutMemory, ReadSplitsIntoFortyByteChunks)
{
    for (const std::size_t len : {std::size_t{39}, std::size_t{40}, std::size_t{41}, std::size_t{80}})
    {
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        const bytes::Bytes expected = counting(len);
        for (std::size_t off = 0; off < len; off += 40)
        {
            const std::size_t n = std::min<std::size_t>(40, len - off);
            script_chunk(t, static_cast<std::uint16_t>(0x8000 + off),
                         bytes::Bytes(expected.begin() + static_cast<std::ptrdiff_t>(off),
                                      expected.begin() + static_cast<std::ptrdiff_t>(off + n)));
        }
        EXPECT_THAT(read_memory(t, 0x8000, len, token), IsOkAnd(ElementsAreArray(expected))) << len;
        EXPECT_TRUE(t.scriptConsumed()) << len;
    }
}

TEST(MutMemory, ReadReturnsWhatItHadWhenALaterChunkFails)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    const bytes::Bytes first = counting(40);
    script_chunk(t, 0x8000, first);
    t.expectWrite(buildSetupFrame(0xA0, 40));
    t.queue_error(ErrorKind::Disconnected);
    EXPECT_THAT(read_memory(t, 0x8000, 80, token), IsOkAnd(ElementsAreArray(first)));
}

TEST(MutMemory, ReadFailsWhenTheFirstChunkFails)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    t.expectWrite(buildSetupFrame(0xA0, 4));
    t.queue_error(ErrorKind::Disconnected);
    EXPECT_THAT(read_memory(t, 0x8000, 4, token), IsErr(ErrorKind::Disconnected));
}

TEST(MutMemory, ReadSkipsAChunkWhosePollReturnsNoFrame)
{
    // Today's loop only stops on a poll *error*; an empty poll appends nothing
    // and moves to the next chunk.
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    const auto channels = planReadChannels(0x8000, 40);
    t.expectWrite(buildSetupFrame(0xA0, 40));
    t.queueRead(buildCommandFrame(0xA5, bytes::Bytes{}, TRAILER_STD));
    t.expectWrite(buildIdListFrame(0xA1, channels));
    t.queueRead(buildCommandFrame(0x05, bytes::Bytes{}, TRAILER_STD));
    t.queue_no_frame();
    const bytes::Bytes second = counting(10, 0x40);
    script_chunk(t, 0x8028, second);
    EXPECT_THAT(read_memory(t, 0x8000, 50, token), IsOkAnd(ElementsAreArray(second)));
}
```

Add to `src/backend/protocol/BUILD.bazel`:

```python
cc_library(
    name = "mut_memory",
    srcs = ["mut_memory.cpp"],
    hdrs = ["mut_memory.h"],
    deps = [
        ":protocol",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/mut_dma",
        "//src/backend/ports",
    ],
)

fastecu_portable_gtest(
    name = "mut_memory_test",
    srcs = ["mut_memory_test.cpp"],
    deps = [
        ":mut_memory",
        "//src/algorithms/protocol/mut_dma",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:result_matchers",
        "//src/backend/protocol/testing:scripted_transports",
    ],
)
```

In `bazel/portable_targets.bzl`, change `"src/backend/protocol": ["protocol"],` to `"src/backend/protocol": ["mut_memory", "protocol"],`.

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/backend/protocol:mut_memory_test`
Expected: FAIL — `mut_memory.h` not found.

- [ ] **Step 3: Implement**

`src/backend/protocol/mut_memory.h`:

```cpp
#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/ikline_transport.h"

#include <cstddef>
#include <cstdint>

namespace mutdma
{

// Standalone MUT/DMA memory access over an ECU already in MUT/DMA mode at
// 125000 baud. Moved from MainWindow (step 6g), where nothing called it.

// Refuses addresses outside the writable RAM window 0x4000-0xBFFF with
// InvalidConfig before any I/O. Do not relax the window.
fastecu::Status write_memory(IKlineTransport& transport, std::uint16_t addr, bytes::ByteView data,
                             const fastecu::ICancellationToken& cancellation);

// Reads in chunks of up to 40 bytes. Returns what was read before the first
// failed chunk; fails only if the first chunk fails. A chunk whose poll yields
// no frame contributes nothing and the read continues.
fastecu::Result<bytes::Bytes> read_memory(IKlineTransport& transport, std::uint16_t addr, std::size_t len,
                                          const fastecu::ICancellationToken& cancellation);

} // namespace mutdma
```

`src/backend/protocol/mut_memory.cpp`:

```cpp
#include "src/backend/protocol/mut_memory.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"
#include "src/backend/protocol/imut_dma_init.h"
#include "src/backend/protocol/mut_dma_driver.h"

namespace mutdma
{
namespace
{
constexpr std::uint16_t kWritableLow = 0x4000;
constexpr std::uint16_t kWritableHigh = 0xBFFF;
constexpr int kMutBaud = 125000;
constexpr std::size_t kReadChunk = 40;
constexpr bytes::Byte kSetupCmd = 0xA0;
constexpr bytes::Byte kListCmd = 0xA1;
constexpr std::chrono::milliseconds kPollTimeout{50};
} // namespace

fastecu::Status write_memory(IKlineTransport& transport, std::uint16_t addr, bytes::ByteView data,
                             const fastecu::ICancellationToken& cancellation)
{
    if (addr < kWritableLow || addr > kWritableHigh)
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "MUT/DMA: refusing write outside 0x4000-0xBFFF");
    }
    AlreadyInMode init(kMutBaud);
    MutDmaDriver driver(transport, init);
    return driver.writeMemory(addr, data, cancellation);
}

fastecu::Result<bytes::Bytes> read_memory(IKlineTransport& transport, std::uint16_t addr, std::size_t len,
                                          const fastecu::ICancellationToken& cancellation)
{
    AlreadyInMode init(kMutBaud);
    MutDmaDriver driver(transport, init);
    bytes::Bytes out;
    for (std::size_t off = 0; off < len; off += kReadChunk)
    {
        const std::size_t chunk = std::min(kReadChunk, len - off);
        const std::vector<Channel> channels =
            planReadChannels(static_cast<std::uint16_t>(addr + off), static_cast<int>(chunk));
        if (auto started = driver.startFreeFormLog(channels, kSetupCmd, kListCmd, cancellation); !started.has_value())
        {
            if (off == 0)
            {
                return std::unexpected(started.error());
            }
            break;
        }
        auto values = driver.pollOnce(kPollTimeout, cancellation);
        if (!values.has_value())
        {
            if (off == 0)
            {
                return std::unexpected(values.error());
            }
            break;
        }
        const bytes::Bytes piece = reassembleRead(*values);
        out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
}

} // namespace mutdma
```

- [ ] **Step 4: Run the tests**

Run: `bazel test --config=release //src/backend/protocol:all`
Expected: PASS.

- [ ] **Step 5: Delete the MainWindow helpers and the transport edge**

- In `src/ui/desktop/log_operations_ssm.cpp`, delete the comment block beginning `// MUT/DMA memory read/write bench utilities.` and both functions `MainWindow::mut_write_memory` and `MainWindow::mut_read_memory`. Then delete `#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"`, `#include "src/backend/protocol/transport_legacy_compat.h"`, and `using namespace mutdma;` / `using namespace std::chrono_literals;` if the file still compiles without them (try removing, build; restore any the build needs).
- In `src/ui/desktop/mainwindow.h`, delete the declarations `bool mut_write_memory(quint16 addr, const QByteArray& bytes);` and `QByteArray mut_read_memory(quint16 addr, int len);`, and the line `#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"`.
- In `src/ui/desktop/BUILD.bazel`, delete `"//src/platform/desktop/common/transport",` from `:desktop`'s deps.
- In `src/platform/desktop/common/transport/BUILD.bazel`, delete the four-line `# GRANDFATHERED (Task 5 human decision)...` comment and the `"//src/ui/desktop:__pkg__",` entry from `package(default_visibility = ...)`.

Run: `grep -rn "common/transport/" src/ui/desktop --include='*.h' --include='*.cpp'`
Expected: no output.

- [ ] **Step 6: Build and test**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //src/ui/... //src/backend/protocol/... //src/platform/desktop/common/transport/... //:portable_closure`
Expected: build succeeds, all PASS. (`//:portable_closure` is a build-time check; if the label differs, run `bazel build --config=release //:portable_closure`.)

- [ ] **Step 7: Commit**

```bash
git add -A src/backend/protocol bazel/portable_targets.bzl src/ui/desktop src/platform/desktop/common/transport/BUILD.bazel
git commit -m "refactor(protocol): portable MUT memory helpers; drop the UI transport edge (step 6g-1)"
```

### Task 5: Delete `hexcommander`

**Files:**
- Delete: `src/ui/desktop/hexcommander.cpp`, `src/ui/desktop/hexcommander.h`
- Modify: `src/ui/desktop/BUILD.bazel` (delete the `# hexcommander.cpp is deliberately omitted...` comment on `:desktop`)

- [ ] **Step 1: Confirm nothing references it**

Run: `grep -rn "hexcommander\|HexCommander" src apps tests bazel scripts docs --include='*' | grep -v "^src/ui/desktop/hexcommander\."`
Expected: only the BUILD comment in `src/ui/desktop/BUILD.bazel` (and possibly historical docs; leave docs untouched).

- [ ] **Step 2: Delete and clean the comment**

```bash
git rm src/ui/desktop/hexcommander.cpp src/ui/desktop/hexcommander.h
```

Remove the four-line comment beginning `# hexcommander.cpp is deliberately omitted` from `:desktop` in `src/ui/desktop/BUILD.bazel`.

- [ ] **Step 3: Verify and commit**

Run: `bazel build --config=release //:fastecu && prek run --all-files`
Expected: success.

```bash
git add src/ui/desktop/BUILD.bazel
git commit -m "refactor(ui): delete dead HexCommander (step 6g-1)"
```

- [ ] **Step 4: Full gate for the PR**

Run: `bazel test --config=release //... && bazel run //:clang_tidy_report_changed`
Expected: all PASS; clang-tidy reports no findings on changed files.

---

## Chunk 2 — PR 3: 6g-2 BIU (branch `refactor/step6g-2-biu`, stacked on 6g-1)

### Task 6: BIU on the port; `//src/ui/desktop/biu` leaves the allowlist

**Files:**
- Create: `src/ui/desktop/diagnostic_link_io.h` (Qt boundary helpers shared by BIU and DataTerminal)
- Modify: `src/ui/desktop/BUILD.bazel` (new `:diagnostic_link_io` target; `:desktop` deps)
- Modify: `src/ui/desktop/biu/biu_operations_subaru.h`, `src/ui/desktop/biu/biu_operations_subaru.cpp`, `src/ui/desktop/biu/biu_ops_subaru_switches.h`, `src/ui/desktop/biu/BUILD.bazel`
- Modify: `src/ui/desktop/menu_actions.cpp` (`show_subaru_biu_window`)
- Modify: `src/platform/desktop/common/serial/BUILD.bazel` (visibility), `scripts/check-serial-compat-allowlist.py` (`FROZEN`)

**Interfaces:**
- Consumes: `IDiagnosticLink` (Task 2), `SerialDiagnosticLink` (Task 3).
- Produces: `diagnostic_link_io::read_or_empty(IDiagnosticLink&, std::uint16_t timeout_ms) -> QByteArray` and `diagnostic_link_io::write(IDiagnosticLink&, const QByteArray&)`; `BiuOperationsSubaru(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent = nullptr)`.

- [ ] **Step 1: Add the boundary helper**

`src/ui/desktop/diagnostic_link_io.h`:

```cpp
#pragma once
#include <QByteArray>

#include <chrono>
#include <cstdint>

#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/protocol/idiagnostic_link.h"
#include "src/backend/protocol/transport_legacy_compat.h"

// QByteArray boundary for the synchronous diagnostic dialogs (BIU,
// DataTerminal), which keep today's facade semantics: a failed or empty read
// is an empty array, and write results are ignored.
namespace diagnostic_link_io
{

inline QByteArray read_or_empty(fastecu::diagnostics::IDiagnosticLink& link, std::uint16_t timeout_ms)
{
    const auto frame = link.read(std::chrono::milliseconds{timeout_ms},
                                 fastecu::transport_legacy_compat::detail::never_cancelled());
    if (!frame.has_value() || !frame->has_value())
    {
        return {};
    }
    return bytes::toQByteArray(**frame);
}

inline void write(fastecu::diagnostics::IDiagnosticLink& link, const QByteArray& data)
{
    static_cast<void>(link.write(bytes::view(data)));
}

} // namespace diagnostic_link_io
```

Add to `src/ui/desktop/BUILD.bazel` (above `:desktop`):

```python
# QByteArray boundary for the synchronous diagnostic dialogs.
qt_cc_library(
    name = "diagnostic_link_io",
    srcs = [],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["diagnostic_link_io.h"],
    deps = QT_DEPS + [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/qt_compat",
        "//src/backend/protocol",
    ],
)
```

and add `":diagnostic_link_io",` and `"//src/platform/desktop/common/diagnostics:serial_diagnostic_link",` to `:desktop`'s deps.

- [ ] **Step 2: Move BIU onto the port**

In `src/ui/desktop/biu/biu_operations_subaru.h`:
- replace `class SerialPortActions;` (if present) with `#include "src/backend/protocol/idiagnostic_link.h"` among the includes;
- change the constructor to `explicit BiuOperationsSubaru(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent = nullptr);`;
- replace the member `SerialPortActions *serial;` with `fastecu::diagnostics::IDiagnosticLink *link = nullptr;`.

In `src/ui/desktop/biu/biu_ops_subaru_switches.h`, delete `class SerialPortActions;` and the commented `// SerialPortActions *serial;` line.

In `src/ui/desktop/biu/biu_operations_subaru.cpp`:
- replace `#include "src/platform/desktop/common/serial/serial_port_actions.h"` with `#include "src/ui/desktop/diagnostic_link_io.h"`;
- constructor signature `BiuOperationsSubaru::BiuOperationsSubaru(fastecu::diagnostics::IDiagnosticLink& link_arg, QWidget *parent)` and body line `this->serial = serial_arg;` → `this->link = &link_arg;`;
- in `send_biu_msg`, replace the three facade calls:

```cpp
    if (connection_state == NOT_CONNECTED && current_command == CONNECT)
    {
        static_cast<void>(link->fast_init(bytes::view(output)));
    }
    else
    {
        diagnostic_link_io::write(*link, output);
    }

    received = diagnostic_link_io::read_or_empty(*link, serial_read_long_timeout);
```

Run: `grep -n "serial" src/ui/desktop/biu/*.cpp src/ui/desktop/biu/*.h`
Expected: only `serial_read_*_timeout` members and unrelated words — no `serial->`, no `SerialPortActions`.

In `src/ui/desktop/biu/BUILD.bazel`, replace `"//src/platform/desktop/common/serial:serial_qt_compat",` with:

```python
        "//src/backend/protocol",
        "//src/ui/desktop:diagnostic_link_io",
```

- [ ] **Step 3: Construct the link in `MainWindow`**

In `src/ui/desktop/menu_actions.cpp`, add `#include "src/platform/desktop/common/diagnostics/serial_diagnostic_link.h"`, and in `show_subaru_biu_window` replace `BiuOperationsSubaru biuOperationsSubaru(serial, this);` with:

```cpp
    fastecu::diagnostics::SerialDiagnosticLink link(serial);
    BiuOperationsSubaru biuOperationsSubaru(link, this);
```

The preamble above it (reset, header flags, `open_serial_port()`, `change_port_speed`) stays unchanged — it is step 6h's connection work.

- [ ] **Step 4: Shrink the allowlist**

In `src/platform/desktop/common/serial/BUILD.bazel`, delete `"//src/ui/desktop/biu:__pkg__",` from `serial_qt_compat`'s `visibility`. In `scripts/check-serial-compat-allowlist.py`, delete `"//src/ui/desktop/biu:__pkg__",` from `FROZEN`.

Run: `bazel test --config=release //:serial_compat_allowlist`
Expected: PASS.

- [ ] **Step 5: Build, test, commit**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //... && prek run --all-files && bazel run //:clang_tidy_report_changed`
Expected: all PASS.

```bash
git add -A src/ui/desktop src/platform/desktop/common/serial/BUILD.bazel scripts/check-serial-compat-allowlist.py
git commit -m "refactor(ui): BIU talks to IDiagnosticLink; drop its serial_qt_compat entry (step 6g-2)"
```

---

## Chunk 3 — PR 4: 6g-3 DataTerminal (branch `refactor/step6g-3-data-terminal`, stacked on 6g-2)

### Task 7: DataTerminal on the port

**Files:**
- Modify: `src/ui/desktop/dataterminal.h`, `src/ui/desktop/dataterminal.cpp`, `src/ui/desktop/menu_actions.cpp` (`show_terminal_window`)

**Interfaces:**
- Consumes: `IDiagnosticLink`, `KlineLinkConfig`, `CanLinkConfig`, `KlineHeader` (Task 2); `diagnostic_link_io` (Task 6); `SerialDiagnosticLink` (Task 3).
- Produces: `DataTerminal(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent = nullptr)`.

- [ ] **Step 1: Change the header**

In `src/ui/desktop/dataterminal.h`: replace `class SerialPortActions;` with `#include "src/backend/protocol/idiagnostic_link.h"`; constructor `explicit DataTerminal(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent = nullptr);`; member `SerialPortActions *serial;` → `fastecu::diagnostics::IDiagnosticLink *link = nullptr;`.

- [ ] **Step 2: Change the source**

In `src/ui/desktop/dataterminal.cpp`:
- replace the `serial_port_actions.h` include with `#include "src/ui/desktop/diagnostic_link_io.h"`;
- constructor `DataTerminal::DataTerminal(fastecu::diagnostics::IDiagnosticLink& link_arg, QWidget *parent)`, body `this->link = &link_arg;`.

Replace the K-Line setup block — from `if (ui->klineProtocol->currentText() == "SSM")` through the `if (serialOk) { ... serial->open_serial_port(); }` block — with:

```cpp
        bool iso14230 = false;
        if (ui->klineProtocol->currentText() == "SSM")
        {
            iso14230 = false;
        }
        else if (ui->klineProtocol->currentText() == "iso14230")
        {
            iso14230 = true;
        }
        else
        {
            serialOk = false;
        }
        emit LOG_D("Checking baudrate: " + ui->klineBaudRate->text(), true, true);
        if (!(ui->klineBaudRate->text().toDouble() >= 300 && ui->klineBaudRate->text().toDouble() <= 2000000))
        {
            serialOk = false;
        }
        emit LOG_D("Checking tester id: " + ui->klineTesterId->text(), true, true);
        const auto tester = static_cast<std::uint8_t>(ui->klineTesterId->text().toUInt(&ok, 16));
        emit LOG_D("Checking target id: " + ui->klineTargetId->text(), true, true);
        const auto target = static_cast<std::uint8_t>(ui->klineTargetId->text().toUInt(&ok, 16));
        if (serialOk)
        {
            emit LOG_D("All good, setting interface...", true, true);
            emit LOG_D("Opening interface...", true, true);
            const auto opened = link->open(fastecu::diagnostics::KlineLinkConfig{
                .header = fastecu::diagnostics::KlineHeader::None,
                .iso14230_connection = iso14230,
                .baud = ui->klineBaudRate->text().toInt(),
                .start_byte = 0x80,
                .tester_id = tester,
                .target_id = target,
            });
            if (!opened.has_value())
            {
                emit LOG_E("Unable to open interface: " + QString::fromStdString(opened.error().detail), true, true);
            }
        }
```

Replace the CAN setup block — from `serial->set_is_can_connection(false);` through its `if (serialOk) { ... serial->open_serial_port(); }` — with:

```cpp
        bool iso15765 = false;
        if (ui->canProtocol->currentText() == "CAN")
        {
            iso15765 = false;
        }
        else if (ui->canProtocol->currentText() == "iso15765")
        {
            iso15765 = true;
        }
        else
        {
            serialOk = false;
        }
        emit LOG_D("Checking baudrate: " + ui->canBaudRate->text(), true, true);
        if (!(ui->canBaudRate->text().toDouble() >= 300 && ui->canBaudRate->text().toDouble() <= 2000000))
        {
            serialOk = false;
        }
        emit LOG_D("Checking CAN ID length: " + ui->canIdLength->currentText(), true, true);
        emit LOG_D("Checking tester id: " + ui->canTesterId->text(), true, true);
        const std::uint32_t source = ui->canTesterId->text().toUInt(&ok, 16);
        emit LOG_D("Checking target id: " + ui->canTargetId->text(), true, true);
        const std::uint32_t destination = ui->canTargetId->text().toUInt(&ok, 16);
        if (serialOk)
        {
            emit LOG_D("All good, setting interface...", true, true);
            emit LOG_D("Opening interface...", true, true);
            const auto opened = link->open(fastecu::diagnostics::CanLinkConfig{
                .iso15765 = iso15765,
                .bitrate = ui->canBaudRate->text().toInt(),
                .extended_id = ui->canIdLength->currentIndex() == 1,
                .source_id = source,
                .destination_id = destination,
            });
            if (!opened.has_value())
            {
                emit LOG_E("Unable to open interface: " + QString::fromStdString(opened.error().detail), true, true);
            }
        }
```

In both send loops, replace:

```cpp
            serial->write_serial_data_echo_check(output);
            delay(rspDelay);
            received = serial->read_serial_data(serial_read_short_timeout);
```

with:

```cpp
            diagnostic_link_io::write(*link, output);
            delay(rspDelay);
            received = diagnostic_link_io::read_or_empty(*link, serial_read_short_timeout);
```

and replace each trailing `serial->reset_connection();` with `static_cast<void>(link->reset());`.

Leave the `delay(...)` script-line parse unchanged (it yields 0 for `delay(100)`; the spec pins it). Add `#include <cstdint>` if not already present.

Run: `grep -n "serial->\|SerialPortActions" src/ui/desktop/dataterminal.*`
Expected: no output.

- [ ] **Step 3: Construct the link in `MainWindow`**

In `show_terminal_window` (`src/ui/desktop/menu_actions.cpp`), replace `DataTerminal hexCommander(serial, this);` with:

```cpp
    fastecu::diagnostics::SerialDiagnosticLink link(serial);
    DataTerminal hexCommander(link, this);
```

- [ ] **Step 4: Build, test, commit**

Run: `bazel build --config=release //:fastecu && bazel test --config=release //... && prek run --all-files && bazel run //:clang_tidy_report_changed`
Expected: all PASS.

```bash
git add src/ui/desktop
git commit -m "refactor(ui): DataTerminal talks to IDiagnosticLink (step 6g-3)"
```

---

## Chunk 4 — PR 5: 6g-4 DTC and close-out (branch `refactor/step6g-4-dtc`, stacked on 6g-3)

### Task 8: `obd_frames` pure helpers

**Files:**
- Create: `src/backend/diagnostics/BUILD.bazel`, `src/backend/diagnostics/obd_frames.h`, `src/backend/diagnostics/obd_frames.cpp`
- Test: `src/backend/diagnostics/obd_frames_test.cpp`
- Modify: `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: `KlineHeader` (Task 2).
- Produces (namespace `fastecu::diagnostics`): `enum class ObdProtocol { Iso9141, Iso14230, Iso15765 }`; `std::string_view protocol_name(ObdProtocol)`; `std::size_t response_index(ObdProtocol)`; `bytes::Bytes build_request(ObdProtocol, std::uint32_t source_id, bytes::ByteView payload)`; `enum class ResponseCheck { Short, Ok, Nrc, WrongId }`; `ResponseCheck check_response(ObdProtocol, bytes::ByteView frame, std::uint8_t mode, std::optional<std::uint8_t> pid)`; `bytes::Bytes unframe_data_response(ObdProtocol, bytes::ByteView)`; `bytes::Bytes unframe_dtc_list_response(ObdProtocol, bytes::ByteView)`; `std::optional<KlineHeader> five_baud_header(ObdProtocol requested, bytes::ByteView response, bool uses_j2534)`; `bool fast_init_accepted(bytes::ByteView)`; `std::string format_hex(bytes::ByteView)`; `std::string format_pid_page_label(std::size_t page, bytes::ByteView bitmap)`; `std::string format_supported_pids(std::size_t page, bytes::ByteView bitmap)`; `std::vector<std::uint16_t> decode_dtcs(bytes::ByteView)`; target `//src/backend/diagnostics:obd_frames`.

- [ ] **Step 1: Write the failing tests**

`src/backend/diagnostics/obd_frames_test.cpp`:

```cpp
#include "src/backend/diagnostics/obd_frames.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace fastecu::diagnostics;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Optional;

namespace
{
bytes::Bytes b(std::initializer_list<int> values)
{
    bytes::Bytes out;
    for (int v : values)
    {
        out.push_back(static_cast<bytes::Byte>(v));
    }
    return out;
}
} // namespace

TEST(ObdFrames, RequestsCarryTheCanIdPrefixOnlyOnIso15765)
{
    EXPECT_THAT(build_request(ObdProtocol::Iso9141, 0x7E0, b({0x01, 0x00})), ElementsAre(0x01, 0x00));
    EXPECT_THAT(build_request(ObdProtocol::Iso15765, 0x7E0, b({0x03})), ElementsAre(0x00, 0x00, 0x07, 0xE0, 0x03));
}

TEST(ObdFrames, CheckResponse)
{
    // K-Line: response byte at index 3.
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10}), 0x01, 0x00), ResponseCheck::Short);
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x41, 0x00, 0xAA}), 0x01, 0x00),
              ResponseCheck::Ok);
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x7F, 0x01, 0x12}), 0x01, 0x00),
              ResponseCheck::Nrc);
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x42, 0x00}), 0x01, 0x00),
              ResponseCheck::WrongId);
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x41, 0x20}), 0x01, 0x00),
              ResponseCheck::WrongId);
    // A PID echo byte missing entirely is a wrong response, not an out-of-range read.
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x41}), 0x01, 0x00), ResponseCheck::WrongId);
    // No PID to echo (DTC list requests).
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x43}), 0x03, std::nullopt),
              ResponseCheck::Ok);
    // PIDs >= 0x80 compare unsigned (spec behavior change 5).
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x41, 0xA0, 0x00}), 0x01, 0xA0),
              ResponseCheck::Ok);
    // iso15765: index 4.
    EXPECT_EQ(check_response(ObdProtocol::Iso15765, b({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00}), 0x01, 0x00),
              ResponseCheck::Ok);
}

TEST(ObdFrames, KlineDataUnframingHeuristics)
{
    // Checksum dropped first; then by the remaining length m:
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso9141, b({0x11})), IsEmpty());              // m == 0
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 0xCC})),
                ElementsAre(6));                                                               // m < 7: last byte
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 7, 8, 9, 0xCC})),
                ElementsAre(6, 7, 8, 9));                                                      // m < 10: drop 5
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xCC})),
                ElementsAre(7, 8, 9, 10));                                                     // else: drop 6
}

TEST(ObdFrames, KlineDtcListUnframingHeuristics)
{
    EXPECT_THAT(unframe_dtc_list_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 0xCC})), ElementsAre(6));
    EXPECT_THAT(unframe_dtc_list_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 7, 0xCC})),
                ElementsAre(5, 6, 7));
}

TEST(ObdFrames, Iso15765Unframing)
{
    const auto frame = b({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00, 0xBE, 0x1F});
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso15765, frame), ElementsAre(0x1F));
    EXPECT_THAT(unframe_dtc_list_response(ObdProtocol::Iso15765, b({0x00, 0x00, 0x07, 0xE8, 0x43, 0x01, 0x01, 0x33})),
                ElementsAre(0x01, 0x33));
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso15765, b({0x00, 0x00, 0x07})), IsEmpty());
}

TEST(ObdFrames, FiveBaudHeaderOnOpenPortComparesAsciiDigits)
{
    // Pinned as-is: the J2534 branch compares bytes to ASCII '8' and 'f'.
    const auto iso9141 = b({0, 0, 0, 0, 0, '8', 0, '8'});
    EXPECT_THAT(five_baud_header(ObdProtocol::Iso9141, iso9141, true), Optional(KlineHeader::Iso9141));
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso14230, iso9141, true), std::nullopt);
    const auto iso14230 = b({0, 0, 0, 0, 0, 0, 0, 0, '8', 'f'});
    EXPECT_THAT(five_baud_header(ObdProtocol::Iso14230, iso14230, true), Optional(KlineHeader::Iso14230));
    // Short responses are rejected, not read out of range.
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso9141, b({0, 0, 0, 0, 0, '8', 0}), true), std::nullopt);
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso14230, b({0, 0, 0, 0, 0, 0, 0, 0, '8'}), true), std::nullopt);
}

TEST(ObdFrames, FiveBaudHeaderOnDirectSerialIgnoresTheRequestedProtocol)
{
    EXPECT_THAT(five_baud_header(ObdProtocol::Iso14230, b({0x55, 0x08, 0x08}), false),
                Optional(KlineHeader::Iso9141));
    EXPECT_THAT(five_baud_header(ObdProtocol::Iso9141, b({0x55, 0xEF, 0x8F}), false),
                Optional(KlineHeader::Iso14230));
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso9141, b({0x55, 0x00, 0x00}), false), std::nullopt);
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso9141, b({0x55, 0x08}), false), std::nullopt);
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso9141, bytes::Bytes{}, false), std::nullopt);
}

TEST(ObdFrames, FastInitAcceptance)
{
    EXPECT_TRUE(fast_init_accepted(b({0x83, 0xF1, 0x10, 0xC1, 0xE9, 0x8F, 0xAE})));
    EXPECT_FALSE(fast_init_accepted(b({0x83, 0xF1, 0x10, 0xC1, 0xE9})));
    EXPECT_FALSE(fast_init_accepted(b({0x83, 0xF1, 0x11, 0xC1, 0xE9, 0x8F})));
}

TEST(ObdFrames, Formatting)
{
    EXPECT_EQ(format_hex(b({0x83, 0x0A})), "83 0a ");
    EXPECT_EQ(format_pid_page_label(1, b({0xBE})), "Supported PIDs 0x21-0x40: be ");
    EXPECT_EQ(format_supported_pids(0, b({0x80})), "0x01 0x00 0x00 0x00 0x00 0x00 0x00 0x00 ");
    EXPECT_EQ(format_supported_pids(0, b({0x01})), "0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x08 ");
}

TEST(ObdFrames, DtcDecodingDropsZerosSortsAndIgnoresAnOddTail)
{
    EXPECT_THAT(decode_dtcs(b({0x04, 0x20, 0x00, 0x00, 0x01, 0x33, 0x7F})), ElementsAre(0x0133, 0x0420));
}
```

`src/backend/diagnostics/BUILD.bazel`:

```python
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("//bazel:gtest_targets.bzl", "fastecu_portable_gtest")

package(default_visibility = [
    "//src/backend:__subpackages__",
    "//src/platform:__subpackages__",
    "//src/ui:__subpackages__",
    "//tests:__pkg__",
])

cc_library(
    name = "obd_frames",
    srcs = ["obd_frames.cpp"],
    hdrs = ["obd_frames.h"],
    deps = [
        "//src/algorithms/protocol",
        "//src/backend/protocol",
    ],
)

fastecu_portable_gtest(
    name = "obd_frames_test",
    srcs = ["obd_frames_test.cpp"],
    deps = [":obd_frames"],
)
```

In `bazel/portable_targets.bzl`, add (keeping keys sorted, before `"src/backend/flash"`):

```python
    "src/backend/diagnostics": [
        "obd_frames",
    ],
```

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/backend/diagnostics:obd_frames_test`
Expected: FAIL — `obd_frames.h` not found.

- [ ] **Step 3: Implement**

`src/backend/diagnostics/obd_frames.h`:

```cpp
#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/protocol/idiagnostic_link.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// OBD-II framing rules the DTC dialog used, reproduced exactly (step 6g spec,
// "DTC today"). Byte comparisons are unsigned; bounds are checked.
namespace fastecu::diagnostics
{

enum class ObdProtocol
{
    Iso9141,
    Iso14230,
    Iso15765,
};

constexpr std::string_view protocol_name(ObdProtocol protocol) noexcept
{
    switch (protocol)
    {
    case ObdProtocol::Iso9141:
        return "iso9141";
    case ObdProtocol::Iso14230:
        return "iso14230";
    case ObdProtocol::Iso15765:
        return "iso15765";
    }
    return "iso9141";
}

// Index of the service-response byte in a received frame.
constexpr std::size_t response_index(ObdProtocol protocol) noexcept
{
    return protocol == ObdProtocol::Iso15765 ? 4 : 3;
}

bytes::Bytes build_request(ObdProtocol protocol, std::uint32_t source_id, bytes::ByteView payload);

enum class ResponseCheck
{
    Short,   // no response byte at response_index(); the caller keeps reading
    Ok,
    Nrc,     // 0x7F at response_index()
    WrongId, // mode or PID echo does not match, or the PID echo is missing
};

ResponseCheck check_response(ObdProtocol protocol, bytes::ByteView frame, std::uint8_t mode,
                             std::optional<std::uint8_t> pid);

bytes::Bytes unframe_data_response(ObdProtocol protocol, bytes::ByteView frame);
bytes::Bytes unframe_dtc_list_response(ObdProtocol protocol, bytes::ByteView frame);

// The header to set after a five-baud init, or nullopt when rejected.
std::optional<KlineHeader> five_baud_header(ObdProtocol requested, bytes::ByteView response, bool uses_j2534);
bool fast_init_accepted(bytes::ByteView response);

// "%02x " per byte, as the dialog's parse_message_to_hex did.
std::string format_hex(bytes::ByteView data);
std::string format_pid_page_label(std::size_t page, bytes::ByteView bitmap);
std::string format_supported_pids(std::size_t page, bytes::ByteView bitmap);
std::vector<std::uint16_t> decode_dtcs(bytes::ByteView data);

} // namespace fastecu::diagnostics
```

`src/backend/diagnostics/obd_frames.cpp`:

```cpp
#include "src/backend/diagnostics/obd_frames.h"

#include <algorithm>
#include <array>
#include <format>

namespace fastecu::diagnostics
{

bytes::Bytes build_request(ObdProtocol protocol, std::uint32_t source_id, bytes::ByteView payload)
{
    bytes::Bytes out;
    if (protocol == ObdProtocol::Iso15765)
    {
        out = {0x00, 0x00, static_cast<bytes::Byte>((source_id >> 8U) & 0xFFU),
               static_cast<bytes::Byte>(source_id & 0xFFU)};
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

ResponseCheck check_response(ObdProtocol protocol, bytes::ByteView frame, std::uint8_t mode,
                             std::optional<std::uint8_t> pid)
{
    const std::size_t index = response_index(protocol);
    if (frame.size() <= index)
    {
        return ResponseCheck::Short;
    }
    if (frame[index] == 0x7F)
    {
        return ResponseCheck::Nrc;
    }
    if (frame[index] != static_cast<bytes::Byte>(mode | 0x40U))
    {
        return ResponseCheck::WrongId;
    }
    if (pid.has_value() && (frame.size() <= index + 1 || frame[index + 1] != *pid))
    {
        return ResponseCheck::WrongId;
    }
    return ResponseCheck::Ok;
}

namespace
{
bytes::Bytes tail(bytes::ByteView data, std::size_t from)
{
    if (from >= data.size())
    {
        return {};
    }
    return bytes::Bytes(data.begin() + static_cast<std::ptrdiff_t>(from), data.end());
}

// K-Line: drop the checksum, then keep only the last byte of a short frame.
bytes::ByteView without_checksum(bytes::ByteView frame)
{
    return frame.empty() ? frame : frame.first(frame.size() - 1);
}
} // namespace

bytes::Bytes unframe_data_response(ObdProtocol protocol, bytes::ByteView frame)
{
    if (protocol == ObdProtocol::Iso15765)
    {
        return tail(frame, response_index(protocol) + 3);
    }
    const bytes::ByteView body = without_checksum(frame);
    if (body.empty())
    {
        return {};
    }
    if (body.size() < 7)
    {
        return tail(body, body.size() - 1);
    }
    return tail(body, body.size() < 10 ? 5 : 6);
}

bytes::Bytes unframe_dtc_list_response(ObdProtocol protocol, bytes::ByteView frame)
{
    if (protocol == ObdProtocol::Iso15765)
    {
        return tail(frame, response_index(protocol) + 2);
    }
    const bytes::ByteView body = without_checksum(frame);
    if (body.empty())
    {
        return {};
    }
    if (body.size() < 7)
    {
        return tail(body, body.size() - 1);
    }
    return tail(body, 4);
}

std::optional<KlineHeader> five_baud_header(ObdProtocol requested, bytes::ByteView r, bool uses_j2534)
{
    if (uses_j2534)
    {
        if (requested == ObdProtocol::Iso9141 && r.size() > 7 && r[5] == '8' && r[7] == '8')
        {
            return KlineHeader::Iso9141;
        }
        if (requested == ObdProtocol::Iso14230 && r.size() > 9 && r[8] == '8' && r[9] == 'f')
        {
            return KlineHeader::Iso14230;
        }
        return std::nullopt;
    }
    if (r.size() > 2 && r[1] == 0x08 && r[2] == 0x08)
    {
        return KlineHeader::Iso9141;
    }
    if (r.size() > 2 && r[2] == 0x8F)
    {
        return KlineHeader::Iso14230;
    }
    return std::nullopt;
}

bool fast_init_accepted(bytes::ByteView response)
{
    static constexpr std::array<bytes::Byte, 6> kExpected{0x83, 0xF1, 0x10, 0xC1, 0xE9, 0x8F};
    return response.size() >= kExpected.size() && std::equal(kExpected.begin(), kExpected.end(), response.begin());
}

std::string format_hex(bytes::ByteView data)
{
    std::string out;
    for (const bytes::Byte b : data)
    {
        out += std::format("{:02x} ", b);
    }
    return out;
}

std::string format_pid_page_label(std::size_t page, bytes::ByteView bitmap)
{
    const std::size_t start = page * 0x20 + 1;
    return std::format("Supported PIDs 0x{:x}-0x{:x}: ", start, start + 0x1F) + format_hex(bitmap);
}

std::string format_supported_pids(std::size_t page, bytes::ByteView bitmap)
{
    const std::size_t start = page * 0x20 + 1;
    std::string out;
    for (std::size_t i = 0; i < bitmap.size(); ++i)
    {
        for (int j = 7; j >= 0; --j)
        {
            const std::size_t enabled = (static_cast<unsigned>(bitmap[i]) >> static_cast<unsigned>(j)) & 1U;
            out += std::format("0x{:02x} ", enabled * ((i * 8 + 7 - static_cast<std::size_t>(j)) + start));
        }
    }
    return out;
}

std::vector<std::uint16_t> decode_dtcs(bytes::ByteView data)
{
    std::vector<std::uint16_t> codes;
    for (std::size_t i = 0; i < data.size(); i += 2)
    {
        // readU16Be yields 0 for a trailing odd byte, which is dropped below.
        if (const std::uint16_t code = bytes::readU16Be(data, i); code != 0)
        {
            codes.push_back(code);
        }
    }
    std::ranges::sort(codes);
    return codes;
}

} // namespace fastecu::diagnostics
```

- [ ] **Step 4: Run the tests**

Run: `bazel test --config=release //src/backend/diagnostics:obd_frames_test`
Expected: PASS.

- [ ] **Step 5: Mutation-check the two pinned quirks**

Per the design notes ("Pin every correction with a mutation check"): temporarily change `r[5] == '8'` to `r[5] == 0x08` in `five_baud_header`, run the test, confirm `FiveBaudHeaderOnOpenPortComparesAsciiDigits` FAILS, and revert. Then temporarily change `frame[index + 1] != *pid` to `static_cast<char>(frame[index + 1]) != static_cast<int>(*pid)`, confirm `CheckResponse` FAILS, and revert.

- [ ] **Step 6: Commit**

```bash
git add src/backend/diagnostics bazel/portable_targets.bzl
git commit -m "feat(diagnostics): portable OBD framing helpers (step 6g-4)"
```

### Task 9: `run_dtc_session`

**Files:**
- Create: `src/backend/diagnostics/dtc_session.h`, `src/backend/diagnostics/dtc_session.cpp`
- Test: `src/backend/diagnostics/dtc_session_test.cpp`
- Modify: `src/backend/diagnostics/BUILD.bazel`, `bazel/portable_targets.bzl`

**Interfaces:**
- Consumes: Task 8's helpers; `IDiagnosticLink`, `FakeDiagnosticLink` (Task 2); `fastecu::IClock`, `fastecu::ICancellationToken`, `fastecu::IEventSink`; `dtc_description`, `nrc_description` (`src/algorithms/diagnostics`).
- Produces: `enum class DtcOperation { Read, Clear }`; `struct DtcRequest { ObdProtocol protocol; DtcOperation operation; }`; `struct SupportedPidPage { std::size_t page; bytes::Bytes bitmap; }`; `struct DtcReport`; `Result<DtcReport> run_dtc_session(const DtcRequest&, IDiagnosticLink&, IClock&, const ICancellationToken&, IEventSink&)`; target `//src/backend/diagnostics:dtc_session`.

- [ ] **Step 1: Write the failing tests**

`src/backend/diagnostics/dtc_session_test.cpp`:

```cpp
#include "src/backend/diagnostics/dtc_session.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using namespace fastecu::diagnostics;
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::FakeCancellationToken;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::IsEmpty;
using ::testing::Not;

namespace
{
bytes::Bytes b(std::initializer_list<int> values)
{
    bytes::Bytes out;
    for (int v : values)
    {
        out.push_back(static_cast<bytes::Byte>(v));
    }
    return out;
}

std::vector<std::string> lines(const RecordingEventSink& sink, LogLevel level)
{
    std::vector<std::string> out;
    for (const auto& [l, message] : sink.logs)
    {
        if (l == level)
        {
            out.push_back(message);
        }
    }
    return out;
}

// The 14 vehicle-info requests, each answered by one "no frame" read.
std::vector<std::string> vehicle_info_calls(const std::string& read, const std::string& prefix = "")
{
    std::vector<std::string> calls;
    for (const char *pid : {"00", "20", "40", "60", "80", "A0", "C0"})
    {
        calls.push_back("write " + prefix + "01 " + pid);
        calls.push_back(read);
    }
    calls.push_back("write " + prefix + "01 01");
    calls.push_back(read);
    for (const char *pid : {"01", "02", "03", "04", "05", "06"})
    {
        calls.push_back("write " + prefix + "09 " + pid);
        calls.push_back(read);
    }
    return calls;
}

struct Harness
{
    FakeDiagnosticLink link;
    FakeClock clock;
    FakeCancellationToken token;
    RecordingEventSink events;

    fastecu::Result<DtcReport> run(ObdProtocol protocol, DtcOperation operation)
    {
        return run_dtc_session(DtcRequest{protocol, operation}, link, clock, token, events);
    }
};
} // namespace

TEST(DtcSession, Iso9141DirectReadRunsTheFullSequence)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    // Page 0x00: one frame, then no frame.
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x41, 0x00, 0xBE, 0x1F, 0xB8, 0x10, 0xCC}));
    h.link.queue_no_frame();
    // 13 more vehicle-info requests, each answered by one "no frame".
    for (int i = 0; i < 13; ++i)
    {
        h.link.queue_no_frame();
    }
    // Stored, then pending.
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x43, 0x01, 0x33, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();

    const auto report = h.run(ObdProtocol::Iso9141, DtcOperation::Read);

    ASSERT_THAT(report, IsOk());
    std::vector<std::string> expected{
        "open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A",
        "p1 35",
        "five_baud 33",
        "p1 25",
        "set_header Iso9141",
    };
    auto info = vehicle_info_calls("read_obd 200");
    info.insert(info.begin() + 1, "read_obd 200"); // page 0x00: a frame, then no frame
    expected.insert(expected.end(), info.begin(), info.end());
    expected.insert(expected.end(), {"write 03", "read_obd 200", "read_obd 200", "write 07", "read_obd 200",
                                     "read_obd 200", "set_header None", "reset"});
    EXPECT_THAT(h.link.calls, ElementsAreArray(expected));
    EXPECT_EQ(h.clock.elapsed(), 4500ms);
    ASSERT_EQ(report->supported_pids.size(), 1U);
    EXPECT_THAT(report->supported_pids[0].bitmap, ElementsAre(0xBE, 0x1F, 0xB8, 0x10));
    EXPECT_THAT(report->stored, ElementsAre(0x0133));
    EXPECT_THAT(report->pending, IsEmpty());
    const auto info_lines = lines(h.events, LogLevel::Info);
    EXPECT_THAT(info_lines, Contains("Supported PIDs 0x1-0x20: be 1f b8 10 "));
    EXPECT_THAT(info_lines, Contains("Stored DTCs: 01 33 00 00 00 00 "));
    EXPECT_THAT(info_lines, Contains("DTC: " + dtc_description(0x0133)));
    EXPECT_THAT(info_lines, Contains("Pending DTCs: 00 00 00 00 00 00 "));
    EXPECT_THAT(info_lines, Contains("Diagnostic trouble codes succesfully read!"));
    EXPECT_TRUE(h.link.script_consumed());
}

TEST(DtcSession, OpenPortUsesPlainReadsAndTheAsciiFiveBaudCheck)
{
    Harness h;
    h.link.j2534 = true;
    h.link.queue_five_baud(b({0, 0, 0, 0, 0, '8', 0, '8'}));
    const auto report = h.run(ObdProtocol::Iso9141, DtcOperation::Read);
    // Stored DTC request gets no frame -> BadResponse after vehicle info.
    EXPECT_THAT(report, IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(h.link.calls, Contains("set_header Iso9141"));
    EXPECT_THAT(h.link.calls, Contains("read 200"));
    EXPECT_THAT(h.link.calls, Not(Contains("read_obd 200")));
    EXPECT_THAT(h.link.calls, Not(Contains("p1 25"))); // only direct serial restores P1
}

TEST(DtcSession, Iso14230FastInitSuccess)
{
    Harness h;
    h.link.queue_read(b({0x83, 0xF1, 0x10, 0xC1, 0xE9, 0x8F, 0xAE}));
    static_cast<void>(h.run(ObdProtocol::Iso14230, DtcOperation::Read));
    ASSERT_GE(h.link.calls.size(), 4U);
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin(), h.link.calls.begin() + 4),
                ElementsAre("open kline header=Iso14230 iso14230=true baud=10400 start=C0 tester=F1 target=33",
                            "fast_init 81", "read_obd 200", "write 01 00"));
    EXPECT_THAT(lines(h.events, LogLevel::Info), Contains("iso14230 fast init mode succesfully completed."));
}

TEST(DtcSession, RejectedFastInitFallsBackToFiveBaud)
{
    Harness h;
    h.link.queue_read(b({0x83, 0xF1, 0x10, 0x00, 0x00, 0x00}));
    h.link.queue_five_baud(b({0x55, 0xEF, 0x8F}));
    static_cast<void>(h.run(ObdProtocol::Iso14230, DtcOperation::Read));
    EXPECT_THAT(lines(h.events, LogLevel::Error), Contains("iso14230 fast init mode failed."));
    ASSERT_GE(h.link.calls.size(), 8U);
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin() + 3, h.link.calls.begin() + 8),
                ElementsAre("open kline header=None iso14230=false baud=10400 start=C0 tester=F1 target=33", "p1 35",
                            "five_baud 33", "p1 25", "set_header Iso14230"));
}

TEST(DtcSession, FacadeFastInitFailureFallsBackSilently)
{
    Harness h;
    h.link.queue_fast_init(fastecu::fail(ErrorKind::Disconnected, "fast_init failed"));
    h.link.queue_five_baud(b({0x55, 0xEF, 0x8F}));
    static_cast<void>(h.run(ObdProtocol::Iso14230, DtcOperation::Read));
    EXPECT_THAT(lines(h.events, LogLevel::Error), Not(Contains("iso14230 fast init mode failed.")));
    EXPECT_THAT(h.link.calls, Contains("five_baud 33"));
    EXPECT_THAT(h.link.calls, Contains("set_header Iso14230"));
}

TEST(DtcSession, RejectedFiveBaudFailsAndStillRestoresTheLink)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x00, 0x00}));
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(lines(h.events, LogLevel::Error), Contains("iso9141 five baud init failed."));
    EXPECT_THAT(h.link.calls, ElementsAre("open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A",
                                          "p1 35", "five_baud 33", "p1 25", "set_header None", "reset"));
}

TEST(DtcSession, ShortFiveBaudResponseFailsCleanly)
{
    Harness h;
    h.link.j2534 = true;
    h.link.queue_five_baud(b({0x00, 0x08}));
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(lines(h.events, LogLevel::Info), Contains("Init response: 00 08 "));
}

TEST(DtcSession, Iso15765ReadDecodesCanFrames)
{
    Harness h;
    h.link.j2534 = true;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00})); // init answer
    for (int i = 0; i < 14; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x43, 0x01, 0x01, 0x33}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x47, 0x00, 0x00, 0x00}));
    h.link.queue_no_frame();
    const auto report = h.run(ObdProtocol::Iso15765, DtcOperation::Read);
    ASSERT_THAT(report, IsOk());
    EXPECT_THAT(std::vector<std::string>(h.link.calls.begin(), h.link.calls.begin() + 4),
                ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "write 00 00 07 E0 01 00", "read 2000", "write 00 00 07 E0 01 00"));
    EXPECT_THAT(report->stored, ElementsAre(0x0133));
}

TEST(DtcSession, Iso15765InitNrcIsLoggedFromOffsetThree)
{
    Harness h;
    h.link.j2534 = true;
    h.link.queue_read(b({0x00, 0x00, 0x07, 0xE8, 0x7F, 0x01, 0x12}));
    EXPECT_THAT(h.run(ObdProtocol::Iso15765, DtcOperation::Read), IsErr(ErrorKind::BadResponse));
    const bytes::Bytes nrc_frame = b({0xE8, 0x7F, 0x01, 0x12});
    EXPECT_THAT(lines(h.events, LogLevel::Error),
                Contains("Wrong response from ECU: " + nrc_description(nrc_frame)));
}

TEST(DtcSession, NrcOnStoredDtcsLogsAndFails)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    for (int i = 0; i < 14; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x7F, 0x03, 0x22, 0xCC}));
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::BadResponse));
    const bytes::Bytes nrc_frame = b({0x7F, 0x03, 0x22, 0xCC});
    EXPECT_THAT(lines(h.events, LogLevel::Error),
                Contains("Wrong response from ECU: " + nrc_description(nrc_frame)));
}

TEST(DtcSession, WrongPidIsLoggedAndDiscarded)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x41, 0x20, 0xBE, 0xCC}));
    static_cast<void>(h.run(ObdProtocol::Iso9141, DtcOperation::Read));
    EXPECT_THAT(lines(h.events, LogLevel::Error), Contains("Wrong response from ECU: 48 6b 10 41 20 be cc "));
    EXPECT_THAT(h.link.calls, Contains("write 01 20")); // the loop moved on
}

TEST(DtcSession, HighPidPagesAreAccepted)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    for (int i = 0; i < 4; ++i)
    {
        h.link.queue_no_frame(); // pages 00, 20, 40, 60
    }
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x41, 0x80, 0x01, 0x02, 0x03, 0x04, 0xCC}));
    h.link.queue_no_frame();
    static_cast<void>(h.run(ObdProtocol::Iso9141, DtcOperation::Read));
    EXPECT_THAT(lines(h.events, LogLevel::Info), Contains("Supported PIDs 0x81-0xa0: 01 02 03 04 "));
}

TEST(DtcSession, ClearSucceedsOnPositiveAcknowledgement)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    for (int i = 0; i < 14; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x44, 0xCC}));
    const auto report = h.run(ObdProtocol::Iso9141, DtcOperation::Clear);
    ASSERT_THAT(report, IsOk());
    EXPECT_TRUE(report->cleared);
    EXPECT_THAT(lines(h.events, LogLevel::Info), Contains("Diagnostic trouble codes succesfully cleared!"));
    EXPECT_THAT(h.link.calls, Contains("write 04"));
}

TEST(DtcSession, ClearWithoutAcknowledgementFails)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    for (int i = 0; i < 14; ++i)
    {
        h.link.queue_no_frame();
    }
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    h.link.queue_read(b({0x48, 0x6B, 0x10, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCC}));
    h.link.queue_no_frame();
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Clear), IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(h.link.calls.back(), "reset");
}

TEST(DtcSession, CancellationStopsAtTheFirstSleepAndRestoresTheLink)
{
    Harness h;
    h.link.queue_five_baud(b({0x55, 0x08, 0x08}));
    h.token.set_cancelled(true);
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::Cancelled));
    EXPECT_THAT(h.link.calls, Not(Contains("write 01 00")));
    EXPECT_THAT(std::vector<std::string>(h.link.calls.end() - 2, h.link.calls.end()),
                ElementsAre("set_header None", "reset"));
}

TEST(DtcSession, OpenFailureEndsRunAfterEpilogue)
{
    Harness h;
    h.link.queue_open(fastecu::fail(ErrorKind::Disconnected, "adapter did not open a port"));
    EXPECT_THAT(h.run(ObdProtocol::Iso9141, DtcOperation::Read), IsErr(ErrorKind::Disconnected));
    EXPECT_THAT(h.link.calls,
                ElementsAre("open kline header=None iso14230=false baud=10400 start=68 tester=F1 target=6A",
                            "set_header None", "reset"));
}
```

Add to `src/backend/diagnostics/BUILD.bazel`:

```python
cc_library(
    name = "dtc_session",
    srcs = ["dtc_session.cpp"],
    hdrs = ["dtc_session.h"],
    deps = [
        ":obd_frames",
        "//src/algorithms/diagnostics",
        "//src/algorithms/protocol",
        "//src/backend/ports",
        "//src/backend/protocol",
    ],
)

fastecu_portable_gtest(
    name = "dtc_session_test",
    srcs = ["dtc_session_test.cpp"],
    deps = [
        ":dtc_session",
        "//src/algorithms/diagnostics",
        "//src/backend/ports/testing:fake_cancellation_token",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/ports/testing:recording_event_sink",
        "//src/backend/ports/testing:result_matchers",
        "//src/backend/protocol/testing:fake_diagnostic_link",
    ],
)
```

In `bazel/portable_targets.bzl`, extend the entry to `"src/backend/diagnostics": ["dtc_session", "obd_frames"],`. Confirm the `//src/backend/ports/testing` target names by `grep -n "name =" src/backend/ports/testing/BUILD.bazel` and adjust if they differ.

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/backend/diagnostics:dtc_session_test`
Expected: FAIL — `dtc_session.h` not found.

- [ ] **Step 3: Implement**

`src/backend/diagnostics/dtc_session.h`:

```cpp
#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/diagnostics/obd_frames.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/idiagnostic_link.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace fastecu::diagnostics
{

enum class DtcOperation
{
    Read,
    Clear,
};

struct DtcRequest
{
    ObdProtocol protocol = ObdProtocol::Iso9141;
    DtcOperation operation = DtcOperation::Read;
};

struct SupportedPidPage
{
    std::size_t page = 0;
    bytes::Bytes bitmap;
};

struct DtcReport
{
    std::vector<SupportedPidPage> supported_pids;
    std::optional<bytes::Bytes> monitor_status, vin_length, vin, cal_id_length, cal_id, cvn_length, cvn;
    std::vector<std::uint16_t> stored, pending;
    bool cleared = false;
};

// One OBD-II DTC read or clear, reproducing the legacy DtcOperations dialog's
// wire sequence, sleeps, and log wording (step 6g spec, "DTC today"). Always
// clears the link's header and resets it before returning.
Result<DtcReport> run_dtc_session(const DtcRequest& request, IDiagnosticLink& link, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events);

} // namespace fastecu::diagnostics
```

`src/backend/diagnostics/dtc_session.cpp`:

```cpp
#include "src/backend/diagnostics/dtc_session.h"

#include <array>
#include <chrono>
#include <string>
#include <utility>

#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/diagnostics/nrc_parser.h"

namespace fastecu::diagnostics
{
namespace
{
using namespace std::chrono_literals;

constexpr auto kShortRead = 200ms;
constexpr auto kCanInitRead = 2000ms;
constexpr auto kBeforeVehicleInfo = 500ms;
constexpr auto kBetweenRequests = 250ms;
constexpr std::uint8_t kFiveBaudAddress = 0x33;
constexpr std::uint8_t kFastInitWakeup = 0x81;
constexpr std::uint32_t kCanSource = 0x7E0;
constexpr std::uint32_t kCanDestination = 0x7E8;
constexpr std::uint8_t kLiveData = 0x01;
constexpr std::uint8_t kStoredDtcs = 0x03;
constexpr std::uint8_t kClearDtcs = 0x04;
constexpr std::uint8_t kPendingDtcs = 0x07;
constexpr std::uint8_t kVehicleInfo = 0x09;
constexpr std::array<std::uint8_t, 7> kSupportPages{0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0};

struct KlineIds
{
    std::uint8_t start_byte;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

constexpr KlineIds ids_for(ObdProtocol protocol)
{
    return protocol == ObdProtocol::Iso9141 ? KlineIds{0x68, 0xF1, 0x6A} : KlineIds{0xC0, 0xF1, 0x33};
}

std::string as_text(const bytes::Bytes& data)
{
    return std::string(data.begin(), data.end());
}

class DtcRun
{
  public:
    DtcRun(const DtcRequest& request, IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
           IEventSink& events)
        : request_(request), link_(link), clock_(clock), cancellation_(cancellation), events_(events)
    {
    }

    Result<DtcReport> execute()
    {
        const Status outcome = body();
        // Today's select_operation epilogue; its results were never checked.
        static_cast<void>(link_.set_header(KlineHeader::None));
        static_cast<void>(link_.reset());
        if (!outcome.has_value())
        {
            return std::unexpected(outcome.error());
        }
        return std::move(report_);
    }

  private:
    Status body()
    {
        if (auto initialised = init(); !initialised.has_value())
        {
            return initialised;
        }
        if (auto info = vehicle_info(); !info.has_value())
        {
            return info;
        }
        return request_.operation == DtcOperation::Read ? read_dtcs() : clear_dtcs();
    }

    Status init()
    {
        switch (request_.protocol)
        {
        case ObdProtocol::Iso9141:
            return five_baud(ObdProtocol::Iso9141);
        case ObdProtocol::Iso14230:
            if (auto fast = fast_init(); fast.has_value() || fast.error().kind == ErrorKind::Cancelled)
            {
                return fast;
            }
            return five_baud(ObdProtocol::Iso14230);
        case ObdProtocol::Iso15765:
            return can_init();
        }
        return fail(ErrorKind::Internal, "unknown OBD protocol");
    }

    Status five_baud(ObdProtocol requested)
    {
        const std::string name(protocol_name(requested));
        const KlineIds ids = ids_for(requested);
        if (auto opened = link_.open(KlineLinkConfig{.header = KlineHeader::None,
                                                     .iso14230_connection = false,
                                                     .baud = 10400,
                                                     .start_byte = ids.start_byte,
                                                     .tester_id = ids.tester_id,
                                                     .target_id = ids.target_id});
            !opened.has_value())
        {
            return opened;
        }
        info("Testing " + name + " five baud init, please wait...");
        static_cast<void>(link_.set_p1_max(35ms)); // result never checked today
        auto response = link_.five_baud_init(kFiveBaudAddress);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        info("Init response: " + format_hex(*response));
        const bool j2534 = link_.uses_j2534();
        const std::optional<KlineHeader> header = five_baud_header(requested, *response, j2534);
        if (!j2534)
        {
            static_cast<void>(link_.set_p1_max(25ms));
        }
        if (!header.has_value())
        {
            error(name + " five baud init failed.");
            return fail(ErrorKind::BadResponse, name + " five baud init failed");
        }
        static_cast<void>(link_.set_header(*header));
        info(name + " five baud init succesfully completed.");
        return {};
    }

    Status fast_init()
    {
        const KlineIds ids = ids_for(ObdProtocol::Iso14230);
        if (auto opened = link_.open(KlineLinkConfig{.header = KlineHeader::Iso14230,
                                                     .iso14230_connection = true,
                                                     .baud = 10400,
                                                     .start_byte = ids.start_byte,
                                                     .tester_id = ids.tester_id,
                                                     .target_id = ids.target_id});
            !opened.has_value())
        {
            return opened;
        }
        info("Initialising iso14230 fast init K-Line communications, please wait...");
        if (auto woke = link_.fast_init(bytes::Bytes{kFastInitWakeup}); !woke.has_value())
        {
            return woke; // silent, as today; the caller falls back to five-baud
        }
        auto frame = read_frame(kShortRead);
        if (!frame.has_value())
        {
            return std::unexpected(frame.error());
        }
        if (!frame->has_value() || !fast_init_accepted(**frame))
        {
            error("iso14230 fast init mode failed.");
            return fail(ErrorKind::BadResponse, "iso14230 fast init mode failed");
        }
        info("iso14230 fast init mode succesfully completed.");
        return {};
    }

    Status can_init()
    {
        if (auto opened = link_.open(CanLinkConfig{.iso15765 = true,
                                                   .bitrate = 500000,
                                                   .extended_id = false,
                                                   .source_id = kCanSource,
                                                   .destination_id = kCanDestination});
            !opened.has_value())
        {
            return opened;
        }
        info("Initialising iso15765 CAN communications, please wait...");
        const bytes::Bytes probe = build_request(ObdProtocol::Iso15765, kCanSource, bytes::Bytes{kLiveData, 0x00});
        if (auto written = link_.write(probe); !written.has_value())
        {
            return std::unexpected(written.error());
        }
        auto frame = link_.read(kCanInitRead, cancellation_);
        if (!frame.has_value())
        {
            return std::unexpected(frame.error());
        }
        const bytes::Bytes f = frame->value_or(bytes::Bytes{});
        if (f.size() <= 4)
        {
            return fail(ErrorKind::BadResponse, "no iso15765 init response");
        }
        if (f[4] == 0x7F)
        {
            // Today's code describes the NRC from offset 3, not 4.
            error("Wrong response from ECU: " + nrc_description(bytes::ByteView(f).subspan(3)));
            return fail(ErrorKind::BadResponse, "iso15765 init rejected");
        }
        if (f[4] != 0x41)
        {
            error("Wrong response from ECU: " + format_hex(f));
            return fail(ErrorKind::BadResponse, "iso15765 init wrong response");
        }
        info("iso15765 init mode succesfully completed.");
        return {};
    }

    Status vehicle_info()
    {
        info("Requesting vehicle info, please wait...");
        if (auto slept = clock_.sleep(kBeforeVehicleInfo, cancellation_); !slept.has_value())
        {
            return slept;
        }
        for (std::size_t page = 0; page < kSupportPages.size(); ++page)
        {
            auto response = request(kLiveData, kSupportPages.at(page), false);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (!response->empty())
            {
                info(format_pid_page_label(page, *response));
                info("Supported PIDs: " + format_supported_pids(page, *response));
                report_.supported_pids.push_back(SupportedPidPage{page, *response});
            }
            if (auto slept = clock_.sleep(kBetweenRequests, cancellation_); !slept.has_value())
            {
                return slept;
            }
        }
        struct Item
        {
            std::uint8_t mode;
            std::uint8_t pid;
            const char *label;
            std::optional<bytes::Bytes> DtcReport::*field;
            bool also_text;
        };
        static constexpr std::array<Item, 7> kItems{{
            {kLiveData, 0x01, "Status since DTCs cleared: ", &DtcReport::monitor_status, false},
            {kVehicleInfo, 0x01, "VIN length: ", &DtcReport::vin_length, false},
            {kVehicleInfo, 0x02, "VIN: ", &DtcReport::vin, true},
            {kVehicleInfo, 0x03, "CAL ID length: ", &DtcReport::cal_id_length, false},
            {kVehicleInfo, 0x04, "CAL ID: ", &DtcReport::cal_id, true},
            {kVehicleInfo, 0x05, "CAL ID num length: ", &DtcReport::cvn_length, false},
            {kVehicleInfo, 0x06, "CAL ID num: ", &DtcReport::cvn, false},
        }};
        for (const Item& item : kItems)
        {
            auto response = request(item.mode, item.pid, false);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (!response->empty())
            {
                info(std::string(item.label) + format_hex(*response));
                if (item.also_text)
                {
                    info(std::string(item.label) + as_text(*response));
                }
                report_.*item.field = *response;
            }
            if (auto slept = clock_.sleep(kBetweenRequests, cancellation_); !slept.has_value())
            {
                return slept;
            }
        }
        return {};
    }

    Status read_dtcs()
    {
        struct List
        {
            std::uint8_t mode;
            const char *label;
            const char *missing;
            std::vector<std::uint16_t> DtcReport::*field;
        };
        static constexpr std::array<List, 2> kLists{{
            {kStoredDtcs, "Stored DTCs: ", "no stored DTC response", &DtcReport::stored},
            {kPendingDtcs, "Pending DTCs: ", "no pending DTC response", &DtcReport::pending},
        }};
        for (const List& list : kLists)
        {
            auto response = request(list.mode, std::nullopt, true);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (response->empty())
            {
                return fail(ErrorKind::BadResponse, list.missing);
            }
            info(std::string(list.label) + format_hex(*response));
            report_.*list.field = decode_dtcs(*response);
            for (const std::uint16_t code : report_.*list.field)
            {
                info("DTC: " + dtc_description(code));
            }
            if (auto slept = clock_.sleep(kBetweenRequests, cancellation_); !slept.has_value())
            {
                return slept;
            }
        }
        info("Diagnostic trouble codes succesfully read!");
        return {};
    }

    Status clear_dtcs()
    {
        if (auto read = read_dtcs(); !read.has_value())
        {
            return read;
        }
        const std::size_t index = response_index(request_.protocol);
        if (auto written = link_.write(build_request(request_.protocol, kCanSource, bytes::Bytes{kClearDtcs}));
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        bool cleared = false;
        while (true)
        {
            auto frame = read_frame(kShortRead);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }
            if (!frame->has_value())
            {
                break;
            }
            const bytes::Bytes& f = **frame;
            if (f.size() <= index)
            {
                continue; // today's loop keeps reading past a short frame
            }
            if (f[index] == 0x7F)
            {
                error("Wrong response from ECU: " + nrc_description(bytes::ByteView(f).subspan(index)));
                break;
            }
            if (f[index] != (kClearDtcs | 0x40U))
            {
                error("Wrong response from ECU: " + format_hex(f));
                break;
            }
            cleared = true;
            break;
        }
        if (!cleared)
        {
            return fail(ErrorKind::BadResponse, "clear DTCs not acknowledged");
        }
        report_.cleared = true;
        info("Diagnostic trouble codes succesfully cleared!");
        return {};
    }

    // Writes one request and collects frames until an empty read. An NRC or
    // wrong response is logged and ends collection with what was gathered.
    Result<bytes::Bytes> request(std::uint8_t mode, std::optional<std::uint8_t> pid, bool dtc_list)
    {
        bytes::Bytes payload{mode};
        if (pid.has_value())
        {
            payload.push_back(*pid);
        }
        if (auto written = link_.write(build_request(request_.protocol, kCanSource, payload)); !written.has_value())
        {
            return std::unexpected(written.error());
        }
        bytes::Bytes response;
        while (true)
        {
            auto frame = read_frame(kShortRead);
            if (!frame.has_value())
            {
                return std::unexpected(frame.error());
            }
            if (!frame->has_value())
            {
                break;
            }
            const bytes::Bytes& f = **frame;
            const ResponseCheck check = check_response(request_.protocol, f, mode, pid);
            if (check == ResponseCheck::Nrc)
            {
                error("Wrong response from ECU: " +
                      nrc_description(bytes::ByteView(f).subspan(response_index(request_.protocol))));
                break;
            }
            if (check == ResponseCheck::WrongId)
            {
                error("Wrong response from ECU: " + format_hex(f));
                break;
            }
            const bytes::Bytes data = dtc_list ? unframe_dtc_list_response(request_.protocol, f)
                                               : unframe_data_response(request_.protocol, f);
            response.insert(response.end(), data.begin(), data.end());
        }
        return response;
    }

    Result<IDiagnosticLink::OptionalBytes> read_frame(std::chrono::milliseconds timeout)
    {
        return link_.uses_j2534() ? link_.read(timeout, cancellation_) : link_.read_obd(timeout, cancellation_);
    }

    void info(const std::string& message)
    {
        events_.log(LogLevel::Info, message);
    }
    void error(const std::string& message)
    {
        events_.log(LogLevel::Error, message);
    }

    DtcRequest request_;
    IDiagnosticLink& link_;
    IClock& clock_;
    const ICancellationToken& cancellation_;
    IEventSink& events_;
    DtcReport report_;
};

} // namespace

Result<DtcReport> run_dtc_session(const DtcRequest& request, IDiagnosticLink& link, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events)
{
    return DtcRun(request, link, clock, cancellation, events).execute();
}

} // namespace fastecu::diagnostics
```

Note the `Iso15765InitNrcIsLoggedFromOffsetThree` expectation: offset 3 of `00 00 07 E8 7F 01 12` is `E8 7F 01 12`, which is what the test builds.

- [ ] **Step 4: Run the tests**

Run: `bazel test --config=release //src/backend/diagnostics:all`
Expected: PASS. If a sequence assertion fails, compare against the spec's "DTC today" section and `git show origin/master:src/ui/desktop/dtc_operations.cpp`, not against the test — the legacy dialog is the source of truth, except for the recorded behavior changes 2, 5, and 6.

- [ ] **Step 5: Commit**

```bash
git add src/backend/diagnostics bazel/portable_targets.bzl
git commit -m "feat(diagnostics): portable DTC session (step 6g-4)"
```

### Task 10: `DtcWorker`

**Files:**
- Create: `src/platform/desktop/common/diagnostics/dtc_worker.h`, `src/platform/desktop/common/diagnostics/dtc_worker.cpp`
- Test: `src/platform/desktop/common/diagnostics/dtc_worker_test.cpp`
- Modify: `src/platform/desktop/common/diagnostics/BUILD.bazel`

**Interfaces:**
- Consumes: `run_dtc_session`, `DtcRequest` (Task 9); `QtEventSink` (`src/platform/desktop/common/ports/qt_event_sink.h`); `fastecu::ManualCancellationToken` (`src/backend/ports/manual_cancellation_token.h`).
- Produces: `fastecu::diagnostics::DtcWorkerResult { bool success; ErrorKind error_kind; QString error_detail; }`; `DtcWorker(DtcRequest, IDiagnosticLink&, std::unique_ptr<IClock>, QObject *parent = nullptr)`, `requestStop()`, signals `logEvent(int level, QString message)` and `completed(fastecu::diagnostics::DtcWorkerResult result)`.

- [ ] **Step 1: Write the failing test**

`src/platform/desktop/common/diagnostics/dtc_worker_test.cpp`:

```cpp
#include "src/platform/desktop/common/diagnostics/dtc_worker.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::diagnostics::DtcOperation;
using fastecu::diagnostics::DtcRequest;
using fastecu::diagnostics::DtcWorker;
using fastecu::diagnostics::DtcWorkerResult;
using fastecu::diagnostics::FakeDiagnosticLink;
using fastecu::diagnostics::ObdProtocol;

class DtcWorkerTest : public QObject
{
    Q_OBJECT

  private slots:

    void reportsTheSessionOutcomeAndForwardsLogLines()
    {
        FakeDiagnosticLink link;
        link.queue_five_baud(bytes::Bytes{0x55, 0x00, 0x00}); // rejected
        DtcWorker worker(DtcRequest{ObdProtocol::Iso9141, DtcOperation::Read}, link, std::make_unique<FakeClock>());
        QSignalSpy logs(&worker, &DtcWorker::logEvent);
        QSignalSpy done(&worker, &DtcWorker::completed);
        worker.start();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<DtcWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::BadResponse);
        QVERIFY(logs.count() >= 2); // "Testing ..." and "iso9141 five baud init failed."
    }

    void stopBeforeStartCancelsTheRun()
    {
        FakeDiagnosticLink link;
        link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08});
        DtcWorker worker(DtcRequest{ObdProtocol::Iso9141, DtcOperation::Read}, link, std::make_unique<FakeClock>());
        QSignalSpy done(&worker, &DtcWorker::completed);
        worker.requestStop();
        worker.start();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.at(0).at(0).value<DtcWorkerResult>().error_kind, ErrorKind::Cancelled);
        QCOMPARE(link.calls.back(), std::string("reset"));
    }
};

QTEST_MAIN(DtcWorkerTest)
#include "dtc_worker_test.moc"
```

Add to `src/platform/desktop/common/diagnostics/BUILD.bazel`:

```python
qt_cc_library(
    name = "dtc_worker",
    srcs = ["dtc_worker.cpp"],
    hdrs = ["dtc_worker.h"],
    copts = COMMON_COPTS,
    deps = QT_DEPS + [
        "//src/backend/diagnostics:dtc_session",
        "//src/backend/ports",
        "//src/backend/protocol",
        "//src/platform/desktop/common/ports",
    ],
)

fastecu_qttest(
    name = "dtc_worker_test",
    src = "dtc_worker_test.cpp",
    deps = [
        ":dtc_worker",
        "//src/backend/ports/testing:fake_clock",
        "//src/backend/protocol/testing:fake_diagnostic_link",
    ],
)
```

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/platform/desktop/common/diagnostics:dtc_worker_test`
Expected: FAIL — `dtc_worker.h` not found.

- [ ] **Step 3: Implement**

`src/platform/desktop/common/diagnostics/dtc_worker.h`:

```cpp
#pragma once

#include <QString>
#include <QThread>

#include <memory>

#include "src/backend/diagnostics/dtc_session.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/idiagnostic_link.h"

namespace fastecu::diagnostics
{

// Qt-friendly outcome: std::expected is not a Qt metatype.
struct DtcWorkerResult
{
    bool success = false;
    ErrorKind error_kind = ErrorKind::Internal;
    QString error_detail;
};

// Runs one run_dtc_session on its own thread. Mirrors ServiceFunctionWorker
// without operator gates. The link is not owned and must outlive the worker.
class DtcWorker final : public QThread
{
    Q_OBJECT

  public:
    DtcWorker(DtcRequest request, IDiagnosticLink& link, std::unique_ptr<IClock> clock, QObject *parent = nullptr);
    ~DtcWorker() override;

    DtcWorker(const DtcWorker&) = delete;
    DtcWorker& operator=(const DtcWorker&) = delete;

    // Safe from any thread, any number of times, before or after start().
    void requestStop();

  signals:
    void logEvent(int level, QString message);
    // Emitted exactly once per run(), from the worker thread. Named
    // `completed` so it does not overload QThread::finished().
    void completed(fastecu::diagnostics::DtcWorkerResult result);

  protected:
    void run() override;

  private:
    DtcRequest request_;
    IDiagnosticLink& link_;
    std::unique_ptr<IClock> clock_;
    ManualCancellationToken cancellation_;
};

} // namespace fastecu::diagnostics

Q_DECLARE_METATYPE(fastecu::diagnostics::DtcWorkerResult)
```

`src/platform/desktop/common/diagnostics/dtc_worker.cpp`:

```cpp
#include "src/platform/desktop/common/diagnostics/dtc_worker.h"

#include <utility>

#include "src/platform/desktop/common/ports/qt_event_sink.h"

namespace fastecu::diagnostics
{

DtcWorker::DtcWorker(DtcRequest request, IDiagnosticLink& link, std::unique_ptr<IClock> clock, QObject *parent)
    : QThread(parent), request_(request), link_(link), clock_(std::move(clock))
{
    qRegisterMetaType<DtcWorkerResult>();
}

DtcWorker::~DtcWorker()
{
    requestStop();
    // run() uses owned members; join fully before they are destroyed.
    wait();
}

void DtcWorker::requestStop()
{
    cancellation_.cancel();
}

void DtcWorker::run()
{
    QtEventSink events;
    connect(&events, &QtEventSink::logged, this, &DtcWorker::logEvent, Qt::DirectConnection);
    connect(
        &events, &QtEventSink::noticed, this,
        [this](QString message) { emit logEvent(static_cast<int>(LogLevel::Info), std::move(message)); },
        Qt::DirectConnection);

    const Result<DtcReport> report = run_dtc_session(request_, link_, *clock_, cancellation_, events);
    DtcWorkerResult result;
    result.success = report.has_value();
    if (!report.has_value())
    {
        result.error_kind = report.error().kind;
        result.error_detail = QString::fromStdString(report.error().detail);
    }
    emit completed(result);
}

} // namespace fastecu::diagnostics
```

- [ ] **Step 4: Run the tests**

Run: `bazel test --config=release //src/platform/desktop/common/diagnostics:all`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/platform/desktop/common/diagnostics
git commit -m "feat(platform): DtcWorker runs the DTC session off the UI thread (step 6g-4)"
```

### Task 11: `DtcOperations` dialog on the worker

**Files:**
- Modify: `src/ui/desktop/dtc_operations.h`, `src/ui/desktop/dtc_operations.cpp` (rewrite)
- Modify: `src/ui/desktop/BUILD.bazel` (new `:dtc_operations` target; move `dtc_operations.{h,cpp}` out of `:desktop`)
- Modify: `src/ui/desktop/menu_actions.cpp` (`show_dtc_window`)
- Test: `src/ui/desktop/dtc_operations_test.cpp`

**Interfaces:**
- Consumes: `DtcWorker`, `DtcWorkerResult` (Task 10); `DtcRequest`, `ObdProtocol`, `DtcOperation` (Tasks 8–9); `QtClock` (`src/platform/desktop/common/ports/qt_clock.h`); `SerialDiagnosticLink` (Task 3).
- Produces: `DtcOperations(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent = nullptr)`; target `//src/ui/desktop:dtc_operations`.

- [ ] **Step 1: Write the failing test**

`src/ui/desktop/dtc_operations_test.cpp`:

```cpp
#include "src/ui/desktop/dtc_operations.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>

#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using fastecu::diagnostics::FakeDiagnosticLink;

class DtcOperationsTest : public QObject
{
    Q_OBJECT

  private slots:

    void aFailedRunLogsOnceAndReenablesTheButtons()
    {
        FakeDiagnosticLink link; // five-baud answers nothing -> fails before any sleep
        DtcOperations dialog(link);
        QSignalSpy errors(&dialog, &DtcOperations::LOG_E);
        auto *read = dialog.findChild<QPushButton *>("readDtcButton");
        QVERIFY(read != nullptr);
        read->click();
        QVERIFY(!read->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(read->isEnabled(), 5000);
        const bool logged = std::any_of(errors.begin(), errors.end(), [](const QList<QVariant>& args)
                                        { return args.at(0).toString().startsWith("DTC operation failed: "); });
        QVERIFY(logged);
    }

    void closeDuringARunStopsTheWorkerAndResets()
    {
        FakeDiagnosticLink link;
        link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08}); // accepted -> 500 ms sleep follows
        auto *dialog = new DtcOperations(link);
        dialog->findChild<QPushButton *>("readDtcButton")->click();
        QTest::qWait(50);
        QElapsedTimer timer;
        timer.start();
        dialog->close();
        QVERIFY(timer.elapsed() < 400);
        QCOMPARE(link.calls.back(), std::string("reset"));
        QVERIFY(std::find(link.calls.begin(), link.calls.end(), "set_header None") != link.calls.end());
        delete dialog;
    }
};

QTEST_MAIN(DtcOperationsTest)
#include "dtc_operations_test.moc"
```

In `src/ui/desktop/BUILD.bazel`: remove `"dtc_operations.cpp"` from `:desktop` `srcs` and `"dtc_operations.h"` from its `hdrs`; add `":dtc_operations",` to `:desktop` deps; add:

```python
qt_cc_library(
    name = "dtc_operations",
    srcs = ["dtc_operations.cpp"],
    hdrs = ["dtc_operations.h"],
    copts = COMMON_COPTS,
    deps = QT_DEPS + [
        ":ui_dtc_operations",
        "//src/backend/diagnostics:dtc_session",
        "//src/backend/ports",
        "//src/backend/protocol",
        "//src/platform/desktop/common/diagnostics:dtc_worker",
        "//src/platform/desktop/common/ports",
    ],
)

fastecu_qttest(
    name = "dtc_operations_test",
    src = "dtc_operations_test.cpp",
    copts = ["-DQT_WIDGETS_LIB"],
    env = {"QT_QPA_PLATFORM": "offscreen"},
    deps = [
        ":dtc_operations",
        "//src/backend/protocol/testing:fake_diagnostic_link",
    ],
)
```

- [ ] **Step 2: Run to see it fail**

Run: `bazel test --config=release //src/ui/desktop:dtc_operations_test`
Expected: FAIL — no `DtcOperations(IDiagnosticLink&)` constructor.

- [ ] **Step 3: Rewrite the dialog**

`src/ui/desktop/dtc_operations.h`:

```cpp
#pragma once

#include <QCloseEvent>
#include <QDialog>
#include <QString>

#include <memory>

#include "src/backend/protocol/idiagnostic_link.h"
#include "src/platform/desktop/common/diagnostics/dtc_worker.h"

namespace Ui
{
class DtcOperationsWindow;
}

// Presents OBD-II DTC read/clear. The protocol runs in DtcWorker; the dialog
// collects the protocol choice and forwards log lines.
class DtcOperations : public QDialog
{
    Q_OBJECT

  public:
    explicit DtcOperations(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent = nullptr);
    ~DtcOperations() override;

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void start(fastecu::diagnostics::DtcOperation operation);
    void forwardLog(int level, const QString& message);
    void finish(const fastecu::diagnostics::DtcWorkerResult& result);
    void setButtonsEnabled(bool enabled);

    fastecu::diagnostics::IDiagnosticLink& link_;
    std::unique_ptr<Ui::DtcOperationsWindow> ui;
    std::unique_ptr<fastecu::diagnostics::DtcWorker> worker_;
};
```

`src/ui/desktop/dtc_operations.cpp`:

```cpp
#include "src/ui/desktop/dtc_operations.h"

#include <QPushButton>
#include <QStandardItemModel>

#include <ui_dtc_operations.h>

#include "src/backend/diagnostics/dtc_session.h"
#include "src/backend/ports/event_sink.h"
#include "src/platform/desktop/common/ports/qt_clock.h"

using fastecu::diagnostics::DtcOperation;
using fastecu::diagnostics::DtcRequest;
using fastecu::diagnostics::DtcWorker;
using fastecu::diagnostics::DtcWorkerResult;
using fastecu::diagnostics::ObdProtocol;

DtcOperations::DtcOperations(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent)
    : QDialog(parent), link_(link), ui{std::make_unique<Ui::DtcOperationsWindow>()}
{
    ui->setupUi(this);

    ui->protocolComboBox->addItem("SSM (K-Line)");
    ui->protocolComboBox->addItem("SSM (CAN)");
    ui->protocolComboBox->addItem("iso9141");
    ui->protocolComboBox->addItem("iso14230");
    ui->protocolComboBox->addItem("iso15765");
    ui->protocolComboBox->setCurrentIndex(2);

    if (auto *model = qobject_cast<QStandardItemModel *>(ui->protocolComboBox->model()); model != nullptr)
    {
        for (int i = 0; i < ui->protocolComboBox->count(); i++)
        {
            if (auto *item = model->item(i); item != nullptr && item->text().startsWith("SSM "))
            {
                item->setEnabled(false);
            }
        }
    }

    connect(ui->readDtcButton, &QPushButton::clicked, this, [this] { start(DtcOperation::Read); });
    connect(ui->clearDtcButton, &QPushButton::clicked, this, [this] { start(DtcOperation::Clear); });
    connect(ui->closeButton, &QPushButton::clicked, this, &QDialog::close);

    this->show();
}

DtcOperations::~DtcOperations() = default;

void DtcOperations::start(DtcOperation operation)
{
    const QString text = ui->protocolComboBox->currentText();
    ObdProtocol protocol = ObdProtocol::Iso9141;
    if (text.startsWith("iso9141"))
    {
        protocol = ObdProtocol::Iso9141;
    }
    else if (text.startsWith("iso14230"))
    {
        protocol = ObdProtocol::Iso14230;
    }
    else if (text.startsWith("iso15765"))
    {
        protocol = ObdProtocol::Iso15765;
    }
    else
    {
        return; // SSM entries are disabled, as before
    }

    setButtonsEnabled(false);
    worker_ = std::make_unique<DtcWorker>(DtcRequest{protocol, operation}, link_, std::make_unique<QtClock>());
    connect(worker_.get(), &DtcWorker::logEvent, this, &DtcOperations::forwardLog, Qt::QueuedConnection);
    connect(worker_.get(), &DtcWorker::completed, this, &DtcOperations::finish, Qt::QueuedConnection);
    worker_->start();
}

void DtcOperations::forwardLog(int level, const QString& message)
{
    if (level == static_cast<int>(fastecu::LogLevel::Error))
    {
        emit LOG_E(message, true, true);
    }
    else if (level == static_cast<int>(fastecu::LogLevel::Warning))
    {
        emit LOG_W(message, true, true);
    }
    else if (level == static_cast<int>(fastecu::LogLevel::Debug))
    {
        emit LOG_D(message, true, true);
    }
    else
    {
        emit LOG_I(message, true, true);
    }
}

void DtcOperations::finish(const DtcWorkerResult& result)
{
    if (!result.success)
    {
        emit LOG_E("DTC operation failed: " + result.error_detail, true, true);
    }
    worker_.reset(); // joins; run() has already returned or is returning
    setButtonsEnabled(true);
}

void DtcOperations::setButtonsEnabled(bool enabled)
{
    ui->readDtcButton->setEnabled(enabled);
    ui->clearDtcButton->setEnabled(enabled);
}

void DtcOperations::closeEvent(QCloseEvent *event)
{
    if (worker_)
    {
        worker_->requestStop();
        worker_->wait();
        worker_.reset();
    }
    // Today's closeEvent reset the facade; the session epilogue already reset
    // after a run, and a second reset is harmless.
    static_cast<void>(link_.reset());
    QDialog::closeEvent(event);
}
```

Check `ui_dtc_operations.h`'s form class name with `grep -n "class" src/ui/desktop/dtc_operations.ui | head -3`; it must be `DtcOperationsWindow` (it is, per the old header). If the `.ui` references slots or widgets the old dialog created in code, keep them — the rewrite only changes wiring.

In `show_dtc_window` (`src/ui/desktop/menu_actions.cpp`), replace `DtcOperations dtcOperations(serial, this);` with:

```cpp
    fastecu::diagnostics::SerialDiagnosticLink link(serial);
    DtcOperations dtcOperations(link, this);
```

and delete the commented `// dtcOperations->run();` line.

- [ ] **Step 4: Run the tests**

Run: `bazel test --config=release //src/ui/desktop:dtc_operations_test //src/ui/desktop:test_mainwindow`
Expected: PASS.

Run: `grep -rn "serial_port_actions.h" src/ui/desktop/dtc_operations.* src/ui/desktop/dataterminal.* src/ui/desktop/biu`
Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add src/ui/desktop
git commit -m "refactor(ui): DtcOperations runs DtcWorker over IDiagnosticLink (step 6g-4)"
```

### Task 12: Close-out docs and bench checklist

**Files:**
- Create: `docs/diagnostics-bench-checklist.md`
- Modify: `docs/modularization-plan.md`, `docs/design-notes.md`, `docs/tech-debt.md`
- Delete: `docs/superpowers/specs/2026-09-26-step6g-diagnostic-tools-design.md`, `docs/superpowers/plans/2026-09-26-step6g-diagnostic-tools.md`

- [ ] **Step 1: Write the bench checklist**

`docs/diagnostics-bench-checklist.md` — follow the structure of [the logging-composition checklist](../../logging-composition-bench-checklist.md) (open it and mirror its headings: purpose, prerequisites, a table of checks with Expected and Result columns, sign-off). Checks:

1. DTC read, iso9141, OpenPort 2.0: five-baud succeeds, vehicle info lists PID pages including 0x81–0xA0 when the ECU supports them, stored and pending DTCs logged.
2. DTC read, iso9141, direct K-Line cable: same, via the OBD-framed read.
3. DTC read, iso14230, fast init succeeds.
4. DTC read, iso14230, fast init rejected → five-baud fallback succeeds.
5. DTC clear on each protocol that read succeeded on: "Diagnostic trouble codes succesfully cleared!".
6. DTC read, iso15765, OpenPort 2.0.
7. Close the DTC dialog during vehicle info: dialog closes promptly; next DTC run works.
8. BIU connect and keep-alive for at least 30 s.
9. DataTerminal: SSM K-Line send gets a response; CAN iso15765 send gets a response; after using DTC first, DataTerminal still sends with no header (flags reset by `open()`).

State at the top that no row is qualified until run on a bench, and that the flash qualification matrix is not affected.

- [ ] **Step 2: Update the modularization plan**

In `docs/modularization-plan.md`:
- Status paragraph: 6f is merged (#378); add 6g as complete and name 6h (MainWindow connection service and SSM identification) as next.
- Under step 6, after the 6f bullet, add a **6g diagnostic tools — complete** bullet in the same style as 6e: what moved (`IDiagnosticLink` in `src/backend/protocol`, `SerialDiagnosticLink` and `DtcWorker` in `//src/platform/desktop/common/diagnostics`, `run_dtc_session` in `//src/backend/diagnostics`, `mut_memory`), what left (`//src/ui/desktop/biu` allowlist entry, the GRANDFATHERED UI→`transport` edge, `hexcommander`, `kline_listener`/`canbus_listener` via #379), the PR numbers, and links to the design notes section and the bench checklist.
- Update the baseline bullet on the portable closure to include `src/backend/diagnostics`.

- [ ] **Step 3: Update the design notes**

Add a `## Diagnostic tools` section to `docs/design-notes.md` (before `## Flash-operation dispatch`, matching the other step sections), covering: why the port is byte-faithful and exposes `uses_j2534()` instead of hiding the adapter split; why `SerialDiagnosticLink` lives in a new platform package via `serial_platform_api`; why BIU and DataTerminal stay synchronous; the pinned quirks (OpenPort five-baud ASCII `'8'`/`'f'` comparison, K-Line unframing heuristics, DataTerminal's `delay(...)` parse yielding 0, the ISO-15765 init NRC described from offset 3); and the recorded behavior changes 1–6 from the spec.

- [ ] **Step 4: Update the tech-debt roadmap**

In `docs/tech-debt.md`:
- "P1: Drain the `serial_qt_compat` allowlist": 4 entries now; list them; `//src/ui/desktop` is step 6h's.
- Add under "P1: Separate UI from application logic": an entry to confirm or fix the OpenPort five-baud ASCII comparison against a bench capture, and an entry for DataTerminal's `delay(...)` parser.
- Update the `mainwindow.cpp`/`menu_actions.cpp` line counts (`wc -l`).

- [ ] **Step 5: Delete the spec and plan**

```bash
git rm docs/superpowers/specs/2026-09-26-step6g-diagnostic-tools-design.md docs/superpowers/plans/2026-09-26-step6g-diagnostic-tools.md
```

- [ ] **Step 6: Final gate and commit**

Run: `bazel test --config=release //... && prek run --all-files && bazel run //:clang_tidy_report_changed`
Expected: all PASS; lychee finds no broken links.

```bash
git add -A docs
git commit -m "docs: close out step 6g diagnostic tools"
```

- [ ] **Step 7: Build the stack**

```bash
gh stack init --base master docs/step6g-diagnostic-tools-spec refactor/step6g-1-diagnostic-link \
  refactor/step6g-2-biu refactor/step6g-3-data-terminal refactor/step6g-4-dtc
gh stack submit --auto
```

Push only with the user's authorization. Each PR description lists its verification commands and ends with the Claude Code attribution footer.
