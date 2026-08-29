# Desktop Quick Functional Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load a bundled Colt `.ohd` and render its live CDBG values as responsive numeric cards with bounded updates and retained stale readings.

**Architecture:** Production startup decodes a checked-in dashboard resource and gives equal immutable copies to the connection controller and a new `DashboardController`. The latter owns `DashboardCardModel`, reduces logging batches to newest values at 30 Hz, and projects connection state as waiting/live/stale roles consumed by passive QML.

**Tech Stack:** C++23, Qt 6.8.3, QML/Quick Controls, `QAbstractListModel`, `QTimer`, Bazel and `rules_qt` 0.0.6, GoogleTest/QtTest, XML `.ohd` codec.

**Spec:** `docs/superpowers/specs/2026-08-29-desktop-quick-functional-dashboard-design.md`

## Global Constraints

- Keep connection actions and lifecycle exclusively in `DashboardConnectionController`.
- QML receives no protocols, transports, adapters, backend services, or writable ECU APIs.
- Flush value-model changes no more than once per 33 ms without throttling acquisition.
- Retain readings through silence, disconnect, and failure; never replace them with zero.
- Use real Colt data and numeric cards only; reject unsupported fixture content.
- Do not add document workflows, editing, gauges, sparklines, or persistence.
- Preserve Widgets behavior and unrelated working-tree changes.

---

### Task 1: Bundle and validate the Colt dashboard

**Files:**
- Create: `src/ui/desktop-quick/resources/colt-dashboard.ohd`
- Create: `src/ui/desktop-quick/dashboard/bundled_dashboard_loader.h`
- Create: `src/ui/desktop-quick/dashboard/bundled_dashboard_loader.cpp`
- Create: `src/ui/desktop-quick/dashboard/bundled_dashboard_loader_test.cpp`
- Modify: `src/ui/desktop-quick/qml.qrc`
- Modify: `src/ui/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: `DashboardDocumentService::load()` and `prepare_dashboard_session()`.
- Produces: `load_bundled_colt_dashboard(DashboardDocumentService&)` and `:/omnihaste/dashboards/colt-dashboard.ohd`.

- [ ] **Step 1: Write failing loader tests**

```cpp
TEST(BundledDashboardLoaderTest, LoadsRealResourceAndBuildsSession)
{
    QtFileRepository repository;
    InMemoryAtomicFileWriter writer;
    dashboard::DashboardDocumentService service(repository, writer);
    auto document = load_bundled_colt_dashboard(service);
    ASSERT_TRUE(document) << document.error().detail;
    EXPECT_EQ(document->metadata.name, "Colt CDBG Dashboard");
    EXPECT_TRUE(std::ranges::all_of(document->cards, [](const auto& card) {
        return card.display_type == dashboard::CardDisplayType::Numeric;
    }));
    EXPECT_TRUE(dashboard::prepare_dashboard_session(*document));
}
```

Add cases for missing resource and an in-memory document containing a
Sparkline card; expect the latter to return `ErrorKind::Unsupported`.

- [ ] **Step 2: Register and run the failing target**

Run: `bazel test //src/ui/desktop-quick:test_bundled_dashboard_loader --test_output=errors`

Expected: FAIL because the resource and loader do not exist.

- [ ] **Step 3: Add the resource and QRC entry**

Create format-version-1 XML with the four channels from
`resources/shared/config/logger_cdbg_example.xml`: Engine RPM, ECU Load, Knock
Sum, and Coil Dwell Optimal RPM x Power. Preserve their addresses,
conversions, units, precision, and gauge defaults. Add numeric cards in that
order. Use 500000 bit/s, standard IDs `0x630`/`0x631`, stream 0, 50 ms sampling,
100 ms timeout, silence threshold 3, three retries, and 250 ms retry period.

Add `<file alias="dashboards/colt-dashboard.ohd">resources/colt-dashboard.ohd</file>`
to the QRC and the source path to `qml_resources.files`.

- [ ] **Step 4: Implement the loader**

```cpp
inline constexpr std::string_view kBundledColtDashboardHandle =
    ":/omnihaste/dashboards/colt-dashboard.ohd";
Result<dashboard::DashboardDocument>
load_bundled_colt_dashboard(dashboard::DashboardDocumentService& service);
```

Load through the service, reject each non-numeric card with its card ID, call
`prepare_dashboard_session()` as the final integrity check, and propagate exact
errors.

- [ ] **Step 5: Verify and commit**

Run: `bazel test //src/ui/desktop-quick:test_bundled_dashboard_loader --test_output=errors`

Expected: PASS.

Stage only the six Task 1 paths and commit with
`feat(desktop-quick): bundle Colt dashboard document`.

---

### Task 2: Relay sample batches through the logging presentation interface

**Files:**
- Modify: `src/ui/desktop-quick/dashboard/dashboard_connection_controller.h`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_connection_controller.cpp`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_connection_controller_test.cpp`

**Interfaces:**
- Consumes: `LoggingEngine::valuesUpdated(QVector<LogSample>)`.
- Produces: identical `ILoggingEngine::valuesUpdated`; connection behavior is unchanged.

- [ ] **Step 1: Write a failing relay test**

Extract a package-private `make_logging_engine_bridge(LoggingEngine&)` factory,
observe its sample signal with `QSignalSpy`, emit one concrete-engine sample,
and assert the channel ID and numeric value arrive unchanged.

```cpp
auto bridge = make_logging_engine_bridge(engine);
QSignalSpy spy(bridge.get(), &ILoggingEngine::valuesUpdated);
emit engine.valuesUpdated({sample("CDBG_ENGINE_RPM", 3125.0)});
QCOMPARE(spy.count(), 1);
```

- [ ] **Step 2: Run to verify failure**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_connection_controller --test_output=errors`

Expected: FAIL because `ILoggingEngine` lacks the signal.

- [ ] **Step 3: Add the signal and concrete relay**

```cpp
signals:
    void valuesUpdated(QVector<fastecu::logging::LogSample> samples);
```

Connect concrete signal to bridge signal. Do not forward any hardware object.

- [ ] **Step 4: Verify and commit**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_connection_controller //src/platform/desktop/common/logging:test_logging_engine --test_output=errors`

Expected: PASS. Commit the three Task 2 files with
`feat(desktop-quick): relay dashboard logging samples`.

---

### Task 3: Implement the immutable card model

**Files:**
- Create: `src/ui/desktop-quick/dashboard/dashboard_card_model.h`
- Create: `src/ui/desktop-quick/dashboard/dashboard_card_model.cpp`
- Create: `src/ui/desktop-quick/dashboard/dashboard_card_model_test.cpp`
- Modify: `src/ui/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: validated `DashboardDocument`, `LogSample`, monotonic milliseconds.
- Produces: `DashboardCardModel`, `ReadingState`, sample/state/age mutation methods.

- [ ] **Step 1: Write failing projection tests**

```cpp
DashboardCardModel model(two_card_document());
QCOMPARE(model.rowCount(), 2);
QCOMPARE(role(model, 0, DashboardCardModel::TitleRole), "Engine RPM");
QCOMPARE(role(model, 0, DashboardCardModel::FormattedValueRole), QStringLiteral("—"));
QCOMPARE(state(model, 0), ReadingState::Waiting);
model.applySamples({sample("unknown", 1.0)}, 100, true);
QCOMPARE(model.rowCount(), 2);
```

Also assert document order, title fallback/override, unit, precision, role
names, and absence of reset/insertion notifications.

- [ ] **Step 2: Write failing lifecycle tests**

```cpp
model.applySamples({sample("CDBG_ENGINE_RPM", 3125.4)}, 1000, true);
QCOMPARE(role(model, 0, DashboardCardModel::FormattedValueRole), "3125");
QCOMPARE(state(model, 0), ReadingState::Live);
model.markReceivedRowsStale();
QCOMPARE(state(model, 0), ReadingState::Stale);
model.updateAges(13000);
QCOMPARE(role(model, 0, DashboardCardModel::LastUpdateAgeTextRole),
         "Last update 12s ago");
```

Add partial-batch, resume, non-finite rejection, and compact `dataChanged`
coverage.

- [ ] **Step 3: Register and run the failing target**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_card_model --test_output=errors`

Expected: FAIL because the model does not exist.

- [ ] **Step 4: Implement the model**

```cpp
enum class ReadingState { Waiting, Live, Stale };
class DashboardCardModel final : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Role { CardIdRole = Qt::UserRole + 1, ChannelIdRole, TitleRole,
        FormattedValueRole, NumericValueRole, UnitRole, PrecisionRole,
        ReadingStateRole, HasReadingRole, LastUpdateAgeTextRole };
    explicit DashboardCardModel(const dashboard::DashboardDocument&, QObject * = nullptr);
    void applySamples(const QVector<logging::LogSample>&, std::uint64_t, bool running);
    void markReceivedRowsStale();
    void updateAges(std::uint64_t now_ms);
    bool hasReceivedRows() const;
    bool containsChannel(std::string_view channel_id) const;
};
```

Resolve metadata once, sort by card order, index channel ID to row, format via
`QString::number(value, 'f', precision)`, reject `!std::isfinite(value)`, and
never insert or reset after construction. Register `ReadingState` with
`Q_ENUM` so QML compares named states rather than integer literals.

- [ ] **Step 5: Verify and commit**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_card_model --test_output=errors`

Expected: PASS. Commit the four Task 3 paths with
`feat(desktop-quick): add numeric dashboard card model`.

---

### Task 4: Add the presentation controller and coalescing

**Files:**
- Create: `src/ui/desktop-quick/dashboard/dashboard_controller.h`
- Create: `src/ui/desktop-quick/dashboard/dashboard_controller.cpp`
- Create: `src/ui/desktop-quick/dashboard/dashboard_controller_test.cpp`
- Modify: `src/ui/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: sample signal, connection state, `IClock::now_ms()`.
- Produces: card/title/load-error properties, 33 ms value flush, 1000 ms ages.

- [ ] **Step 1: Write failing coalescing tests**

```cpp
DashboardController dashboard(document, engine, connection, clock);
QSignalSpy changed(dashboard.cards(), &QAbstractItemModel::dataChanged);
engine.publishValues({sample("CDBG_ENGINE_RPM", 1000.0)});
engine.publishValues({sample("CDBG_ENGINE_RPM", 2000.0)});
QTRY_VERIFY_WITH_TIMEOUT(changed.count() > 0, 100);
QCOMPARE(cardValue(dashboard, 0), QStringLiteral("2000"));
QCOMPARE(valueNotificationCount(changed), 1);
```

- [ ] **Step 2: Write failing state and teardown tests**

Cover waiting, fresh-sample live transition, immediate silence staling,
pending flush before terminal stale, stale-through-reconnect, FakeClock age
updates, unknown/non-finite diagnostics, and destruction with armed timers.

- [ ] **Step 3: Register and run the failing target**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_controller --test_output=errors`

Expected: FAIL because the controller does not exist.

- [ ] **Step 4: Implement the controller**

```cpp
class DashboardController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *cards READ cards CONSTANT)
    Q_PROPERTY(QString dashboardTitle READ dashboardTitle CONSTANT)
    Q_PROPERTY(bool hasLoadError READ hasLoadError CONSTANT)
    Q_PROPERTY(QString loadErrorText READ loadErrorText CONSTANT)
  public:
    DashboardController(dashboard::DashboardDocument, ILoggingEngine&,
                        DashboardConnectionController&, IClock&, QObject * = nullptr);
    static std::unique_ptr<DashboardController>
    fromLoadError(QString, ILoggingEngine&, DashboardConnectionController&,
                  IClock&, QObject * = nullptr);
};
```

Use a channel-keyed pending map, a single-shot 33 ms timer, and a repeating
1000 ms age timer. New samples replace pending values for their channel.
Flush before staling on CarNotResponding, Disconnecting, Disconnected, or
Failed. Connecting and adapter selection retain state. Running alone never
revives an old reading. Use `containsChannel()` and `std::isfinite()` before
queuing; emit `diagnostic(QString)` for rejected samples. Start the age timer
after the first received value and stop it whenever no received row requires
age updates.

- [ ] **Step 5: Verify and commit**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_card_model //src/ui/desktop-quick:test_dashboard_controller --test_output=errors`

Expected: PASS without long sleeps. Commit the four Task 4 paths with
`feat(desktop-quick): coalesce live dashboard values`.

---

### Task 5: Build responsive numeric-card QML

**Files:**
- Create: `src/ui/desktop-quick/qml/dashboard/NumericCard.qml`
- Create: `src/ui/desktop-quick/qml/dashboard/DashboardView.qml`
- Modify: `src/ui/desktop-quick/qml/shell/ApplicationShell.qml`
- Modify: `src/ui/desktop-quick/qml.qrc`
- Modify: `src/ui/desktop-quick/BUILD.bazel`
- Modify: `src/ui/desktop-quick/desktop_quick_application_test.cpp`

**Interfaces:**
- Consumes: `dashboardPresentation` and model roles.
- Produces: `dashboardView`, `dashboardGrid`, `numericCard`, `cardTitle`, `cardValue`, `cardUnit`, `cardState` object names.

- [ ] **Step 1: Write failing offscreen QML tests**

Pass a two-card controller fixture to `load_root()`. Assert title, two cards,
em-dash waiting values, connection panel retention, live/stale text and value
retention. Resize across the 240 px threshold; assert columns change and model
order does not.

- [ ] **Step 2: Run to verify failure**

Run: `bazel test //src/ui/desktop-quick:test_application --test_output=errors`

Expected: FAIL because dashboard QML is absent.

- [ ] **Step 3: Implement `NumericCard.qml`**

Create a passive Frame showing title, right-aligned value, unit, textual state,
and stale age. Use subdued waiting, normal live, and amber stale styling. Set
`Accessible.name` to title, value, unit, and state. Invoke no controller method.

- [ ] **Step 4: Implement view and shell layout**

Use ScrollView, GridLayout, and Repeater. Calculate:

```qml
readonly property int minimumCardWidth: 240
columns: Math.max(1, Math.floor((width + columnSpacing) /
                                (minimumCardWidth + columnSpacing)))
```

Make cards equal width, preserve model order, keep the connection panel at the
bottom, and show `loadErrorText` instead of the grid on load failure.

- [ ] **Step 5: Register resources, verify, and commit**

Run: `bazel test //src/ui/desktop-quick:test_application --test_output=errors`

Expected: PASS offscreen. Commit the six Task 5 paths with
`feat(desktop-quick): render responsive numeric dashboard`.

---

### Task 6: Compose production document loading and failure state

**Files:**
- Modify: `src/ui/desktop-quick/desktop_quick_application.h`
- Modify: `src/ui/desktop-quick/desktop_quick_application.cpp`
- Modify: `src/ui/desktop-quick/desktop_quick_application_test.cpp`
- Modify: `src/ui/desktop-quick/BUILD.bazel`
- Modify: `apps/desktop-quick/main.cpp`
- Modify: `apps/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: loader, both controllers, Qt file/atomic ports, QtClock.
- Produces: `load_root(engine, connection, presentation)` and visible errors.

- [ ] **Step 1: Write failing composition tests**

Assert both context properties exist. Add a failure fixture asserting no card
rows, visible `dashboardLoadError`, error detail, and disabled Connect.

- [ ] **Step 2: Run to verify failure**

Run: `bazel test //src/ui/desktop-quick:test_application --test_output=errors`

Expected: FAIL until `load_root()` accepts the presentation controller.

- [ ] **Step 3: Change the application API**

```cpp
bool load_root(QQmlApplicationEngine&, DashboardConnectionController&,
               DashboardController&);
```

Set `dashboardConnection` and `dashboardPresentation` before loading QML.

- [ ] **Step 4: Compose production dependencies**

In `main.cpp`, construct QtFileRepository, QtAtomicFileWriter,
DashboardDocumentService, and QtClock. On success, give equal document copies
to both controllers. On failure, set no connection document and create
`DashboardController::fromLoadError()`. Always load the shell.

- [ ] **Step 5: Verify and commit**

Run: `bazel test //src/ui/desktop-quick:test_application //src/ui/desktop-quick:test_bundled_dashboard_loader //src/ui/desktop-quick:test_dashboard_controller --test_output=errors`

Run: `bazel build //:fastecu-desktop-quick`

Expected: PASS. Commit the six Task 6 paths with
`feat(desktop-quick): compose functional Colt dashboard`.

---

### Task 7: Run regression, portability, and scope gates

**Files:**
- Modify only Task 1–6 files when a verified gate failure requires correction.

**Interfaces:**
- Consumes: all preceding outputs.
- Produces: a green functional-dashboard checkpoint.

- [ ] **Step 1: Run focused tests**

Run: `bazel test //src/ui/desktop-quick:test_bundled_dashboard_loader //src/ui/desktop-quick:test_dashboard_card_model //src/ui/desktop-quick:test_dashboard_controller //src/ui/desktop-quick:test_dashboard_connection_controller //src/ui/desktop-quick:test_application //src/platform/desktop/common/logging:test_logging_engine //src/platform/desktop/common/connection:test_desktop_connection_service //src/backend/dashboard:dashboard_codec_test //src/backend/dashboard:dashboard_session_builder_test --test_output=errors`

Expected: PASS.

- [ ] **Step 2: Run portable and desktop gates**

Run: `bazel test //:portable_closure --test_output=errors`

Run: `bazel build //:fastecu //:fastecu-desktop-quick`

Expected: PASS; presentation remains outside the portable closure.

- [ ] **Step 3: Run formatting checks**

Run: `prek run --all-files`

Run: `git diff --check`

Expected: PASS.

- [ ] **Step 4: Inspect architecture and scope**

Run: `rg -n "SerialPortActions|ICanTransport|CdbgLoggingProtocol|set_is_can_connection" src/ui/desktop-quick/qml src/ui/desktop-quick/dashboard`

Run: `rg -n "import_legacy|Sparkline|HorizontalGauge" src/ui/desktop-quick/dashboard/dashboard_controller.* src/ui/desktop-quick/qml/dashboard`

Run: `git status --short`

Expected: no hardware exposure; no later visualization implementation beyond
type rejection; only intended and preserved pre-existing changes.

- [ ] **Step 5: Run the smoke binary**

Run: `bazel run //:fastecu-desktop-quick -- --smoke-test`

Expected: exit 0 after the document and QML root load.

- [ ] **Step 6: Commit gate-only corrections when present**

Stage only exact corrected files and commit with
`test(desktop-quick): complete dashboard regression gates`. Do not create an
empty commit.

## Completion checkpoint

Invoke `superpowers:verification-before-completion` before claiming success and
rerun its required final commands. Record platform limitations explicitly;
real CDBG/CAN bench qualification remains in the later hardening checkpoint.
