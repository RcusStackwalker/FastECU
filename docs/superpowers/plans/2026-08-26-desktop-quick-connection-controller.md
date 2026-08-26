# Desktop Quick Connection Service and Controller Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Connect a validated dashboard document to a user-selected local J2534 or SocketCAN adapter and expose explicit Connect/Disconnect state through the QtQuick shell.

**Architecture:** Add normalized local-adapter providers and a connection service that returns either a complete owned `LoggingRun`, a typed selection request, or an error. Keep worker lifecycle and presentation mapping in `DashboardConnectionController`, which owns `LoggingEngine` and exposes only Qt properties, candidate rows, and actions to QML.

**Tech Stack:** C++23, Bazel, Qt 6.8.3 Core/QML/Quick/QtTest, Linux SocketCAN APIs, GoogleTest, existing `Result`, dashboard session builder, CDBG protocol, `SerialPortActions`, `FastEcuCanTransport`, and `LoggingEngine`.

**Spec:** `docs/superpowers/specs/2026-08-26-desktop-quick-connection-controller-design.md`

## Global Constraints

- Hardware connection always requires an explicit Connect action; document restoration must never connect automatically.
- Discover and open local J2534 and SocketCAN adapters only. Exclude generic serial and all remote/WebSocket/QtRO modes.
- Auto-select a preferred adapter only when kind, normalized vendor, and normalized display name match exactly one current candidate.
- Candidate IDs are opaque, process-local, generation-bound, and never serialized.
- A user-selected adapter remains a session override and never mutates `DashboardDocument`.
- Apply bitrate, 11/29-bit mode, raw-CAN mode, and reply filtering exactly; never round, substitute, or silently fall back.
- Return either a fully owned run or no open adapter. Every rejected or failed path must release hardware.
- Keep `SerialPortActions`, transport, protocol, and writable ECU APIs out of QML.
- Do not add card/value presentation, document loading/saving, remote adapters, generic serial support, or automatic reconnect after terminal failure.
- Preserve the Widgets application's fixed Colt setup, wire traffic, and build behavior.
- Preserve unrelated working-tree changes in dashboard codec files and untracked artifacts.

---

## File structure

### Desktop connection platform

- `src/platform/desktop/common/connection/local_adapter.h`: normalized descriptor, candidate selection, discovery generation, and provider/opened-adapter contracts.
- `src/platform/desktop/common/connection/local_adapter_matching.{h,cpp}`: text normalization and exact preference resolution without Qt or hardware access.
- `src/platform/desktop/common/connection/j2534_adapter_provider.{h,cpp}`: direct local discovery through `SerialPortActions`, filtering, normalized descriptors, and owned J2534/OpenPort opening.
- `src/platform/desktop/common/connection/socketcan_adapter_provider.{h,cpp}`: Linux CAN-interface enumeration and owned socket opening; returns no candidates on non-Linux builds.
- `src/platform/desktop/common/connection/socketcan_transport.{h,cpp}`: `ICanTransport` implementation over one owned nonblocking Linux CAN raw socket.
- `src/platform/desktop/common/connection/desktop_connection_service.{h,cpp}`: session preparation, discovery-generation management, resolution, exact setup, protocol construction, and typed outcomes.
- `src/platform/desktop/common/connection/*_test.cpp`: focused matching, provider/transport, and service contracts.
- `src/platform/desktop/common/connection/BUILD.bazel`: narrow libraries and tests with Linux-only source selection.

### Existing compatibility components

- `src/platform/desktop/common/logging/cdbg_serial_setup.{h,cpp}`: replace fixed boolean actions with validated profile-driven configuration.
- `src/platform/desktop/common/logging/legacy_logging_protocol_factory.cpp`: supply Colt defaults through the generalized setup API.
- `src/platform/desktop/common/logging/legacy_logging_protocol_factory_test.cpp`: prove existing ordering and values remain unchanged.
- `src/platform/desktop/common/transport/fastecu_can_transport.{h,cpp}`: optionally own `SerialPortActions` so a prepared run has complete adapter lifetime.

### QtQuick presentation

- `src/ui/desktop-quick/dashboard/dashboard_connection_controller.{h,cpp}`: connection state machine, engine ownership, QML actions, and error mapping.
- `src/ui/desktop-quick/dashboard/dashboard_connection_controller_test.cpp`: service/engine-seam state tests.
- `src/ui/desktop-quick/qml/dashboard/ConnectionPanel.qml`: Connect/Disconnect, state, selection, refresh, and error detail.
- `src/ui/desktop-quick/qml/shell/ApplicationShell.qml`: host the connection panel.
- `src/ui/desktop-quick/desktop_quick_application.{h,cpp}`: register/inject the controller into QML without loading hardware.
- `apps/desktop-quick/main.cpp`: compose production providers, service, engine controller, and the QML engine.

---

### Task 1: Normalize descriptors and resolve preferred adapters

**Files:**
- Create: `src/platform/desktop/common/connection/local_adapter.h`
- Create: `src/platform/desktop/common/connection/local_adapter_matching.h`
- Create: `src/platform/desktop/common/connection/local_adapter_matching.cpp`
- Create: `src/platform/desktop/common/connection/local_adapter_matching_test.cpp`
- Create: `src/platform/desktop/common/connection/BUILD.bazel`

**Interfaces:**
- Consumes: `fastecu::dashboard::AdapterKind`, `PreferredAdapter`, `Result`, and `Error`.
- Produces: `LocalAdapterDescriptor`, `AdapterDiscoverySnapshot`, `SelectedAdapter`, `ILocalAdapterProvider`, `normalize_adapter_text()`, and `resolve_preferred_adapter()`.

- [ ] **Step 1: Write matching tests before the implementation**

Create table and behavior tests with these concrete cases:

```cpp
TEST(LocalAdapterMatching, NormalizesCaseAndSurroundingWhitespace)
{
    EXPECT_EQ(normalize_adapter_text("  Tactrix Inc.  "), "tactrix inc.");
}

TEST(LocalAdapterMatching, ResolvesOnlyOneExactThreeFieldMatch)
{
    const PreferredAdapter preferred{AdapterKind::J2534, " TACTRIX ", "OpenPort 2.0"};
    const std::vector<LocalAdapterDescriptor> candidates{
        descriptor("j0", AdapterKind::J2534, "Tactrix", "OpenPort 2.0"),
        descriptor("j1", AdapterKind::J2534, "Other", "OpenPort 2.0"),
    };
    const auto result = resolve_preferred_adapter(preferred, candidates);
    ASSERT_EQ(result.kind, AdapterResolutionKind::UniqueMatch);
    EXPECT_EQ(result.candidate_id, "j0");
}
```

Also prove that absent preference, zero matches, two identical matches, kind mismatch, and substring-only text all return `SelectionRequired`.

- [ ] **Step 2: Run the absent target and confirm the RED state**

Run: `bazel test //src/platform/desktop/common/connection:test_local_adapter_matching --test_output=errors`

Expected: FAIL because the package and matching types do not exist.

- [ ] **Step 3: Add the provider and descriptor contracts**

Define the exact public values in `local_adapter.h`:

```cpp
namespace fastecu::desktop::connection {
struct LocalAdapterDescriptor {
    std::string candidate_id;
    dashboard::AdapterKind kind;
    std::string vendor;
    std::string display_name;
    std::string label;
    bool operator==(const LocalAdapterDescriptor&) const = default;
};

struct AdapterDiscoverySnapshot {
    std::uint64_t generation;
    std::vector<LocalAdapterDescriptor> candidates;
    std::vector<Error> diagnostics;
};

class OpenedCanAdapter {
  public:
    virtual ~OpenedCanAdapter() = default;
    virtual std::unique_ptr<cdbg::ICanTransport> into_transport() && = 0;
};

class ILocalAdapterProvider {
  public:
    virtual ~ILocalAdapterProvider() = default;
    virtual dashboard::AdapterKind kind() const = 0;
    virtual Result<std::vector<LocalAdapterDescriptor>> discover() = 0;
    virtual Result<std::unique_ptr<OpenedCanAdapter>> open(
        std::string_view candidate_id,
        const dashboard::CdbgConnectionProfile& profile) = 0;
};
} // namespace fastecu::desktop::connection
```

Keep candidate IDs opaque to consumers. Provider implementations may decode only IDs they issued.

- [ ] **Step 4: Implement exact matching**

Declare and implement:

```cpp
enum class AdapterResolutionKind { UniqueMatch, SelectionRequired };
struct AdapterResolution {
    AdapterResolutionKind kind;
    std::string candidate_id;
};

std::string normalize_adapter_text(std::string_view text);
AdapterResolution resolve_preferred_adapter(
    const std::optional<dashboard::PreferredAdapter>& preferred,
    const std::vector<LocalAdapterDescriptor>& candidates);
```

Trim ASCII surrounding whitespace and lowercase ASCII characters. Require exact kind/vendor/display-name equality after normalization. Return `UniqueMatch` only for exactly one match; do not score, fuzzy-match, or select the first candidate.

- [ ] **Step 5: Run focused tests**

Run: `bazel test //src/platform/desktop/common/connection:test_local_adapter_matching --test_output=errors`

Expected: PASS.

- [ ] **Step 6: Commit the matching contract**

```bash
git add src/platform/desktop/common/connection
git commit -m "feat(connection): define local adapter matching"
```

---

### Task 2: Generalize raw-CAN setup without changing Widgets behavior

**Files:**
- Modify: `src/platform/desktop/common/logging/cdbg_serial_setup.h`
- Modify: `src/platform/desktop/common/logging/cdbg_serial_setup.cpp`
- Create: `src/platform/desktop/common/logging/cdbg_serial_setup_test.cpp`
- Modify: `src/platform/desktop/common/logging/legacy_logging_protocol_factory.cpp`
- Modify: `src/platform/desktop/common/logging/legacy_logging_protocol_factory_test.cpp`
- Modify: `src/platform/desktop/common/logging/BUILD.bazel`

**Interfaces:**
- Consumes: validated `dashboard::CdbgConnectionProfile` or explicit `RawCanSetupProfile`.
- Produces: `configure_raw_can(const RawCanSetupProfile&, const RawCanSetupActions&)` used by both legacy and dashboard paths.

- [ ] **Step 1: Write profile-driven setup tests**

Add tests recording calls and values:

```cpp
RawCanSetupProfile profile{
    .bitrate = 250000,
    .identifier_width = dashboard::CanIdentifierWidth::Extended,
    .reply_id = 0x18daf110,
};
EXPECT_TRUE(configure_raw_can(profile, recording.actions()));
EXPECT_EQ(recording.calls(), (std::vector<std::string>{
    "iso14230:false", "iso14230-header:false", "raw-can:true",
    "iso15765:false", "identifier-width:29", "bitrate:250000",
    "reply-id:0x18daf110",
}));
```

Add one test per failing action and require `InvalidConfig` detail to name that stable operation. Add a Colt-default test for 11-bit, 500000 baud, and `MitsuColtCanCdbg::kReplyCanId`.

- [ ] **Step 2: Run tests and verify the old fixed API fails them**

Run: `bazel test //src/platform/desktop/common/logging:test_cdbg_serial_setup --test_output=errors`

Expected: FAIL because the profile/value callbacks do not exist.

- [ ] **Step 3: Replace fixed actions with value-bearing actions**

Implement:

```cpp
struct RawCanSetupProfile {
    std::uint32_t bitrate;
    dashboard::CanIdentifierWidth identifier_width;
    std::uint32_t reply_id;
};

struct RawCanSetupActions {
    std::function<bool(bool)> set_iso14230;
    std::function<bool(bool)> set_iso14230_header;
    std::function<bool(bool)> set_raw_can;
    std::function<bool(bool)> set_iso15765;
    std::function<bool(dashboard::CanIdentifierWidth)> set_identifier_width;
    std::function<bool(std::uint32_t)> set_bitrate;
    std::function<bool(std::uint32_t)> set_reply_id;
};

Status configure_raw_can(const RawCanSetupProfile& profile,
                         const RawCanSetupActions& actions);
```

Call all seven operations in the existing order. Reject a missing callback or false return with the existing `InvalidConfig` category and a detail naming the failed operation.

- [ ] **Step 4: Adapt the legacy factory through Colt defaults**

Map values to `SerialPortActions` exactly:

```cpp
.set_identifier_width = [&serial](CanIdentifierWidth width) {
    return serial.set_is_29_bit_id(width == CanIdentifierWidth::Extended);
},
.set_bitrate = [&serial](std::uint32_t bitrate) {
    return serial.set_can_speed(QString::number(bitrate));
},
.set_reply_id = [&serial](std::uint32_t id) {
    return serial.set_can_destination_address(id);
},
```

Construct `RawCanSetupProfile{500000, Standard, MitsuColtCanCdbg::kReplyCanId}` in the legacy CDBG branch. Update its recording test to prove configuration still precedes open and protocol construction.

- [ ] **Step 5: Run setup and compatibility tests**

Run: `bazel test //src/platform/desktop/common/logging:test_cdbg_serial_setup //src/platform/desktop/common/logging:test_legacy_logging_protocol_factory --test_output=errors`

Expected: PASS with the existing legacy order and values.

- [ ] **Step 6: Commit generalized setup**

```bash
git add src/platform/desktop/common/logging
git commit -m "refactor(logging): configure raw CAN from a profile"
```

---

### Task 3: Add owned J2534 adapter discovery and transport lifetime

**Files:**
- Create: `src/platform/desktop/common/connection/j2534_adapter_provider.h`
- Create: `src/platform/desktop/common/connection/j2534_adapter_provider.cpp`
- Create: `src/platform/desktop/common/connection/j2534_adapter_provider_test.cpp`
- Modify: `src/platform/desktop/common/transport/fastecu_can_transport.h`
- Modify: `src/platform/desktop/common/transport/fastecu_can_transport.cpp`
- Create: `src/platform/desktop/common/transport/fastecu_can_transport_test.cpp`
- Modify: `src/platform/desktop/common/transport/BUILD.bazel`
- Modify: `src/platform/desktop/common/connection/BUILD.bazel`

**Interfaces:**
- Consumes: `ILocalAdapterProvider`, generalized raw-CAN setup, `SerialPortActions`, and `isJ2534CapableEntry()`.
- Produces: `J2534AdapterProvider`, an `OpenedCanAdapter`, and an owning `FastEcuCanTransport(std::unique_ptr<SerialPortActions>)` constructor.

- [ ] **Step 1: Add lifetime tests for `FastEcuCanTransport`**

Using the existing fake serial backend, construct a transport with an owned `SerialPortActions`, verify `isOpen()` delegates to it, and prove destruction destroys the backend exactly once. Retain a test for the existing borrowed-pointer constructor because the Widgets path still uses it.

- [ ] **Step 2: Run the transport test and confirm ownership is absent**

Run: `bazel test //src/platform/desktop/common/transport:test_fastecu_can_transport --test_output=errors`

Expected: FAIL because there is no owning constructor or focused test target.

- [ ] **Step 3: Add optional ownership without breaking legacy callers**

Implement these constructors and storage:

```cpp
explicit FastEcuCanTransport(SerialPortActions *serial);
explicit FastEcuCanTransport(std::unique_ptr<SerialPortActions> serial);

std::unique_ptr<SerialPortActions> owned_serial_;
SerialPortActions *serial_ = nullptr;
```

The owning constructor initializes `serial_` from `owned_serial_.get()`. Do not add close calls; `SerialPortActions` destruction already drains and destroys its direct backend.

- [ ] **Step 4: Write J2534 provider tests through injected actions**

Give `J2534AdapterProvider` a production constructor and a package-private testing constructor holding callbacks for list, construct, configure, open, and open-state. Test that:

- `ttyUSB0 - USB Serial` is excluded;
- `cu.usbmodem0 - OpenPort 2.0` and `Acme J2534 DLL` are included;
- labels and vendor/display fields are stable;
- candidate IDs resolve only entries issued by this provider;
- the document profile values reach generalized setup;
- an empty open result or false open-state returns `Disconnected`; and
- all failure paths destroy the created `SerialPortActions`.

- [ ] **Step 5: Run provider tests and verify RED**

Run: `bazel test //src/platform/desktop/common/connection:test_j2534_adapter_provider --test_output=errors`

Expected: FAIL because `J2534AdapterProvider` does not exist.

- [ ] **Step 6: Implement local J2534 discovery/opening**

Production discovery constructs a direct `SerialPortActions` with an empty peer address, calls `check_serial_ports()`, filters using `isJ2534CapableEntry()`, and creates deterministic candidate IDs from the provider prefix plus the exact entry. Do not expose the DLL path or raw entry to QML.

Production open constructs a fresh direct `SerialPortActions`, supplies the exact selected entry through `set_serial_port_list()`, applies `RawCanSetupProfile` from the document, calls `open_serial_port()`, verifies `is_serial_port_open()`, and returns an opened adapter whose `into_transport()` creates an owning `FastEcuCanTransport`.

- [ ] **Step 7: Run focused provider and transport tests**

Run: `bazel test //src/platform/desktop/common/transport:test_fastecu_can_transport //src/platform/desktop/common/connection:test_j2534_adapter_provider --test_output=errors`

Expected: PASS.

- [ ] **Step 8: Commit owned J2534 support**

```bash
git add src/platform/desktop/common/transport src/platform/desktop/common/connection
git commit -m "feat(connection): discover and own local J2534 adapters"
```

---

### Task 4: Add Linux SocketCAN provider and transport

**Files:**
- Create: `src/platform/desktop/common/connection/socketcan_transport.h`
- Create: `src/platform/desktop/common/connection/socketcan_transport.cpp`
- Create: `src/platform/desktop/common/connection/socketcan_transport_test.cpp`
- Create: `src/platform/desktop/common/connection/socketcan_adapter_provider.h`
- Create: `src/platform/desktop/common/connection/socketcan_adapter_provider.cpp`
- Create: `src/platform/desktop/common/connection/socketcan_adapter_provider_test.cpp`
- Modify: `src/platform/desktop/common/connection/BUILD.bazel`

**Interfaces:**
- Consumes: Linux `PF_CAN`, `CAN_RAW`, `if_nameindex`, `ioctl`, `poll`, and `ICanTransport`.
- Produces: `SocketCanTransport` and `SocketCanAdapterProvider`; non-Linux builds expose the same provider with an empty discovery result.

- [ ] **Step 1: Write transport tests around injected socket operations**

Define a package-private `SocketCanActions` seam for `send`, `poll`, `recv`, and `close`. Test:

- standard IDs encode without `CAN_EFF_FLAG`;
- extended IDs encode with `CAN_EFF_FLAG`;
- reads strip CAN flags and preserve payload;
- an unrelated ID remains readable for protocol-level filtering;
- poll timeout returns `std::nullopt`;
- cancellation returns `Cancelled` before blocking;
- `POLLHUP`, `ENODEV`, and zero-byte receive return `Disconnected`; and
- destruction closes the descriptor once.

- [ ] **Step 2: Run the absent transport test**

Run on Linux: `bazel test //src/platform/desktop/common/connection:test_socketcan_transport --test_output=errors`

Expected: FAIL because the transport does not exist. On macOS/Windows, verify the target is excluded by Bazel `select()`.

- [ ] **Step 3: Implement the owned nonblocking CAN raw transport**

Construct `SocketCanTransport(int fd, CanIdentifierWidth width, SocketCanActions actions)` and implement `ICanTransport`. Reject payloads above eight bytes with `InvalidConfig`, preserve explicit arbitration IDs, use `poll()` in bounded slices so cancellation is observed, map syscall failures to stable `Disconnected` or `Internal` errors, and close the descriptor in the destructor.

- [ ] **Step 4: Write provider enumeration/open tests**

Through injected interface/socket actions, prove discovery includes only interfaces whose hardware type is CAN, emits `AdapterKind::SocketCan`, and uses interface name as display name. Prove opening:

- rejects a stale/unknown candidate;
- creates `PF_CAN/SOCK_RAW/CAN_RAW`;
- resolves and binds the chosen interface index;
- applies a kernel receive filter for `profile.reply_id` with the correct standard/extended mask;
- rejects a down interface with `Disconnected`; and
- rejects bitrate mismatch with `Unsupported` rather than reconfiguring the host interface.

The provider reads the current bitrate through its injected platform query. This checkpoint must not invoke `ip link`, NetworkManager, or privileged host reconfiguration.

- [ ] **Step 5: Run provider tests and verify RED**

Run on Linux: `bazel test //src/platform/desktop/common/connection:test_socketcan_adapter_provider --test_output=errors`

Expected: FAIL because the provider does not exist.

- [ ] **Step 6: Implement Linux enumeration and opening**

Use `if_nameindex()` plus `SIOCGIFHWADDR`/`ARPHRD_CAN` to enumerate CAN interfaces. Query flags to require `IFF_UP`; query the effective bitrate through a small platform action so production can use rtnetlink while tests remain deterministic. Bind a nonblocking raw CAN socket, set the exact reply filter, and return an opened adapter whose `into_transport()` transfers the descriptor to `SocketCanTransport`.

Add a non-Linux implementation returning an empty candidate list and `Unsupported` from `open()`. Use Bazel platform `select()` so Linux headers are never compiled on macOS or Windows.

- [ ] **Step 7: Run SocketCAN tests and cross-platform compile targets**

Run on Linux:

```bash
bazel test //src/platform/desktop/common/connection:test_socketcan_transport \
  //src/platform/desktop/common/connection:test_socketcan_adapter_provider --test_output=errors
```

Run on the current platform: `bazel build //src/platform/desktop/common/connection:connection_platform`

Expected: tests PASS on Linux; the provider library builds with an empty provider on non-Linux.

- [ ] **Step 8: Commit SocketCAN support**

```bash
git add src/platform/desktop/common/connection
git commit -m "feat(connection): add local SocketCAN provider"
```

---

### Task 5: Build complete logging runs in `DesktopConnectionService`

**Files:**
- Create: `src/platform/desktop/common/connection/desktop_connection_service.h`
- Create: `src/platform/desktop/common/connection/desktop_connection_service.cpp`
- Create: `src/platform/desktop/common/connection/desktop_connection_service_test.cpp`
- Modify: `src/platform/desktop/common/connection/BUILD.bazel`

**Interfaces:**
- Consumes: `ILocalAdapterProvider`, matching functions, `prepare_dashboard_session()`, `CdbgLoggingProtocol`, and `LoggingRun`.
- Produces: `ConnectionPreparationOutcome` and `DesktopConnectionService::prepare_run()`/`refresh()`.

- [ ] **Step 1: Write outcome and resolution tests with fake providers**

Cover these concrete cases:

- invalid document fails before either provider's `discover()` or `open()`;
- no cards returns `InvalidConfig` before hardware access;
- no preference returns `SelectionRequired` with all candidates;
- one exact preferred match opens that provider;
- no match and duplicate exact matches require selection;
- one explicit current-generation candidate overrides the preference;
- stale generation and unknown ID rediscover and require selection;
- one provider's discovery error appears in diagnostics while the other provider's candidates remain selectable;
- open/configuration errors return `Failed` with no retained adapter; and
- success returns the matching descriptor and a non-null complete `LoggingRun`.

- [ ] **Step 2: Run the missing service test**

Run: `bazel test //src/platform/desktop/common/connection:test_desktop_connection_service --test_output=errors`

Expected: FAIL because the service and typed outcomes do not exist.

- [ ] **Step 3: Define typed preparation outcomes**

Use a move-only variant with these exact alternatives:

```cpp
struct PreparedConnection {
    logging::LoggingRun run;
    LocalAdapterDescriptor selected;
};
struct AdapterSelectionRequired {
    AdapterDiscoverySnapshot snapshot;
    enum class Reason { NoPreference, NoMatch, AmbiguousMatch, StaleSelection } reason;
};
using ConnectionPreparationOutcome =
    std::variant<PreparedConnection, AdapterSelectionRequired, Error>;

struct AdapterSelection {
    std::uint64_t generation;
    std::string candidate_id;
};
```

`Error` is the failed alternative; do not encode selection-required as an error.

- [ ] **Step 4: Implement discovery generations and selection validation**

`refresh()` increments a nonzero `std::uint64_t` generation with saturating behavior, calls every provider, merges successful candidates in kind/vendor/display-name order, and retains provider failures in diagnostics. Candidate IDs must be unique within the merged snapshot; duplicate IDs return `Internal`.

`prepare_run(document, selection)` first calls `prepare_dashboard_session(document)` and rejects an empty `session().channels()` before `refresh()`. It validates an explicit selection against the current snapshot and generation; stale selections force a refreshed selection outcome.

- [ ] **Step 5: Construct the complete owned run**

After resolution, call only the selected provider's `open()`. Move the opened adapter into an `ICanTransport`, split `PreparedDashboardSession::into_parts()`, and construct:

```cpp
auto [session, config] = std::move(prepared_session).into_parts();
auto protocol = std::make_unique<logging::CdbgLoggingProtocol>(
    std::move(transport), session.channels(), std::move(config));
return PreparedConnection{
    .run = logging::LoggingRun{std::move(session), std::move(protocol)},
    .selected = descriptor,
};
```

Catch standard and unknown exceptions at this platform boundary and return `Internal`; local ownership must unwind before return.

- [ ] **Step 6: Run service and dependency tests**

Run:

```bash
bazel test //src/platform/desktop/common/connection:test_desktop_connection_service \
  //src/backend/dashboard:test_dashboard_session_builder \
  //src/backend/logging/protocols:test_cdbg_logging_protocol --test_output=errors
```

Expected: PASS.

- [ ] **Step 7: Commit the connection service**

```bash
git add src/platform/desktop/common/connection
git commit -m "feat(connection): prepare owned dashboard logging runs"
```

---

### Task 6: Add the QtQuick connection controller state machine

**Files:**
- Create: `src/ui/desktop-quick/dashboard/dashboard_connection_controller.h`
- Create: `src/ui/desktop-quick/dashboard/dashboard_connection_controller.cpp`
- Create: `src/ui/desktop-quick/dashboard/dashboard_connection_controller_test.cpp`
- Modify: `src/ui/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: `DesktopConnectionService`, `LoggingEngine`, and a validated `DashboardDocument` supplied through `setDocument()`.
- Produces: QML-facing `DashboardConnectionController`, `ConnectionState`, and `AdapterCandidateModel`.

- [ ] **Step 1: Write controller tests against narrow seams**

Introduce these controller-facing seams so tests do not open hardware or start a real thread:

```cpp
class IConnectionPreparationService {
  public:
    virtual ~IConnectionPreparationService() = default;
    virtual ConnectionPreparationOutcome prepare_run(
        const dashboard::DashboardDocument& document,
        std::optional<AdapterSelection> selection) = 0;
    virtual AdapterDiscoverySnapshot refresh() = 0;
};

class ILoggingEngine {
  public:
    virtual ~ILoggingEngine() = default;
    virtual Status start(logging::LoggingRun run) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
};
```

The production adapters forward to `DesktopConnectionService` and `LoggingEngine`; engine signals are connected through the production adapter's Qt signal surface. Test with `QSignalSpy` that:

- the initial state is `Disconnected`, with Connect disabled until a usable document is set;
- `setDocument()` never calls prepare/start;
- explicit connect transitions `Disconnected -> Connecting`;
- selection-required populates candidate roles and state;
- `connectWithAdapter()` forwards current generation and candidate ID;
- refresh replaces candidates and invalidates old IDs;
- prepared connection calls engine start but remains `Connecting` until `Running` status;
- start failure enters `Failed` and destroys the run;
- `CarNotResponding` recovers to `Running` on engine status;
- handshake/runtime/disconnect end reasons map to stable summaries;
- disconnect transitions through `Disconnecting` and calls joined `stop()` once;
- repeated disconnect is harmless; and
- destruction stops an active engine without emitting late state changes.

- [ ] **Step 2: Run the absent controller test**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_connection_controller --test_output=errors`

Expected: FAIL because the controller target does not exist.

- [ ] **Step 3: Define stable QML properties and actions**

Declare:

```cpp
enum class ConnectionState {
    Disconnected, Connecting, AdapterSelectionRequired,
    Running, CarNotResponding, Disconnecting, Failed
};

Q_PROPERTY(ConnectionState state READ state NOTIFY stateChanged)
Q_PROPERTY(QString statusText READ statusText NOTIFY presentationChanged)
Q_PROPERTY(QString technicalDetail READ technicalDetail NOTIFY presentationChanged)
Q_PROPERTY(QString selectedAdapterLabel READ selectedAdapterLabel NOTIFY presentationChanged)
Q_PROPERTY(QAbstractItemModel* candidates READ candidates CONSTANT)
Q_PROPERTY(qulonglong discoveryGeneration READ discoveryGeneration NOTIFY presentationChanged)
Q_PROPERTY(bool canConnect READ canConnect NOTIFY stateChanged)
Q_PROPERTY(bool canDisconnect READ canDisconnect NOTIFY stateChanged)
Q_PROPERTY(bool needsAdapterSelection READ needsAdapterSelection NOTIFY stateChanged)

Q_INVOKABLE void connectDashboard();
Q_INVOKABLE void connectWithAdapter(const QString& candidateId);
Q_INVOKABLE void refreshAdapters();
Q_INVOKABLE void disconnectDashboard();
void setDocument(std::optional<dashboard::DashboardDocument> document);
```

`AdapterCandidateModel` exposes `candidateId`, `label`, and `kindLabel` roles only. Expose the discovery generation as controller metadata for diagnostics and binding tests, but require `connectWithAdapter()` to pair the selected ID with the controller's current generation; QML never supplies a generation value.

- [ ] **Step 4: Implement the state machine and error mapping**

Map service selection outcomes to the selection state, and errors to stable concise text plus original detail. Map engine events as follows:

```text
LoggingStatus::Running          -> Running
LoggingStatus::CarNotResponding -> CarNotResponding
StoppedByUser                   -> Disconnected
HandshakeFailed                 -> Failed / "Unable to start CDBG logging"
AdapterDisconnected             -> Failed / "Adapter disconnected"
RuntimeFailed                   -> Failed / "Logging stopped unexpectedly"
```

Keep `canConnect` true only for `Disconnected` or `Failed` with a usable document. Keep `canDisconnect` true for `Connecting`, `Running`, or `CarNotResponding`. Disconnect uses synchronous `LoggingEngine::stop()` and relies on its exactly-once completion.

- [ ] **Step 5: Run controller tests**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_connection_controller --test_output=errors`

Expected: PASS.

- [ ] **Step 6: Commit the controller**

```bash
git add src/ui/desktop-quick/dashboard src/ui/desktop-quick/BUILD.bazel
git commit -m "feat(desktop-quick): add dashboard connection controller"
```

---

### Task 7: Compose the controller and add the QtQuick connection surface

**Files:**
- Create: `src/ui/desktop-quick/qml/dashboard/ConnectionPanel.qml`
- Modify: `src/ui/desktop-quick/qml/shell/ApplicationShell.qml`
- Modify: `src/ui/desktop-quick/qml.qrc`
- Modify: `src/ui/desktop-quick/desktop_quick_application.h`
- Modify: `src/ui/desktop-quick/desktop_quick_application.cpp`
- Modify: `src/ui/desktop-quick/desktop_quick_application_test.cpp`
- Modify: `src/ui/desktop-quick/BUILD.bazel`
- Modify: `apps/desktop-quick/main.cpp`
- Modify: `apps/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: production connection providers/service/controller from Tasks 3–6.
- Produces: QML context property `dashboardConnection` and the first functional connection panel.

- [ ] **Step 1: Extend the offscreen application test first**

Change the test to inject a real controller constructed with fake service and engine dependencies before loading QML, then assert objects and bindings:

```cpp
QObject *panel = root->findChild<QObject *>("connectionPanel");
QVERIFY(panel != nullptr);
QCOMPARE(root->findChild<QObject *>("connectButton")->property("enabled").toBool(), false);
QVERIFY(root->findChild<QObject *>("connectionStatus") != nullptr);
QVERIFY(root->findChild<QObject *>("adapterPicker") != nullptr);
QVERIFY(root->findChild<QObject *>("refreshAdaptersButton") != nullptr);
QVERIFY(root->findChild<QObject *>("connectionErrorDetail") != nullptr);
```

Add fake-state cases for `Disconnected`, `AdapterSelectionRequired`, `Running`, and `Failed`, and invoke the QML buttons to prove they call the controller actions.

- [ ] **Step 2: Run the smoke test and verify missing controls**

Run: `bazel test //src/ui/desktop-quick:test_application --test_output=errors`

Expected: FAIL because the context property and connection panel do not exist.

- [ ] **Step 3: Change `load_root()` to require the controller**

Use the exact signature:

```cpp
bool load_root(QQmlApplicationEngine& engine,
               DashboardConnectionController& dashboard_connection);
```

Before `engine.load()`, set `dashboardConnection` on the root context. Update all callers and tests; do not register `SerialPortActions`, service, provider, transport, protocol, or logging engine objects with QML.

- [ ] **Step 4: Implement `ConnectionPanel.qml`**

Bind the panel only to controller properties/actions. Include:

- one Connect/Disconnect button with stable object name `connectButton`;
- `connectionStatus` label;
- `selectedAdapterLabel` label;
- `adapterPicker` visible only when selection is required;
- `refreshAdaptersButton`;
- candidate confirmation calling `connectWithAdapter(candidateId)`; and
- collapsed `connectionErrorDetail` that can be expanded without hiding the concise message.

Disable actions while `Connecting` or `Disconnecting`. Do not add editable bitrate, identifier, protocol, or document fields.

- [ ] **Step 5: Add the panel to resources and shell**

Add the QML file to both `qml.qrc` and the Bazel `qml_resources.files`. Import `../dashboard` from `ApplicationShell.qml` and place `ConnectionPanel` at the bottom of the workspace without replacing the existing navigation rail.

- [ ] **Step 6: Compose production objects in `main.cpp`**

Construct providers, `DesktopConnectionService`, `LoggingEngine` adapter, and `DashboardConnectionController` before `QQmlApplicationEngine`; pass the controller to `load_root()`. Do not set a document and do not call Connect. Until the document-workflow checkpoint supplies a validated document, the production panel correctly starts disabled while offscreen tests inject a document.

- [ ] **Step 7: Run QML and controller tests**

Run:

```bash
bazel test //src/ui/desktop-quick:test_application \
  //src/ui/desktop-quick:test_dashboard_connection_controller --test_output=errors
```

Expected: PASS offscreen with the Basic controls style.

- [ ] **Step 8: Build both desktop applications**

Run: `bazel build //:fastecu //:fastecu-desktop-quick`

Expected: PASS; the QtQuick binary links the connection package without adding it to the Widgets binary's QML surface.

- [ ] **Step 9: Commit composition and QML**

```bash
git add apps/desktop-quick src/ui/desktop-quick
git commit -m "feat(desktop-quick): expose local adapter connection controls"
```

---

### Task 8: Run regression, portability, and cleanup gates

**Files:**
- Modify only if required by verified gate failures: affected `BUILD.bazel` files or test registration lists.

**Interfaces:**
- Consumes: all preceding task outputs.
- Produces: a green, reviewable checkpoint with no unstated dependency or formatting failures.

- [ ] **Step 1: Run all focused new and compatibility tests**

Run:

```bash
bazel test \
  //src/platform/desktop/common/connection:test_local_adapter_matching \
  //src/platform/desktop/common/connection:test_j2534_adapter_provider \
  //src/platform/desktop/common/connection:test_desktop_connection_service \
  //src/platform/desktop/common/logging:test_cdbg_serial_setup \
  //src/platform/desktop/common/logging:test_legacy_logging_protocol_factory \
  //src/platform/desktop/common/logging:test_logging_engine \
  //src/backend/dashboard:test_dashboard_session_builder \
  //src/backend/logging/protocols:test_cdbg_logging_protocol \
  //src/ui/desktop-quick:test_dashboard_connection_controller \
  //src/ui/desktop-quick:test_application \
  --test_output=errors
```

On Linux, add these two platform-specific targets to the same run:

```bash
bazel test \
  //src/platform/desktop/common/connection:test_socketcan_transport \
  //src/platform/desktop/common/connection:test_socketcan_adapter_provider \
  --test_output=errors
```

Expected: PASS. The SocketCAN test targets are absent from non-Linux configurations by Bazel platform selection.

- [ ] **Step 2: Run portable and desktop build gates**

Run:

```bash
bazel test //:portable_closure --test_output=errors
bazel build //:fastecu //:fastecu-desktop-quick
```

Expected: PASS. The connection package remains outside the portable closure, while its backend dependencies remain portable.

- [ ] **Step 3: Run repository formatting and diff checks**

Run:

```bash
prek run --all-files
git diff --check
```

Expected: PASS with no whitespace or formatting failures.

- [ ] **Step 4: Inspect dependency and scope boundaries**

Run:

```bash
rg -n "SerialPortActions|ICanTransport|CdbgLoggingProtocol|set_is_can_connection" src/ui/desktop-quick/qml src/ui/desktop-quick/dashboard
rg -n "RemoteSerialBackend|WebSocket|QRemoteObject" src/platform/desktop/common/connection
git status --short
```

Expected: the first search finds no QML exposure and no controller dependency on concrete transport/setup types; the second finds no production remote dependency; status shows only intended task changes plus the user's pre-existing unrelated changes.

- [ ] **Step 5: Commit any gate-only corrections**

If Step 1–4 required tracked corrections, commit only those files:

```bash
git add src/platform/desktop/common/connection \
  src/platform/desktop/common/logging \
  src/platform/desktop/common/transport \
  src/ui/desktop-quick \
  apps/desktop-quick
git commit -m "test(connection): complete desktop quick regression gates"
```

If no corrections were required, do not create an empty commit.

## Completion checkpoint

Before claiming implementation complete, invoke `superpowers:verification-before-completion` and rerun the exact final commands it requires. Record any platform limitation explicitly: SocketCAN behavioral tests run only on Linux, while macOS and Windows must compile the empty-provider implementation and exercise J2534 discovery through their supported local backend.
