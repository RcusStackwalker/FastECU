# Step 6e Platform Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the serial layer's two platform choices — direct vs. remote, and J2534 Unix vs. Windows — out of `SerialPortActions` and the direct backend's preprocessor guards, into the desktop composition root and the BUILD graph.

**Architecture:** Three stacked PRs. 6e-1 makes the facade take a required backend factory and moves the direct/remote rule into `desktop_serial_factory` plus `DesktopComposition`, splitting the two backends into their own Bazel targets. 6e-2 replaces every OS guard in the direct backend with named per-OS hook functions defined in BUILD-selected source files. 6e-3 moves the link-time Unix/Windows `select()` into the `apps/desktop` and `apps/bench` binaries and closes out the docs.

**Tech Stack:** C++23, Qt 6 (Core, SerialPort, WebSockets, RemoteObjects), Bazel 9 with Bzlmod, QtTest + Google Mock, `prek`, `gh stack`.

**Spec:** [the step 6e design](../specs/2026-09-26-step6e-platform-selection-design.md). Read it before starting; this plan argues from it.

## Global Constraints

- No wire byte, log string, or call order changes on either OS. Hook bodies are moved text, not rewritten.
- The `serial_qt_compat` visibility list may only shrink (`scripts/check-serial-compat-allowlist.py`); 6e-1 removes `//src/platform/desktop/common/remote_utility:__pkg__` and adds nothing.
- Platform differences go in BUILD-selected source files, never new `#if` branches in shared sources (ADR 0005). A guard that cannot be avoided is spelled `_WIN32`, never bare `WIN32`.
- `STATUS_SUCCESS`/`STATUS_ERROR`/`SERIAL_P*` stay macros (the Windows SDK's `ntstatus.h` defines `STATUS_SUCCESS` as a macro).
- Every PR passes `bazel test --config=release //...`, `prek run --all-files`, and `bazel run //:clang_tidy_report_changed`. 6e-2 and 6e-3 additionally need a green Windows/macOS/Linux CI run before merge — Windows compiles only there.
- Never mark a hardware path qualified without a run checklist entry behind it (FastECU CLAUDE.md, "Hardware-facing caution").
- Commits end with the attribution lines from the session's system reminder:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc
  ```
- Branches: `docs/step6e-platform-selection-spec` (spec + this plan, already exists) → `refactor/step6e-1-serial-connection` → `refactor/step6e-2-j2534-os-split` → `refactor/step6e-3-link-select`, each branched from the previous one; published with `gh stack init --base master <all four, bottom to top>` then `gh stack submit --auto`.

## Review Focus

1. **Remote session startup** (`fastecu --host <peer>`): must still show the network splash and wait for both remote sources. No automated test can reach a real peer; Task 4 pins the direct branch (no wait) and the reviewer must read the remote branch diff by eye.
2. **Windows J2534 port list**: `check_serial_ports()` must still append the registry vendor names after the sorted serial ports. Only CI's Windows leg compiles `append_j2534_interfaces`; Task 8's Windows hook test pins `resolve_port` and `j2534_tx_done` there, and the reviewer compares the moved body line by line with `git show master:src/platform/desktop/common/serial/serial_port_actions_direct.cpp`.
3. **Unix open path with a non-adapter port first** (macOS `cu.Bluetooth-Incoming-Port`): must still skip J2534 and fall back to plain serial. Task 8's Unix hook test pins `resolve_port("ttyUSB0 - USB Serial").is_j2534 == false`.
4. **`fastecu-bench` with no factory**: a config built without `backend_factory` must fail cleanly with `InvalidConfig`, not crash. Task 3 pins it.
5. **A test target that forgets the direct backend implementation** after 6e-3: must fail to link, not silently fall back. Task 9 Step 6 proves it by deliberately removing the alias from one test and watching the link fail.

---

## PR 6e-1 — the direct/remote choice moves to the composition root

Start: `git checkout docs/step6e-platform-selection-spec && git checkout -b refactor/step6e-1-serial-connection`

### Task 1: One header for the facade's numeric contract; stop leaking the direct backend through the facade header

**Files:**
- Create: `src/platform/desktop/common/serial/serial_facade_codes.h`
- Modify: `src/platform/desktop/common/serial/BUILD.bazel` (`serial_qt_compat` `normal_hdrs`)
- Modify: `src/platform/desktop/common/serial/serial_port_actions_direct.h:113-120` (SERIAL_P* macros), `:618-619` (STATUS macros)
- Modify: `src/platform/desktop/common/serial/serial_port_actions_direct.cpp` (static_assert)
- Modify: `src/platform/desktop/common/serial/serial_port_actions.h:15-18` (transitive includes)
- Modify: `src/ui/desktop/dtc_operations.h:55-56`, `src/ui/desktop/dtc_operations.cpp:168`
- Modify: every consumer the build names (list in Step 5)

**Interfaces:**
- Produces: `serial_facade_codes.h` with macros `STATUS_SUCCESS`, `STATUS_ERROR`, `SERIAL_P1_MIN` … `SERIAL_P4_MAX` and `inline constexpr std::uint32_t kJ2534IoctlP1Max = 0x07;`. Include path `src/platform/desktop/common/serial/serial_facade_codes.h`.

- [ ] **Step 1: Create the codes header**

```cpp
#pragma once

#include <cstdint>

// Numbers that cross the SerialPortActions facade: its return codes and the
// parameter ids callers pass to set_kline_timings() / set_j2534_ioctl().
// Callers include this instead of reaching the direct backend's header.
//
// STATUS_* and SERIAL_P* stay macros: the Windows SDK's ntstatus.h defines
// STATUS_SUCCESS as a macro, which would break a constexpr of that name in
// any translation unit including both.
#define STATUS_SUCCESS 0x00
#define STATUS_ERROR 0x01

#define SERIAL_P1_MIN 0x00 // J2534 says this may not be changed
#define SERIAL_P1_MAX 0x01
#define SERIAL_P2_MIN 0x02 // J2534 says this may not be changed
#define SERIAL_P2_MAX 0x03 // J2534 says this may not be changed
#define SERIAL_P3_MIN 0x04
#define SERIAL_P3_MAX 0x05 // J2534 says this may not be changed
#define SERIAL_P4_MIN 0x06
#define SERIAL_P4_MAX 0x07 // J2534 says this may not be changed

// The J2534 P1_MAX IOCTL parameter id (0x07 in both J2534_tactrix_*.h),
// named here so UI callers need no J2534 header.
inline constexpr std::uint32_t kJ2534IoctlP1Max = 0x07;
```

- [ ] **Step 2: Register it and delete the duplicates**

In `src/platform/desktop/common/serial/BUILD.bazel`, add `"serial_facade_codes.h",` to `serial_qt_compat`'s `normal_hdrs` (keep the list sorted).

In `serial_port_actions_direct.h`, delete the eight `#define SERIAL_P*` lines (113-120) and the two `#define STATUS_*` lines (618-619), and add next to `#include "serial_backend.h"`:

```cpp
#include "src/platform/desktop/common/serial/serial_facade_codes.h"
```

In `serial_port_actions_direct.cpp`, after the last `#include`, add:

```cpp
static_assert(kJ2534IoctlP1Max == P1_MAX, "serial_facade_codes.h must match the J2534 header's P1_MAX");
```

In `src/ui/desktop/dtc_operations.h`, delete lines 55-56 (`#define STATUS_SUCCESS 0x00`, `#define STATUS_ERROR 0x01`) and add `#include "src/platform/desktop/common/serial/serial_facade_codes.h"` after the existing project includes. In `src/ui/desktop/dtc_operations.cpp:168`, change `serial->set_j2534_ioctl(P1_MAX, 35);` to `serial->set_j2534_ioctl(kJ2534IoctlP1Max, 35);`.

- [ ] **Step 3: Build to confirm the move alone compiles**

Run: `bazel build --config=release //...`
Expected: success (the facade header still includes the direct header at this point).

- [ ] **Step 4: Remove the facade header's transitive includes**

In `serial_port_actions.h`, delete these lines:

```cpp
// Kept for consumers that relied on this header's transitive includes
// (QSerialPort types, WebSocketIoDevice). Candidates for removal in spec 1b.
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"
#include "websocketiodevice.h"
```

and add, after `#include "serial_backend.h"`:

```cpp
#include "src/platform/desktop/common/serial/serial_facade_codes.h"
```

`QWebSocket` is still named by the constructor until Task 4; add `class QWebSocket;` next to `class SerialBackendHost;`.

- [ ] **Step 5: Build and add each missing include precisely**

Run: `bazel build -k --config=release //...`
Expected: compile errors in consumers that relied on the transitive include. Fix each by adding the exact header it uses — never by re-adding an include to `serial_port_actions.h`. Expected fixes (verify against the actual errors):

- `src/ui/desktop/menu_actions.cpp` — `#include <QSerialPort>` (`QSerialPort::NoParity`).
- `src/ui/desktop/log_operations_ssm.cpp` — `#include <QSerialPort>` (`QSerialPort::EvenParity`).
- `src/platform/desktop/common/serial/serial_port_actions.cpp` — `#include "serial_port_actions_direct.h"` (its default factory still constructs `SerialPortActionsDirect` until Task 4).
- `src/platform/desktop/common/serial/remote_serial_backend.cpp` — whatever Qt headers it used through the direct header (`<QTimer>`, `<QDebug>`, …).
- `tests/tst_mut_dma_integration.cpp` — `#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"` (it uses `ISO9141`, `SET_CONFIG`); in `tests/BUILD.bazel` that target already depends on `serial_qt_compat`, which owns the header until Task 5.
- Any file using `STATUS_*`/`SERIAL_P*` that no longer sees them — `#include "src/platform/desktop/common/serial/serial_facade_codes.h"`.

Repeat until `bazel build -k --config=release //...` succeeds.

- [ ] **Step 6: Run the suite**

Run: `bazel test --config=release //...`
Expected: all PASS.

- [ ] **Step 7: Commit**

```bash
git add -A src tests
git commit -m "refactor(serial): one header for the facade's numeric contract (step 6e-1)

serial_facade_codes.h holds STATUS_* and SERIAL_P* (moved from the direct
backend) and kJ2534IoctlP1Max; dtc_operations drops its duplicate
STATUS_* macros. serial_port_actions.h no longer re-exports the direct
backend and websocket headers; consumers include what they use.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

### Task 2: A direct-backend entry point; `SerialConnection` in the factory; the composition root owns the rule

**Files:**
- Create: `src/platform/desktop/common/serial/direct_serial_backend.h`, `direct_serial_backend.cpp`
- Modify: `src/platform/desktop/common/serial/desktop_serial_factory.h`, `desktop_serial_factory.cpp`
- Modify: `apps/desktop/desktop_composition.h`, `desktop_composition.cpp`
- Modify: `src/platform/desktop/common/serial/BUILD.bazel` (`serial_qt_compat` srcs/hdrs, temporarily)
- Test: `src/platform/desktop/common/serial/direct_backend_test.cpp`, `desktop_serial_factory_test.cpp`, `apps/desktop/desktop_composition_test.cpp`

**Interfaces:**
- Consumes: `RemoteSerialBackend(QString peerAddress, QString password, QWebSocket *externalSocket = nullptr, QObject *parent = nullptr)`.
- Produces: `std::unique_ptr<SerialBackend> make_direct_serial_backend();` in `src/platform/desktop/common/serial/direct_serial_backend.h`, and in `desktop_serial_factory.h`:
  ```cpp
  struct DirectSerial {};
  struct RemoteSerial { QString address; QString password; };
  using SerialConnection = std::variant<DirectSerial, RemoteSerial>;
  std::function<SerialBackend *()> make_serial_backend_factory(const SerialConnection& connection);
  OwnedSerialPortActions make_serial_port_actions(const SerialConnection& connection, QObject& log_sink);
  ```
  and in `desktop_composition.h`: `SerialConnection serial_connection_from_args(const QString& host, const QString& password);`

- [ ] **Step 1: Write the failing entry-point test**

In `direct_backend_test.cpp`, add `#include "src/platform/desktop/common/serial/direct_serial_backend.h"` and `#include <memory>`, declare `void makeDirectSerialBackend_buildsTheDirectBackend();` in the `private slots:` list, and define:

```cpp
void TestDirectBackend::makeDirectSerialBackend_buildsTheDirectBackend()
{
    const std::unique_ptr<SerialBackend> backend = make_direct_serial_backend();
    QVERIFY(dynamic_cast<SerialPortActionsDirect *>(backend.get()) != nullptr);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/serial:test_direct_backend`
Expected: FAIL — `direct_serial_backend.h` not found.

- [ ] **Step 3: Add the entry point**

`direct_serial_backend.h`:

```cpp
#pragma once

#include <memory>

class SerialBackend;

// Builds the local (serial port / J2534 adapter) backend. The only way code
// outside the direct backend's own target obtains one, so it never names
// SerialPortActionsDirect or includes a J2534 header.
std::unique_ptr<SerialBackend> make_direct_serial_backend();
```

`direct_serial_backend.cpp`:

```cpp
#include "src/platform/desktop/common/serial/direct_serial_backend.h"

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

std::unique_ptr<SerialBackend> make_direct_serial_backend()
{
    return std::make_unique<SerialPortActionsDirect>();
}
```

Until Task 5 splits the targets, register both files in `serial_qt_compat`: add `"direct_serial_backend.cpp",` to its `srcs` and `"direct_serial_backend.h",` to its `normal_hdrs`.

Run: `bazel test --config=release //src/platform/desktop/common/serial:test_direct_backend`
Expected: PASS.

- [ ] **Step 4: Write the failing factory tests**

Replace `desktop_serial_factory_test.cpp`'s includes and first test:

```cpp
#include <QStringList>
#include <QTest>

#include <memory>

#include "src/platform/desktop/common/serial/desktop_serial_factory.h"
#include "src/platform/desktop/common/serial/remote_serial_backend.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"
```

```cpp
    void directConnectionBuildsTheDirectBackend()
    {
        const auto factory = make_serial_backend_factory(DirectSerial{});
        QVERIFY(factory);
        const std::unique_ptr<SerialBackend> backend{factory()};
        QVERIFY(dynamic_cast<SerialPortActionsDirect *>(backend.get()) != nullptr);
    }

    // An unreachable peer: RemoteSerialBackend's constructor does not block
    // on it (see remote_backend_smoke_test.cpp).
    void remoteConnectionBuildsTheRemoteBackend()
    {
        const auto factory = make_serial_backend_factory(RemoteSerial{"local:fastecu-test-nonexistent", "pw"});
        QVERIFY(factory);
        const std::unique_ptr<SerialBackend> backend{factory()};
        QVERIFY(dynamic_cast<RemoteSerialBackend *>(backend.get()) != nullptr);
    }
```

and in `everyLogLevelReachesTheSink` change `make_serial_port_actions({}, {}, sink)` to `make_serial_port_actions(DirectSerial{}, sink)`. Delete `buildsADirectFacadeWhenNoPeerIsGiven` (its `isDirectConnection()` goes away in Task 4).

(`desktop_serial_factory_test` already depends on `:serial_qt_compat`, which owns both backend headers until Task 5.)

- [ ] **Step 5: Write the failing composition test**

In `apps/desktop/desktop_composition_test.cpp`, add `#include <variant>` and:

```cpp
    void emptyHostSelectsTheDirectBackend()
    {
        QVERIFY(std::holds_alternative<DirectSerial>(serial_connection_from_args({}, {})));
        QVERIFY(std::holds_alternative<DirectSerial>(serial_connection_from_args({}, "ignored")));
    }

    void nonEmptyHostSelectsTheRemoteBackendWithItsCredentials()
    {
        const SerialConnection connection = serial_connection_from_args("peer.example:1234", "secret");
        const auto *remote = std::get_if<RemoteSerial>(&connection);
        QVERIFY(remote != nullptr);
        QCOMPARE(remote->address, QString("peer.example:1234"));
        QCOMPARE(remote->password, QString("secret"));
    }
```

- [ ] **Step 6: Run them to verify they fail**

Run: `bazel test --config=release //src/platform/desktop/common/serial:desktop_serial_factory_test //apps/desktop:desktop_composition_test`
Expected: FAIL to compile — `make_serial_backend_factory`, `DirectSerial`, `serial_connection_from_args` undeclared.

- [ ] **Step 7: Implement the factory**

`desktop_serial_factory.h`:

```cpp
#pragma once

#include <functional>
#include <memory>
#include <variant>

#include <QString>

class QObject;
class SerialBackend;
class SerialPortActions;

// Constructor-only entry point to the serial facade for the desktop
// composition root; see this target's BUILD.bazel comment for why it exists.
// SerialPortActions stays an incomplete type for callers.
struct SerialPortActionsDeleter
{
    void operator()(SerialPortActions *serial) const;
};

using OwnedSerialPortActions = std::unique_ptr<SerialPortActions, SerialPortActionsDeleter>;

// Which backend the facade drives: the local adapter, or a FastECU remote
// peer. The composition root decides; the facade never knows.
struct DirectSerial
{
};

struct RemoteSerial
{
    QString address;
    QString password;
};

using SerialConnection = std::variant<DirectSerial, RemoteSerial>;

// The backend factory for a connection. The facade calls it once, lazily, on
// its I/O thread; separate from make_serial_port_actions so the choice can be
// tested without starting that thread.
std::function<SerialBackend *()> make_serial_backend_factory(const SerialConnection& connection);

// Builds the facade over the connection's backend and routes its
// LOG_E/LOG_W/LOG_I/LOG_D signals to log_sink's
// log_messages(QString, bool, bool) slot.
OwnedSerialPortActions make_serial_port_actions(const SerialConnection& connection, QObject& log_sink);
```

`desktop_serial_factory.cpp` (the facade still has its old constructor until Task 4, so pass the factory as the fifth argument):

```cpp
#include "src/platform/desktop/common/serial/desktop_serial_factory.h"

#include <array>

#include "src/platform/desktop/common/serial/direct_serial_backend.h"
#include "src/platform/desktop/common/serial/remote_serial_backend.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace
{
struct BackendFactoryFor
{
    std::function<SerialBackend *()> operator()(const DirectSerial& /*direct*/) const
    {
        return [] { return make_direct_serial_backend().release(); };
    }

    std::function<SerialBackend *()> operator()(const RemoteSerial& remote) const
    {
        return [remote]() -> SerialBackend * { return new RemoteSerialBackend(remote.address, remote.password); };
    }
};
} // namespace

void SerialPortActionsDeleter::operator()(SerialPortActions *serial) const
{
    delete serial;
}

std::function<SerialBackend *()> make_serial_backend_factory(const SerialConnection& connection)
{
    return std::visit(BackendFactoryFor{}, connection);
}

OwnedSerialPortActions make_serial_port_actions(const SerialConnection& connection, QObject& log_sink)
{
    auto serial = std::make_unique<SerialPortActions>(QString{}, QString{}, nullptr, nullptr,
                                                      make_serial_backend_factory(connection));
    // String-based connections, so log_sink can be any QObject with a
    // log_messages(QString, bool, bool) slot (SystemLogger in production).
    // SystemLogger::log_messages reads sender()'s signal to pick the level,
    // which a direct signal-to-slot connection preserves.
    const auto log_signals = std::to_array<const char *>({
        SIGNAL(LOG_E(QString, bool, bool)),
        SIGNAL(LOG_W(QString, bool, bool)),
        SIGNAL(LOG_I(QString, bool, bool)),
        SIGNAL(LOG_D(QString, bool, bool)),
    });
    for (const char *signal : log_signals)
    {
        QObject::connect(serial.get(), signal, &log_sink, SLOT(log_messages(QString, bool, bool)));
    }
    return OwnedSerialPortActions{serial.release()};
}
```

- [ ] **Step 8: Implement the composition rule**

In `apps/desktop/desktop_composition.h`, after the class, add:

```cpp
// The desktop app's direct/remote rule: no --host means the local adapter.
SerialConnection serial_connection_from_args(const QString& host, const QString& password);
```

In `desktop_composition.cpp`, add:

```cpp
SerialConnection serial_connection_from_args(const QString& host, const QString& password)
{
    if (host.isEmpty())
    {
        return DirectSerial{};
    }
    return RemoteSerial{.address = host, .password = password};
}
```

and change the construction line to:

```cpp
    serial_ = make_serial_port_actions(serial_connection_from_args(peer_address, peer_password), *syslogger_);
```

- [ ] **Step 9: Run the tests**

Run: `bazel test --config=release //src/platform/desktop/common/serial:desktop_serial_factory_test //apps/desktop:desktop_composition_test`
Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add -A src apps
git commit -m "refactor(desktop): the composition root picks the serial backend (step 6e-1)

make_direct_serial_backend() is the direct backend's entry point.
desktop_serial_factory takes a SerialConnection (DirectSerial or
RemoteSerial) and builds the matching backend factory;
serial_connection_from_args holds the empty-host rule.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

### Task 3: The CAN transport factory takes a backend factory; the bench passes the direct one

**Files:**
- Modify: `src/platform/desktop/common/transport/desktop_transport_factory.h:25-35`, `desktop_transport_factory.cpp:17-47`
- Modify: `apps/bench/main.cpp:58,70-72`, `apps/bench/BUILD.bazel` (`fastecu-bench` deps)
- Modify: `src/platform/desktop/common/serial/BUILD.bazel` (`desktop_serial_factory` visibility)
- Test: `src/platform/desktop/common/transport/desktop_transport_factory_test.cpp`

**Interfaces:**
- Consumes: `make_serial_backend_factory(const SerialConnection&)`, `DirectSerial` (Task 2).
- Produces: `DesktopCanTransportConfig { std::string port_name; std::function<SerialBackend *()> backend_factory; }`.

- [ ] **Step 1: Write the failing test**

In `desktop_transport_factory_test.cpp`, rename `config.backend_factory_for_tests` to `config.backend_factory` inside `configWith`, and add:

```cpp
    void refusesAConfigWithoutABackendFactory()
    {
        const DesktopCanTransportConfig config;

        const auto ports = list_desktop_serial_ports(config);
        QVERIFY(!ports.has_value());
        QCOMPARE(ports.error().kind, ErrorKind::InvalidConfig);

        const auto transport = open_desktop_can_flash_transport(config, kColtCan);
        QVERIFY(!transport.has_value());
        QCOMPARE(transport.error().kind, ErrorKind::InvalidConfig);
    }
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/transport:test_desktop_transport_factory`
Expected: FAIL to compile — `DesktopCanTransportConfig` has no member `backend_factory`.

- [ ] **Step 3: Implement**

In `desktop_transport_factory.h`, replace the struct body with:

```cpp
struct DesktopCanTransportConfig
{
    // Empty selects the first detected device.
    std::string port_name;

    // Builds the serial backend the facade drives; required. Production
    // passes make_serial_backend_factory(DirectSerial{}) from
    // desktop_serial_factory.h; tests pass a fake.
    std::function<SerialBackend *()> backend_factory;
};
```

In `desktop_transport_factory.cpp`, replace `make_serial` and the first lines of both entry points:

```cpp
Result<std::unique_ptr<SerialPortActions>> make_serial(const DesktopCanTransportConfig& config)
{
    if (!config.backend_factory)
    {
        return fail(ErrorKind::InvalidConfig, "no serial backend factory");
    }
    return std::make_unique<SerialPortActions>(QString{}, QString{}, nullptr, nullptr, config.backend_factory);
}
```

```cpp
Result<std::vector<std::string>> list_desktop_serial_ports(const DesktopCanTransportConfig& config)
{
    auto made = make_serial(config);
    if (!made.has_value())
    {
        return std::unexpected(made.error());
    }
    std::unique_ptr<SerialPortActions> serial = std::move(*made);
    std::vector<std::string> ports;
    ...unchanged...
```

```cpp
Result<std::unique_ptr<ICanFlashTransport>> open_desktop_can_flash_transport(const DesktopCanTransportConfig& config,
                                                                             const Iso15765Config& can)
{
    auto made = make_serial(config);
    if (!made.has_value())
    {
        return std::unexpected(made.error());
    }
    std::unique_ptr<SerialPortActions> serial = std::move(*made);

    const QStringList detected = serial->check_serial_ports();
    ...unchanged...
```

Add `#include <utility>` for `std::move`.

- [ ] **Step 4: Wire the bench**

In `src/platform/desktop/common/serial/BUILD.bazel`, set `desktop_serial_factory`'s `visibility = ["//apps/bench:__pkg__", "//apps/desktop:__pkg__"],`.

In `apps/bench/BUILD.bazel`, add `"//src/platform/desktop/common/serial:desktop_serial_factory",` to `fastecu-bench`'s `deps`.

In `apps/bench/main.cpp`, add `#include "src/platform/desktop/common/serial/desktop_serial_factory.h"` and a helper in the anonymous namespace:

```cpp
// fastecu-bench always drives the local adapter.
fastecu::flash::DesktopCanTransportConfig direct_transport_config()
{
    fastecu::flash::DesktopCanTransportConfig config;
    config.backend_factory = make_serial_backend_factory(DirectSerial{});
    return config;
}
```

Then `list_ports` returns `fastecu::flash::list_desktop_serial_ports(direct_transport_config());`, and `session` starts with `fastecu::flash::DesktopCanTransportConfig transport_config = direct_transport_config();` before setting `port_name`.

- [ ] **Step 5: Build and test**

Run: `bazel build --config=release //apps/bench:fastecu-bench && bazel test --config=release //src/platform/desktop/common/transport:all //apps/bench:all`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add -A src apps
git commit -m "refactor(transport): the CAN transport factory takes a backend factory (step 6e-1)

DesktopCanTransportConfig drops its peer fields and test seam for a
required backend_factory; fastecu-bench passes the direct one, as it
always used.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

### Task 4: The facade takes a required backend factory and forgets the peer

**Files:**
- Modify: `src/platform/desktop/common/serial/serial_port_actions.h:40-48,297-300`, `serial_port_actions.cpp:8-49`
- Modify: `src/ui/desktop/mainwindow.cpp:365`
- Modify: `src/platform/desktop/common/serial/desktop_serial_factory.cpp`, `src/platform/desktop/common/transport/desktop_transport_factory.cpp`
- Modify (test call sites): `src/ui/desktop/mainwindow_test.cpp`, `src/ui/desktop/service_functions/denso_tcu_read_preflight_test.cpp`, `src/ui/desktop/flash/operation/flash_operation_controller_test.cpp`, `src/platform/desktop/common/service_functions/serial_facade_configurator_test.cpp`, `src/platform/desktop/common/transport/fake_backed_serial.h`, `src/platform/desktop/common/transport/desktop_mixed_can_flash_transport_test.cpp`, `src/platform/desktop/common/flash/flash_workflow_test.cpp`, `src/platform/desktop/common/serial/facade_threading_test.cpp`, `src/platform/desktop/common/serial/serial_idle_test.cpp`, `src/platform/desktop/common/serial/testing/fake_backend_test.cpp`, `tests/serial_pty_e2e_test.cpp`, `tests/tst_mut_dma_integration.cpp`
- Test: `src/ui/desktop/mainwindow_test.cpp`

**Interfaces:**
- Consumes: `make_direct_serial_backend()` (Task 2).
- Produces: `explicit SerialPortActions(std::function<SerialBackend *()> backend_factory, QObject *parent = nullptr);` — no `isDirectConnection()`.

- [ ] **Step 1: Pin the direct startup path in `MainWindow`**

In `mainwindow_test.cpp`, add this slot (it passes today and must keep passing after the change):

```cpp
    // A direct session (no peer address) must never wait for a remote source.
    void directSessionStartupNeverWaitsForARemoteSource()
    {
        ModalDriver constructor_driver{QString()};
        constructor_driver.start();
        TestServices services{config_root_.path()};
        QVERIFY(services.serial != nullptr);
        EXPECT_CALL(*services.fake, waitForSource()).Times(0);
        MainWindow window{services.services()};
        constructor_driver.stop();
    }
```

Run: `bazel test --config=release //src/ui/desktop:test_mainwindow`
Expected: PASS.

- [ ] **Step 2: Change the constructor**

In `serial_port_actions.h`, replace the constructor declaration and delete `bool isDirectConnection(void);`:

```cpp
    // backend_factory builds the backend this facade drives; it is called
    // once, lazily, on the I/O thread. The composition root chooses it
    // (desktop_serial_factory.h); the facade never knows which it is.
    explicit SerialPortActions(std::function<SerialBackend *()> backend_factory, QObject *parent = nullptr);
```

Delete the members `QString peerAddress;`, `QString password;`, `QWebSocket *externalSocket = nullptr;` and the `class QWebSocket;` forward declaration added in Task 1. In `serial_port_actions.cpp`, also delete the `#include "serial_port_actions_direct.h"` added in Task 1.

In `serial_port_actions.cpp`, delete `#include "remote_serial_backend.h"` and `isDirectConnection()`, and replace the constructor with:

```cpp
SerialPortActions::SerialPortActions(std::function<SerialBackend *()> backend_factory, QObject *parent)
    : QObject{parent}, backendFactory(std::move(backend_factory))
{
}
```

In `mainwindow.cpp:365`, change `if (!serial->isDirectConnection())` to `if (!peerAddress.isEmpty())`.

In `desktop_serial_factory.cpp`, construct with `std::make_unique<SerialPortActions>(make_serial_backend_factory(connection))`. In `desktop_transport_factory.cpp`'s `make_serial`, `return std::make_unique<SerialPortActions>(config.backend_factory);`.

- [ ] **Step 3: Rewrite the fake-backend test call sites**

Every site passing an empty peer and a factory has `"", "", nullptr, nullptr, ` directly before the factory. Remove that prefix mechanically:

```bash
perl -0pi -e 's/"", "", nullptr, nullptr,\s*//g' \
  src/ui/desktop/service_functions/denso_tcu_read_preflight_test.cpp \
  src/ui/desktop/flash/operation/flash_operation_controller_test.cpp \
  src/platform/desktop/common/service_functions/serial_facade_configurator_test.cpp \
  src/platform/desktop/common/transport/fake_backed_serial.h \
  src/platform/desktop/common/transport/desktop_mixed_can_flash_transport_test.cpp \
  src/platform/desktop/common/flash/flash_workflow_test.cpp \
  src/platform/desktop/common/serial/facade_threading_test.cpp \
  src/platform/desktop/common/serial/serial_idle_test.cpp \
  src/platform/desktop/common/serial/testing/fake_backend_test.cpp
```

`mainwindow_test.cpp:190` passes a parent; edit it by hand to:

```cpp
    auto serial = std::make_unique<SerialPortActions>(
        [fake]() -> SerialBackend *
        {
            *fake = new NiceFakeBackend;
            return *fake;
        },
        parent);
```

- [ ] **Step 4: Rewrite the default-constructed sites**

`facade_threading_test.cpp:59` (never started, so the factory never runs):

```cpp
        SerialPortActions serial{[]() -> SerialBackend * { return nullptr; }}; // never used: the I/O thread must not start
```

`tests/serial_pty_e2e_test.cpp:86` and each of the six `SerialPortActions spad;` / `SerialPortActions closed;` in `tests/tst_mut_dma_integration.cpp`: add `#include "src/platform/desktop/common/serial/direct_serial_backend.h"`, add a file-local helper near the top of each file's anonymous namespace (create one if the file has none):

```cpp
// The real local backend, as the desktop app's DirectSerial connection builds it.
std::function<SerialBackend *()> directBackend()
{
    return [] { return make_direct_serial_backend().release(); };
}
```

and construct `SerialPortActions serial{directBackend()};` / `SerialPortActions spad{directBackend()};` / `SerialPortActions closed{directBackend()};`, keeping each line's trailing comment.

- [ ] **Step 5: Confirm nothing uses the old surface**

Run: `grep -rnE '"", "", nullptr|isDirectConnection|backend_factory_for_tests|backendFactoryForTests' --include='*.cpp' --include='*.h' src tests apps`
Expected: no output.

- [ ] **Step 6: Clang-format, build, test**

Run: `prek run --all-files && bazel test --config=release //...`
Expected: all PASS (clang-format may rewrap the edited lambdas; accept its output).

- [ ] **Step 7: Commit**

```bash
git add -A src tests apps
git commit -m "refactor(serial): SerialPortActions takes a required backend factory (step 6e-1)

The facade loses peerAddress, password, externalSocket and
isDirectConnection(); MainWindow keys the remote splash wait on the peer
address it already holds. Tests pass their fake or the real direct
backend explicitly.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```


### Task 5: Split the direct and remote backends into their own targets

Possible only now: until Task 4 the facade constructed both backends itself.

**Files:**
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`
- Modify: `src/platform/desktop/common/remote_utility/BUILD.bazel:26-30`
- Modify: `scripts/check-serial-compat-allowlist.py` (`FROZEN`)
- Modify: `tests/BUILD.bazel` (`serial_pty_e2e_test`, `mut_dma_integration_tests`, `serial_crash_tests`)
- Test: the whole suite (this task moves code between targets; no new behavior)

**Interfaces:**
- Consumes: `direct_serial_backend.{h,cpp}` (Task 2), the factory-only facade (Task 4).
- Produces: Bazel targets `//src/platform/desktop/common/serial:direct_serial_backend`, `:remote_serial_backend`, `:j2534_driver_selection`.

- [ ] **Step 1: Split the BUILD targets**

In `src/platform/desktop/common/serial/BUILD.bazel`, replace the `serial_qt_compat` rule with the four rules below (keep the file's other rules). The `serial_qt_compat` visibility list loses only the `remote_utility` entry and its comment block.

```starlark
# TRANSITIONAL. Carries serial_port_actions.h to its UI callers, all of
# which are layering violations step 6 removes. The visibility list below
# is frozen by scripts/check-serial-compat-allowlist.py. Do not add new
# callers.
qt_cc_library(
    name = "serial_qt_compat",
    srcs = [
        "serial_backend_host.cpp",
        "serial_port_actions.cpp",
    ],
    hdrs = ["serial_port_actions.h"],
    copts = COMMON_COPTS,
    normal_hdrs = [
        "serial_backend.h",
        "serial_backend_host.h",
        "serial_facade_codes.h",
    ],
    visibility = [
        # TRANSITIONAL. Every entry below is a layering violation removed by
        # step 6 (ui). Frozen by scripts/check-serial-compat-allowlist.py --
        # this list may shrink, never grow.
        "//src/platform/desktop/common/serial:__pkg__",
        "//src/platform/desktop/common/transport:__pkg__",
        "//src/ui/desktop:__pkg__",
        "//src/ui/desktop/biu:__pkg__",
        "//tests:__pkg__",
    ],
    deps = QT_DEPS + [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/qt_compat",
    ],
)

# isJ2534CapableEntry and the J2534 driver-registry helpers. Shared by the
# direct backend and the desktop CAN transport factory.
qt_cc_library(
    name = "j2534_driver_selection",
    srcs = [],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["j2534_driver_selection.h"],
    visibility = [
        "//src/platform/desktop/common/serial:__pkg__",
        "//src/platform/desktop/common/transport:__pkg__",
    ],
    deps = QT_DEPS,
)

# The local backend: QSerialPort plus the platform J2534 library. Reached
# through make_direct_serial_backend() (direct_serial_backend.h).
qt_cc_library(
    name = "direct_serial_backend",
    srcs = [
        "direct_serial_backend.cpp",
        "serial_port_actions_direct.cpp",
    ],
    hdrs = ["serial_port_actions_direct.h"],
    copts = COMMON_COPTS,
    normal_hdrs = ["direct_serial_backend.h"],
    visibility = [
        "//src/platform/desktop/common/serial:__pkg__",
        "//tests:__pkg__",
    ],
    deps = QT_DEPS + [
        ":j2534_driver_selection",
        ":serial_qt_compat",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/qt_compat",
    ] + select({
        # serial_port_actions_direct.h #includes J2534_unix.h or J2534_win.h
        # behind a Q_OS_UNIX/Q_OS_WIN32 guard; the header must be a declared
        # input on whichever platform is actually compiling it.
        "@platforms//os:windows": ["//src/platform/desktop/windows/j2534"],
        "//conditions:default": ["//src/platform/desktop/unix/j2534"],
    }),
)

# The remote backend (Qt Remote Objects over a websocket), plus the websocket
# device and QtRO helper that remote_utility also uses.
qt_cc_library(
    name = "remote_serial_backend",
    srcs = [
        "remote_serial_backend.cpp",
        "websocketiodevice.cpp",
    ],
    hdrs = [
        "remote_serial_backend.h",
        "websocketiodevice.h",
    ],
    copts = COMMON_COPTS,
    normal_hdrs = ["qtrohelper.hpp"],
    visibility = [
        "//src/platform/desktop/common/remote_utility:__pkg__",
        "//src/platform/desktop/common/serial:__pkg__",
    ],
    deps = QT_DEPS + [
        ":serial_qt_compat",
        ":serial_replicas",
    ],
)
```

Update the package's other rules:
- `desktop_serial_factory`: `deps = QT_DEPS + [":direct_serial_backend", ":remote_serial_backend", ":serial_qt_compat"],`
- `desktop_serial_factory_test`: `deps = [":desktop_serial_factory", ":direct_serial_backend", ":remote_serial_backend", ":serial_qt_compat"],`
- `test_direct_backend`: add `":direct_serial_backend",` to `deps`.
- `test_remote_backend_smoke`: `deps = [":remote_serial_backend"],`
- `test_direct_backend_pty`: add `":direct_serial_backend",` to `deps`.

In `src/platform/desktop/common/remote_utility/BUILD.bazel`, replace `"//src/platform/desktop/common/serial:serial_qt_compat",` with `"//src/platform/desktop/common/serial:remote_serial_backend",` (keep its comment).

In `src/platform/desktop/common/transport/BUILD.bazel`, add `"//src/platform/desktop/common/serial:j2534_driver_selection",` to `flash_transports`' `deps`.

In `tests/BUILD.bazel`, add `"//src/platform/desktop/common/serial:direct_serial_backend",` to the `deps` of `serial_pty_e2e_test`, `mut_dma_integration_tests`, and `serial_crash_tests`.

In `scripts/check-serial-compat-allowlist.py`, delete `"//src/platform/desktop/common/remote_utility:__pkg__",` from `FROZEN` and the comment paragraph above it that explains the `remote_utility` entry.

- [ ] **Step 2: Build and test**

Run: `bazel build -k --config=release //... && bazel test --config=release //...`
Expected: all PASS, including `//:serial_compat_allowlist` and `test_direct_backend`'s new case. If a file under `src/platform/desktop/common/serial/` that moved targets fails to find a header, give its target the dependency that owns the header — never widen `serial_qt_compat`'s visibility.

- [ ] **Step 3: Commit**

```bash
git add -A src tests scripts
git commit -m "refactor(serial): direct and remote backends get their own targets (step 6e-1)

serial_qt_compat keeps only the facade and backend host, so it no longer
depends on J2534 or the Remote Objects replicas. remote_utility depends on
:remote_serial_backend, which takes its entry off the frozen allowlist.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

- [ ] **Step 4: Gate the PR**

Run: `bazel run //:clang_tidy_report_changed`
Expected: no findings in changed lines; fix any without changing behavior in a new commit.

---

## PR 6e-2 — the J2534 OS split moves into BUILD-selected sources

Start: `git checkout -b refactor/step6e-2-j2534-os-split` (from the 6e-1 branch head).

### Task 6: One J2534 include path

**Files:**
- Create: `src/platform/desktop/unix/j2534/j2534_api.h`, `src/platform/desktop/windows/j2534/j2534_api.h`
- Modify: `src/platform/desktop/unix/j2534/BUILD.bazel`, `src/platform/desktop/windows/j2534/BUILD.bazel`
- Modify: `src/platform/desktop/common/serial/serial_port_actions_direct.h:24-28`, `src/platform/desktop/common/serial/BUILD.bazel` (`direct_serial_backend` deps)

**Interfaces:**
- Produces: include path `src/platform/desktop/j2534/j2534_api.h`, provided by `//src/platform/desktop/unix/j2534:j2534_api` or `//src/platform/desktop/windows/j2534:j2534_api`.

- [ ] **Step 1: Add the two headers**

`src/platform/desktop/unix/j2534/j2534_api.h`:

```cpp
#pragma once

// The platform J2534 API behind one include path,
// src/platform/desktop/j2534/j2534_api.h. The consumer's BUILD select()
// decides whether that path resolves to this header or the Windows one.
#include "src/platform/desktop/unix/j2534/J2534_unix.h"
```

`src/platform/desktop/windows/j2534/j2534_api.h`: the same text with `#include "src/platform/desktop/windows/j2534/J2534_win.h"`.

- [ ] **Step 2: Add the two targets**

In `src/platform/desktop/unix/j2534/BUILD.bazel`, add `load("@rules_cc//cc:cc_library.bzl", "cc_library")` to the loads and:

```starlark
# Publishes this platform's J2534 API at the shared path
# src/platform/desktop/j2534/j2534_api.h; see j2534_api.h.
cc_library(
    name = "j2534_api",
    hdrs = ["j2534_api.h"],
    include_prefix = "src/platform/desktop/j2534",
    strip_include_prefix = "/src/platform/desktop/unix/j2534",
    target_compatible_with = select({
        "@platforms//os:windows": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    deps = [":j2534"],
)
```

In `src/platform/desktop/windows/j2534/BUILD.bazel`:

```starlark
# Publishes this platform's J2534 API at the shared path
# src/platform/desktop/j2534/j2534_api.h; see j2534_api.h.
cc_library(
    name = "j2534_api",
    hdrs = ["j2534_api.h"],
    include_prefix = "src/platform/desktop/j2534",
    strip_include_prefix = "/src/platform/desktop/windows/j2534",
    target_compatible_with = WINDOWS_ONLY,
    deps = [":j2534"],
)
```

- [ ] **Step 3: Switch the direct backend to it**

In `serial_port_actions_direct.h`, replace

```cpp
#if defined Q_OS_UNIX
#include "src/platform/desktop/unix/j2534/J2534_unix.h"
#elif defined Q_OS_WIN32
#include "src/platform/desktop/windows/j2534/J2534_win.h"
#endif
```

with `#include "src/platform/desktop/j2534/j2534_api.h"`.

In `src/platform/desktop/common/serial/BUILD.bazel`, change `direct_serial_backend`'s `select()` entries to `"//src/platform/desktop/windows/j2534:j2534_api"` and `"//src/platform/desktop/unix/j2534:j2534_api"`, and replace its comment with `# The platform J2534 API behind src/platform/desktop/j2534/j2534_api.h.`

- [ ] **Step 4: Build and test**

Run: `bazel build --config=release //... && bazel test --config=release //src/platform/desktop/common/serial:all //tests:all`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add -A src
git commit -m "refactor(j2534): one include path for the platform J2534 API (step 6e-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

### Task 7: `isJ2534CapableEntry` per OS

**Files:**
- Create: `src/platform/desktop/common/serial/j2534_driver_selection_unix.cpp`, `j2534_driver_selection_windows.cpp`, `j2534_driver_selection_unix_test.cpp`, `j2534_driver_selection_windows_test.cpp`
- Modify: `src/platform/desktop/common/serial/j2534_driver_selection.h:12-29`, `direct_backend_test.cpp:23,105-126`, `src/platform/desktop/common/serial/BUILD.bazel`

**Interfaces:**
- Produces: `bool isJ2534CapableEntry(QStringView entry);` declared (no longer inline) in `j2534_driver_selection.h`.

- [ ] **Step 1: Move the assertions into per-OS test files**

`j2534_driver_selection_unix_test.cpp`:

```cpp
#include <QtTest>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

// Unix check_serial_ports() entries are "<portName> - <description>"; only the
// description tells an adapter from a Bluetooth or debug-console port (#243).
class TestJ2534DriverSelectionUnix : public QObject
{
    Q_OBJECT
  private slots:
    void capableEntry_matchesOnlyTheAdapterDescription()
    {
        QVERIFY(isJ2534CapableEntry(u"cu.usbmodemTApU_RJO1 - OpenPort 2.0"));
        QVERIFY(isJ2534CapableEntry(u"cu.usbmodem0 - openport 2.0")); // case-insensitive
        // macOS enumerates these ahead of the adapter; driving ISO-15765 over one
        // yields a timeout per exchange, never a response.
        QVERIFY(!isJ2534CapableEntry(u"cu.Bluetooth-Incoming-Port - "));
        QVERIFY(!isJ2534CapableEntry(u"cu.debug-console - "));
        QVERIFY(!isJ2534CapableEntry(u"ttyUSB0 - USB Serial"));
        QVERIFY(!isJ2534CapableEntry(u"ttyUSB0")); // no separator at all
        QVERIFY(!isJ2534CapableEntry(u""));
    }
};

QTEST_GUILESS_MAIN(TestJ2534DriverSelectionUnix)
#include "j2534_driver_selection_unix_test.moc"
```

`j2534_driver_selection_windows_test.cpp`:

```cpp
#include <QtTest>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

// Windows entries come from getAllJ2534DriversNames(), carry no description,
// and open_serial_port() drives every one of them through J2534 -- rejecting
// them here would regress Windows.
class TestJ2534DriverSelectionWindows : public QObject
{
    Q_OBJECT
  private slots:
    void capableEntry_acceptsEveryNonEmptyEntry()
    {
        QVERIFY(isJ2534CapableEntry(u"cu.usbmodemTApU_RJO1 - OpenPort 2.0"));
        QVERIFY(isJ2534CapableEntry(u"cu.usbmodem0 - openport 2.0"));
        QVERIFY(isJ2534CapableEntry(u"Tactrix Inc. - OpenPort 2.0 J2534 DLL"));
        QVERIFY(isJ2534CapableEntry(u"Acme J2534 DLL"));
        QVERIFY(!isJ2534CapableEntry(u""));
    }
};

QTEST_GUILESS_MAIN(TestJ2534DriverSelectionWindows)
#include "j2534_driver_selection_windows_test.moc"
```

Delete `j2534CapableEntry_matchesOnlyTheAdapterDescription` (declaration and definition) from `direct_backend_test.cpp`.

In the package BUILD add:

```starlark
fastecu_qttest(
    name = "j2534_driver_selection_unix_test",
    size = "small",
    src = "j2534_driver_selection_unix_test.cpp",
    target_compatible_with = select({
        "@platforms//os:windows": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    deps = [":j2534_driver_selection"],
)

fastecu_qttest(
    name = "j2534_driver_selection_windows_test",
    size = "small",
    src = "j2534_driver_selection_windows_test.cpp",
    target_compatible_with = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["@platforms//:incompatible"],
    }),
    deps = [":j2534_driver_selection"],
)
```

- [ ] **Step 2: Run the Unix test (passes against the inline version)**

Run: `bazel test --config=release //src/platform/desktop/common/serial:j2534_driver_selection_unix_test`
Expected: PASS.

- [ ] **Step 3: Split the definition**

In `j2534_driver_selection.h`, replace the inline `isJ2534CapableEntry` (keep its doc comment) with the declaration `bool isJ2534CapableEntry(QStringView entry);`.

`j2534_driver_selection_unix.cpp`:

```cpp
#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

bool isJ2534CapableEntry(QStringView entry)
{
    const qsizetype separator = entry.indexOf(QStringView(u" - "));
    return separator >= 0 && entry.sliced(separator + 3).contains(QStringView(u"OpenPort 2.0"), Qt::CaseInsensitive);
}
```

`j2534_driver_selection_windows.cpp`:

```cpp
#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

bool isJ2534CapableEntry(QStringView entry)
{
    return !entry.isEmpty();
}
```

In the package BUILD, give `j2534_driver_selection`:

```starlark
    srcs = select({
        "@platforms//os:windows": ["j2534_driver_selection_windows.cpp"],
        "//conditions:default": ["j2534_driver_selection_unix.cpp"],
    }),
```

- [ ] **Step 4: Build and test**

Run: `bazel test --config=release //src/platform/desktop/common/serial:all //src/platform/desktop/common/transport:all`
Expected: PASS; `j2534_driver_selection_windows_test` reports SKIPPED on macOS/Linux.

- [ ] **Step 5: Commit**

```bash
git add -A src
git commit -m "refactor(serial): per-OS isJ2534CapableEntry and its tests (step 6e-2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

### Task 8: Per-OS hooks replace every guard in the direct backend

**Files:**
- Create: `src/platform/desktop/common/serial/serial_port_actions_direct_unix.cpp`, `serial_port_actions_direct_windows.cpp`, `direct_backend_hooks_unix_test.cpp`, `direct_backend_hooks_windows_test.cpp`
- Modify: `src/platform/desktop/common/serial/serial_port_actions_direct.h:649-653,666-668,681-683` and the `protected:` section (~line 621)
- Modify: `src/platform/desktop/common/serial/serial_port_actions_direct.cpp` (guarded regions at 45, 363, 396, 437, 451-525, 531-545, 554-578, 605-609, 737, 1192, 1350, 1372, 1380, 1486, 1646)
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`

**Interfaces:**
- Produces (protected members of `SerialPortActionsDirect`):
  ```cpp
  struct ResolvedPort { QString port; bool is_j2534 = false; };
  void connect_j2534_logs();
  void settle_after_programming_voltage();
  void append_j2534_interfaces(QStringList& serial_ports);
  ResolvedPort resolve_port(const QString& entry) const;
  void select_j2534_dll();
  bool open_j2534_transport();
  void close_j2534_transport();
  void log_j2534_opened();
  void adopt_j2534_channel_id();
  bool j2534_tx_done();
  ```

- [ ] **Step 1: Write the failing hook tests**

`direct_backend_hooks_unix_test.cpp`:

```cpp
#include <QtTest>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Exposes the pure per-OS hooks of the direct backend.
class HookProbe : public SerialPortActionsDirect
{
  public:
    using SerialPortActionsDirect::append_j2534_interfaces;
    using SerialPortActionsDirect::resolve_port;
};

class TestDirectBackendHooksUnix : public QObject
{
    Q_OBJECT
  private slots:
    void resolvePort_prefixesAndSplitsAnAdapterEntry()
    {
        HookProbe probe;
        const auto resolved = probe.resolve_port("cu.usbmodem0 - OpenPort 2.0");
        QCOMPARE(resolved.port, QString("/dev/cu.usbmodem0"));
        QVERIFY(resolved.is_j2534);
    }

    // macOS lists this port ahead of the adapter; it must fall back to plain serial.
    void resolvePort_plainSerialEntryIsNotJ2534()
    {
        HookProbe probe;
        const auto bluetooth = probe.resolve_port("cu.Bluetooth-Incoming-Port - ");
        QCOMPARE(bluetooth.port, QString("/dev/cu.Bluetooth-Incoming-Port"));
        QVERIFY(!bluetooth.is_j2534);
        const auto usb = probe.resolve_port("ttyUSB0 - USB Serial");
        QCOMPARE(usb.port, QString("/dev/ttyUSB0"));
        QVERIFY(!usb.is_j2534);
    }

    void appendJ2534Interfaces_leavesTheListUntouched()
    {
        HookProbe probe;
        QStringList ports{"cu.usbmodem0 - OpenPort 2.0"};
        probe.append_j2534_interfaces(ports);
        QCOMPARE(ports, QStringList{"cu.usbmodem0 - OpenPort 2.0"});
    }
};

QTEST_GUILESS_MAIN(TestDirectBackendHooksUnix)
#include "direct_backend_hooks_unix_test.moc"
```

`direct_backend_hooks_windows_test.cpp`:

```cpp
#include <QtTest>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Exposes the pure per-OS hooks of the direct backend.
class HookProbe : public SerialPortActionsDirect
{
  public:
    using SerialPortActionsDirect::j2534_tx_done;
    using SerialPortActionsDirect::resolve_port;
};

class TestDirectBackendHooksWindows : public QObject
{
    Q_OBJECT
  private slots:
    // Windows entries are J2534 vendor names: no split, every non-empty one is J2534.
    void resolvePort_keepsTheVendorNameWhole()
    {
        HookProbe probe;
        const auto resolved = probe.resolve_port("Tactrix Inc. - OpenPort 2.0 J2534 DLL");
        QCOMPARE(resolved.port, QString("Tactrix Inc. - OpenPort 2.0 J2534 DLL"));
        QVERIFY(resolved.is_j2534);
    }

    void resolvePort_emptyEntryIsNotJ2534()
    {
        HookProbe probe;
        QVERIFY(!probe.resolve_port("").is_j2534);
    }

    void txDone_isAlwaysTrue()
    {
        HookProbe probe;
        QVERIFY(probe.j2534_tx_done());
    }
};

QTEST_GUILESS_MAIN(TestDirectBackendHooksWindows)
#include "direct_backend_hooks_windows_test.moc"
```

In the package BUILD add (same `target_compatible_with` selects as Task 7):

```starlark
fastecu_qttest(
    name = "direct_backend_hooks_unix_test",
    size = "small",
    src = "direct_backend_hooks_unix_test.cpp",
    target_compatible_with = select({
        "@platforms//os:windows": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    deps = [":direct_serial_backend"],
)

fastecu_qttest(
    name = "direct_backend_hooks_windows_test",
    size = "small",
    src = "direct_backend_hooks_windows_test.cpp",
    target_compatible_with = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["@platforms//:incompatible"],
    }),
    deps = [":direct_serial_backend"],
)
```

- [ ] **Step 2: Run to verify it fails**

Run: `bazel test --config=release //src/platform/desktop/common/serial:direct_backend_hooks_unix_test`
Expected: FAIL to compile — `resolve_port` is not a member of `SerialPortActionsDirect`.

- [ ] **Step 3: Declare the hooks and clean the header's guards**

In `serial_port_actions_direct.h`:
- Replace the two-branch `protocol` block (lines 649-653) with `unsigned int protocol = ISO9141;`.
- Remove the `#ifdef Q_OS_WIN32` / `#endif` around `QMap<QString, QString> installed_drivers;` (keep the member; it stays empty on Unix).
- Remove the `#if defined(_WIN32) || …` / `#endif` around `QMap<QString, QString> getAllJ2534DriversNames();` (keep the declaration; it is defined only in the Windows file and called only there).
- In the `protected:` section after `J2534 *j2534;`, add:

```cpp
    // Per-OS hooks. Declared once here and defined in exactly one of
    // serial_port_actions_direct_unix.cpp / serial_port_actions_direct_windows.cpp,
    // which the BUILD file selects; each replaces one former Q_OS_* branch.
    // Protected so tests can pin the pure ones.
    struct ResolvedPort
    {
        QString port;
        bool is_j2534 = false;
    };
    void connect_j2534_logs();
    void settle_after_programming_voltage();
    void append_j2534_interfaces(QStringList& serial_ports);
    ResolvedPort resolve_port(const QString& entry) const;
    void select_j2534_dll();
    bool open_j2534_transport();
    void close_j2534_transport();
    void log_j2534_opened();
    void adopt_j2534_channel_id();
    bool j2534_tx_done();
```

- [ ] **Step 4: Replace each guarded region in `serial_port_actions_direct.cpp` with its hook call**

Make exactly these substitutions (line numbers from `master` at `f965386e`; locate by the quoted code):

1. Constructor (45-50) and the J2534 re-create path (737-742): replace each `#if defined Q_OS_UNIX` … four `QObject::connect(j2534, &J2534::LOG_*` lines … `#endif` block with `connect_j2534_logs();`.
2. Both LEC toggles (363-365, 396-398): replace `#if defined Q_OS_UNIX` / `delay(1);` / `#endif` with `settle_after_programming_voltage();`.
3. `check_serial_ports` (437-446): replace the `#if defined(_WIN32) …` block with `append_j2534_interfaces(serial_ports);`.
4. Cut lines 451-525 — the whole `#if defined(_WIN32) …` block holding the anonymous namespace (`kJ2534RegistryKey`, `readJ2534RegistryView`), `check_j2534_devices`, and `getAllJ2534DriversNames` — and paste it, without the `#if`/`#endif` lines, into the Windows file (Step 6).
5. `open_serial_port` head (529-545): replace everything from `// QString serial_port_text = serial_port_list.at(1);` through the `#endif` after the J2534 `if` condition with:

```cpp
    // QString serial_port_text = serial_port_list.at(1);
    const ResolvedPort resolved = resolve_port(serial_port_list.at(0));
    serial_port = resolved.port;
    emit LOG_D("Interface: " + serial_port, true, true);

    if (!serial_port.isEmpty() && resolved.is_j2534)
```

6. Inside that `if`, replace the `#if defined(_WIN32) …` DLL block (554-578) with `select_j2534_dll();`, keeping `J2534_is_denso_dsti = serial_port.contains("DST-i");` and the commented `user_j2534_drivers` line above it.
7. Delete the commented-out prefix block (605-609: `#if defined Q_OS_UNIX` / `// serial_port = serial_port_prefix_linux + serial_port;` / `#elif …` / `// serial_port = serial_port_prefix_win + serial_port;` / `#endif`).
8. `get_is_tx_done` (1192-1196): body becomes `return j2534_tx_done();`.
9. `init_j2534_connection`: replace 1349-1355 (`// If Linux, open serial port` + guarded `open_serial_port` check) with

```cpp
    // If Linux, open serial port
    if (!open_j2534_transport())
    {
        return STATUS_ERROR;
    }
```

   replace the guarded `j2534->close_serial_port();` (1372-1374) with `close_j2534_transport();`, and the guarded `LOG_D("INIT: J2534 opened with devID: " …)` (1380-1382) with `log_j2534_opened();`.
10. First connect site (1486-1488): replace the guarded `chanID = protocol;` with `adopt_j2534_channel_id();` (keep the commented `LOG_D` line after it).
11. Second connect site (1646-1655): replace the whole `#if defined Q_OS_WIN32 … #endif` with

```cpp
        adopt_j2534_channel_id();
        emit LOG_D("Connected: DevID " + QString::number(devID) + ", protocol " + QString::number(protocol) +
                       ", baudrate " + QString::number(baudrate) + ", chanID " + QString::number(chanID),
                   true, true);
```

Remove includes the common file no longer needs only if the build proves them unused; `j2534_driver_selection.h` stays (the Windows file includes it too).

- [ ] **Step 5: Write the Unix hook file**

`serial_port_actions_direct_unix.cpp`:

```cpp
// Unix bodies of SerialPortActionsDirect's per-OS hooks; see their
// declaration in serial_port_actions_direct.h. The BUILD file compiles
// exactly one of this file and serial_port_actions_direct_windows.cpp.
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

void SerialPortActionsDirect::connect_j2534_logs()
{
    QObject::connect(j2534, &J2534::LOG_E, this, &SerialPortActionsDirect::LOG_E);
    QObject::connect(j2534, &J2534::LOG_W, this, &SerialPortActionsDirect::LOG_W);
    QObject::connect(j2534, &J2534::LOG_I, this, &SerialPortActionsDirect::LOG_I);
    QObject::connect(j2534, &J2534::LOG_D, this, &SerialPortActionsDirect::LOG_D);
}

void SerialPortActionsDirect::settle_after_programming_voltage()
{
    delay(1);
}

void SerialPortActionsDirect::append_j2534_interfaces(QStringList& /*serial_ports*/)
{
    // A Unix adapter enumerates as a serial port; there is no driver registry.
}

SerialPortActionsDirect::ResolvedPort SerialPortActionsDirect::resolve_port(const QString& entry) const
{
    const QString prefixed = serial_port_prefix_linux + entry;
    return {.port = prefixed.split(" - ").at(0), .is_j2534 = isJ2534CapableEntry(prefixed)};
}

void SerialPortActionsDirect::select_j2534_dll()
{
    // The Unix J2534 drives the adapter's serial port; there is no DLL to pick.
}

bool SerialPortActionsDirect::open_j2534_transport()
{
    return j2534->open_serial_port(serial_port) == serial_port;
}

void SerialPortActionsDirect::close_j2534_transport()
{
    j2534->close_serial_port();
}

void SerialPortActionsDirect::log_j2534_opened()
{
    emit LOG_D("INIT: J2534 opened with devID: " + QString::number(devID), true, true);
}

void SerialPortActionsDirect::adopt_j2534_channel_id()
{
    chanID = protocol;
}

bool SerialPortActionsDirect::j2534_tx_done()
{
    return j2534->get_is_tx_done();
}
```

- [ ] **Step 6: Write the Windows hook file**

`serial_port_actions_direct_windows.cpp` — the pasted block from Step 4 item 4 goes where marked; `select_j2534_dll` and `append_j2534_interfaces` bodies are the old lines moved verbatim:

```cpp
// Windows bodies of SerialPortActionsDirect's per-OS hooks; see their
// declaration in serial_port_actions_direct.h. The BUILD file compiles
// exactly one of this file and serial_port_actions_direct_unix.cpp.
#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

#include <QSettings>

#include <algorithm>
#include <functional>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

// ---- moved verbatim from serial_port_actions_direct.cpp (old lines 452-524):
// namespace { kJ2534RegistryKey; readJ2534RegistryView }
// SerialPortActionsDirect::check_j2534_devices(...)
// SerialPortActionsDirect::getAllJ2534DriversNames()
// ----

void SerialPortActionsDirect::connect_j2534_logs()
{
    // The Windows J2534 is not a QObject and emits no log signals.
}

void SerialPortActionsDirect::settle_after_programming_voltage()
{
}

void SerialPortActionsDirect::append_j2534_interfaces(QStringList& serial_ports)
{
    QStringList j2534_interfaces;
    installed_drivers = getAllJ2534DriversNames();
    for (const QString installed_vendor : installed_drivers.keys())
    {
        j2534_interfaces.append(installed_vendor);
    }
    std::sort(j2534_interfaces.begin(), j2534_interfaces.end(), std::less<QString>());
    serial_ports.append(j2534_interfaces);
}

SerialPortActionsDirect::ResolvedPort SerialPortActionsDirect::resolve_port(const QString& entry) const
{
    const QString port = serial_port_prefix_win + entry;
    return {.port = port, .is_j2534 = !port.isEmpty()};
}

void SerialPortActionsDirect::select_j2534_dll()
{
    QString localDllName;
    QString installedDllName;

    QStringList dllName = installed_drivers.value(serial_port).split("\\");
    localDllName = dllName.at(dllName.count() - 1);
    installedDllName = installed_drivers.value(serial_port);
    emit LOG_D("Local DLL Name: " + localDllName, true, true);
    emit LOG_D("Installed DLL Name: " + installedDllName, true, true);

    QMap<QString, QString> user_j2534_drivers;

    emit LOG_D("Opening device: " + serial_port, true, true);
    QStringList j2534_driver;
    user_j2534_drivers[serial_port] = localDllName;
    j2534_driver = check_j2534_devices(user_j2534_drivers);
    user_j2534_drivers[serial_port] = installedDllName;
    if (j2534_driver.isEmpty())
        j2534_driver = check_j2534_devices(user_j2534_drivers);
    const QString resolvedDllName = resolveJ2534DllForConnection(serial_port, installedDllName, j2534_driver);
    if (!resolvedDllName.isEmpty())
        j2534->setDllName(resolvedDllName.toLocal8Bit().data());
    else
        emit LOG_D("Initializing interface failed!", true, true);
}

bool SerialPortActionsDirect::open_j2534_transport()
{
    return true; // the Windows J2534 loads a DLL in init(); no port to open first
}

void SerialPortActionsDirect::close_j2534_transport()
{
}

void SerialPortActionsDirect::log_j2534_opened()
{
}

void SerialPortActionsDirect::adopt_j2534_channel_id()
{
}

bool SerialPortActionsDirect::j2534_tx_done()
{
    return true;
}
```

Replace the `// ---- moved verbatim …` comment block with the actual cut text. Before committing, diff each moved body against master to prove it is byte-identical apart from indentation:

```bash
git show master:src/platform/desktop/common/serial/serial_port_actions_direct.cpp | sed -n '437,446p;452,524p;554,578p'
```

- [ ] **Step 7: Select the hook file in the BUILD**

In `direct_serial_backend`, set:

```starlark
    srcs = [
        "direct_serial_backend.cpp",
        "serial_port_actions_direct.cpp",
    ] + select({
        "@platforms//os:windows": ["serial_port_actions_direct_windows.cpp"],
        "//conditions:default": ["serial_port_actions_direct_unix.cpp"],
    }),
```

- [ ] **Step 8: Run the done-check grep**

Run: `grep -nE 'Q_OS_|\bWIN32\b|_WIN64' src/platform/desktop/common/serial/*.h src/platform/desktop/common/serial/*.cpp | grep -v '_test.cpp'`
Expected: no output.

- [ ] **Step 9: Build and test**

Run: `prek run --all-files && bazel test --config=release //...`
Expected: all PASS, including `direct_backend_hooks_unix_test`, `test_direct_backend_pty`, `//tests:serial_crash_tests`, `//tests:serial_pty_e2e_test`, `//tests:mut_dma_integration_tests`.

- [ ] **Step 10: Commit**

```bash
git add -A src
git commit -m "refactor(serial): per-OS hooks replace the direct backend's OS guards (step 6e-2)

Each former Q_OS_*/WIN32 branch becomes a named hook defined in
serial_port_actions_direct_unix.cpp or _windows.cpp, selected by the
BUILD file. Hook bodies are the old lines moved verbatim; the Windows
J2534 discovery code moves wholesale.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

- [ ] **Step 11: Gate the PR on the full CI matrix**

Run: `bazel run //:clang_tidy_report_changed`, fix findings in new files without changing behavior (commit separately), then push the branch and wait for the Windows leg of CI to pass before asking for review. A Windows compile failure means a hook body or include went to the wrong file — fix it by moving code, never by adding a guard.

---

## PR 6e-3 — the link-time selection moves into `apps/desktop`; close-out

Start: `git checkout -b refactor/step6e-3-link-select` (from the 6e-2 branch head).

### Task 9: The binaries name the platform's direct backend

**Files:**
- Modify: `src/platform/desktop/common/serial/BUILD.bazel`, `apps/desktop/BUILD.bazel`, `apps/bench/BUILD.bazel`, `tests/BUILD.bazel`

**Interfaces:**
- Produces: targets `//src/platform/desktop/common/serial:direct_serial_backend_api` (header `direct_serial_backend.h`), `:direct_serial_backend_unix`, `:direct_serial_backend_windows`, test alias `:direct_serial_backend_for_tests`; `//apps/desktop:direct_serial_backend` alias.

- [ ] **Step 1: Split the target**

In `src/platform/desktop/common/serial/BUILD.bazel`, replace `direct_serial_backend` with:

```starlark
# make_direct_serial_backend(), declared without an implementation. A binary
# links exactly one of :direct_serial_backend_unix / _windows beside it; the
# binary's own select() names the platform.
qt_cc_library(
    name = "direct_serial_backend_api",
    srcs = [],
    hdrs = [],
    copts = COMMON_COPTS,
    normal_hdrs = ["direct_serial_backend.h"],
    visibility = ["//src/platform/desktop/common/serial:__pkg__"],
    deps = QT_DEPS,
)

DIRECT_SERIAL_BACKEND_VISIBILITY = [
    "//apps/bench:__pkg__",
    "//apps/desktop:__pkg__",
    "//src/platform/desktop/common/serial:__pkg__",
    "//tests:__pkg__",
]

qt_cc_library(
    name = "direct_serial_backend_unix",
    srcs = [
        "direct_serial_backend.cpp",
        "serial_port_actions_direct.cpp",
        "serial_port_actions_direct_unix.cpp",
    ],
    hdrs = ["serial_port_actions_direct.h"],
    copts = COMMON_COPTS,
    normal_hdrs = ["direct_serial_backend.h"],
    target_compatible_with = select({
        "@platforms//os:windows": ["@platforms//:incompatible"],
        "//conditions:default": [],
    }),
    visibility = DIRECT_SERIAL_BACKEND_VISIBILITY,
    deps = QT_DEPS + [
        ":j2534_driver_selection",
        ":serial_qt_compat",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/qt_compat",
        "//src/platform/desktop/unix/j2534:j2534_api",
    ],
)

qt_cc_library(
    name = "direct_serial_backend_windows",
    srcs = [
        "direct_serial_backend.cpp",
        "serial_port_actions_direct.cpp",
        "serial_port_actions_direct_windows.cpp",
    ],
    hdrs = ["serial_port_actions_direct.h"],
    copts = COMMON_COPTS,
    normal_hdrs = ["direct_serial_backend.h"],
    target_compatible_with = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["@platforms//:incompatible"],
    }),
    visibility = DIRECT_SERIAL_BACKEND_VISIBILITY,
    deps = QT_DEPS + [
        ":j2534_driver_selection",
        ":serial_qt_compat",
        "//src/algorithms/protocol",
        "//src/algorithms/protocol/qt_compat",
        "//src/platform/desktop/windows/j2534:j2534_api",
    ],
)

# For tests that drive the real direct backend. Production binaries select
# the implementation themselves (apps/desktop, apps/bench).
alias(
    name = "direct_serial_backend_for_tests",
    testonly = True,
    actual = select({
        "@platforms//os:windows": ":direct_serial_backend_windows",
        "//conditions:default": ":direct_serial_backend_unix",
    }),
    visibility = ["//src/platform/desktop/common/serial:__pkg__", "//tests:__pkg__"],
)
```

`desktop_serial_factory`'s deps: replace `":direct_serial_backend"` with `":direct_serial_backend_api"`. Every test in the package that listed `":direct_serial_backend"` (`test_direct_backend`, `test_direct_backend_pty`, `desktop_serial_factory_test`, `direct_backend_hooks_unix_test`, `direct_backend_hooks_windows_test`) now lists `":direct_serial_backend_for_tests"` instead. In `tests/BUILD.bazel`, the three suites switch to `//src/platform/desktop/common/serial:direct_serial_backend_for_tests`.

- [ ] **Step 2: Select in the binaries**

In `apps/desktop/BUILD.bazel`, add:

```starlark
# The platform choice for the local serial backend, named by the binary.
alias(
    name = "direct_serial_backend",
    actual = select({
        "@platforms//os:windows": "//src/platform/desktop/common/serial:direct_serial_backend_windows",
        "//conditions:default": "//src/platform/desktop/common/serial:direct_serial_backend_unix",
    }),
    visibility = ["//visibility:private"],
)
```

Add `":direct_serial_backend",` to the `deps` of `fastecu` and `desktop_composition_test`, and replace `fastecu`'s comment block ("main.cpp includes no J2534 header, …") with `# :direct_serial_backend picks the platform's local serial backend.`

In `apps/bench/BUILD.bazel`, add the same alias (named `direct_serial_backend`, same `select()`) and add `":direct_serial_backend",` to `fastecu-bench`'s `deps`.

- [ ] **Step 3: Build and test**

Run: `bazel build --config=release //:fastecu //apps/bench:fastecu-bench && bazel test --config=release //...`
Expected: all PASS.

- [ ] **Step 4: Confirm the facade chain no longer carries J2534**

Run: `bazel query 'somepath(//src/ui/desktop, //src/platform/desktop/unix/j2534:j2534)'`
Expected: empty — the UI reaches J2534 only through a binary's alias.

- [ ] **Step 5: Commit**

```bash
git add -A src apps tests
git commit -m "refactor(desktop): the binaries select the platform's direct serial backend (step 6e-3)

desktop_serial_factory links only make_direct_serial_backend()'s
declaration; fastecu and fastecu-bench each select the Unix or Windows
implementation, and tests use :direct_serial_backend_for_tests.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

- [ ] **Step 6: Prove a missing implementation fails loudly (then revert)**

Temporarily remove `":direct_serial_backend",` from `desktop_composition_test`'s `deps` and run `bazel test --config=release //apps/desktop:desktop_composition_test`.
Expected: link FAIL with an undefined `make_direct_serial_backend()`. Restore the line (`git checkout apps/desktop/BUILD.bazel`) and confirm it passes again. Nothing to commit.

### Task 10: Close out step 6e

**Files:**
- Create: `docs/platform-selection-bench-checklist.md`
- Modify: `docs/modularization-plan.md` (Status section; step 6 list), `docs/tech-debt.md` ("Drain the `serial_qt_compat` allowlist"), `docs/design-notes.md` (new "Platform selection" section after "Flash-operation dispatch")
- Delete: `docs/superpowers/specs/2026-09-26-step6e-platform-selection-design.md`, `docs/superpowers/plans/2026-09-26-step6e-platform-selection.md`

- [ ] **Step 1: Write the bench checklist**

```markdown
# Platform selection -- bench verification checklist

Step 6e moved the serial layer's direct/remote choice into the desktop
composition root and split the direct backend's Unix and Windows J2534 code
into per-OS source files. No wire bytes changed, but the J2534 open path was
reorganized on both operating systems and the Windows half is covered only
by CI. None of the items below is qualified until it is run and signed off.
Run `bazel test --config=release //...` first.

## macOS / Linux (OpenPort 2.0)

- [ ] `fastecu-bench ports` lists the OpenPort 2.0 entry.
- [ ] `fastecu-bench connect` succeeds against a bench ECU.
- [ ] In the desktop app, select the OpenPort 2.0 and start MUT/DMA logging;
      gauges update, and Stop ends the session cleanly.
- [ ] With a Bluetooth or debug-console port listed first, the app still
      opens the adapter, not the first port.

## Windows (J2534 DLL)

- [ ] The port list shows the installed J2534 vendor names after the serial
      ports.
- [ ] Selecting the OpenPort 2.0 vendor connects ("J2534: Interface opened
      succesfully!" in the log).

## Remote session

- [ ] `fastecu --host <peer> --password <pw>` shows the network splash and
      connects to a running remote peer. If no peer is available, record
      "unverified" here.

## Sign-off

- [ ] Date: ________________
- [ ] Adapter / OS: ________________
- [ ] Operator initials: ________________
```

- [ ] **Step 2: Update the plan, roadmap, and design notes**

`docs/modularization-plan.md`: in the Status section, change "6c (desktop composition root), and 6d (flash-operation dispatch) are complete" to include "6e (platform selection)"; in step 6's list replace the "Move platform selection into `apps/desktop`" bullet with:

```markdown
   - **6e platform selection — complete.** `SerialPortActions` takes a
     required backend factory and no longer knows direct from remote;
     `desktop_serial_factory`'s `SerialConnection` and the composition
     root's `serial_connection_from_args` own that rule, and the CAN
     transport factory takes a backend factory too. The direct backend's
     Unix/Windows differences are named hooks in BUILD-selected
     `serial_port_actions_direct_{unix,windows}.cpp` files, reached through
     one `j2534_api.h` include path, and the `fastecu` and `fastecu-bench`
     binaries select the implementation. `remote_utility` left the
     `serial_qt_compat` allowlist, and `STATUS_*` has one definition. Three
     PRs (6e-1 to 6e-3; fill in the numbers). See the
     [design notes](design-notes.md#platform-selection) and the
     [platform-selection bench checklist](platform-selection-bench-checklist.md).
```

`docs/tech-debt.md`, "P1: Drain the `serial_qt_compat` allowlist": change "It currently holds 6 entries" to "It currently holds 5 entries", delete the paragraph beginning "One entry, `//src/platform/desktop/common/remote_utility`, is not debt", and change the last action to "Delete `serial_qt_compat` once only `//tests` remains, and fold its sources into the owning packages."

`docs/design-notes.md`, new section:

```markdown
## Platform selection

### The composition root owns the direct/remote rule

`SerialPortActions` receives a backend factory and never learns which
backend it drives. `serial_connection_from_args` (`apps/desktop`) maps an
empty `--host` to `DirectSerial`, and `make_serial_backend_factory`
(`desktop_serial_factory`) builds the matching backend. Before step 6e the
rule lived in the facade's constructor and again in the CAN transport
factory; now a new platform, such as step 7's Android build, supplies its
own factory without touching the facade.

### Per-OS hooks sit behind one guard-free header

Each former `Q_OS_*` branch in the direct backend is a protected member
declared once in `serial_port_actions_direct.h` and defined in exactly one
of `serial_port_actions_direct_unix.cpp` / `_windows.cpp`. Hook bodies were
moved verbatim, so a Windows regression shows up as a compile failure in
the wrong file rather than as changed behavior. Both J2534 packages publish
their API at `src/platform/desktop/j2534/j2534_api.h`, so the common code
has no guarded include.

### The binary names the platform

`desktop_serial_factory` links only the declaration of
`make_direct_serial_backend()`. `fastecu` and `fastecu-bench` each carry a
`select()` alias picking `direct_serial_backend_unix` or `_windows`; tests
use `direct_serial_backend_for_tests`. A target that forgets the
implementation fails to link.

### `STATUS_*` stay macros

`serial_facade_codes.h` keeps `STATUS_SUCCESS`/`STATUS_ERROR` and
`SERIAL_P*` as macros: the Windows SDK's `ntstatus.h` defines
`STATUS_SUCCESS` as a macro, which would break a constexpr of that name.
```

- [ ] **Step 3: Delete the spec and this plan**

```bash
git rm docs/superpowers/specs/2026-09-26-step6e-platform-selection-design.md docs/superpowers/plans/2026-09-26-step6e-platform-selection.md
```

- [ ] **Step 4: Check the docs**

Run: `prek run --all-files`
Expected: PASS (lychee checks the new links).

- [ ] **Step 5: Commit**

```bash
git add -A docs
git commit -m "docs: close out step 6e (platform selection)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc"
```

- [ ] **Step 6: Publish the stack**

After the user authorizes pushing:

```bash
gh stack init --base master docs/step6e-platform-selection-spec refactor/step6e-1-serial-connection refactor/step6e-2-j2534-os-split refactor/step6e-3-link-select
gh stack submit --auto
```

Each PR description ends with:

```
🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01RdgamLUa9uyTg3StdCsVPc
```

Fill the PR numbers into the modularization-plan bullet with a follow-up commit on the top branch.
