# Desktop Quick Dashboard Visualizations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render numeric, sparkline, and horizontal-gauge cards from one validated dashboard while keeping bounded sparkline history and session rules in the C++ presentation model.

**Architecture:** Extend `DashboardCardModel` with immutable visualization configuration and model-owned timestamped history, fed by the controller's existing 33 ms coalesced sample stream. Dispatch QML delegates by display type, with shared card chrome and Canvas-based sparkline/gauge content that cannot affect acquisition or persistence.

**Tech Stack:** C++23, Qt 6.8.3 Core/Test/Quick/QML/Quick Controls, QAbstractListModel, QML Canvas, Bazel, GoogleTest/QtTest.

**Spec:** `docs/superpowers/specs/2026-08-30-desktop-quick-dashboard-visualizations-design.md`

## Global Constraints

- Keep the existing 33 ms single-shot controller timer as the only presentation update clock.
- Record at most one coalesced value per card per flush; do not retain every backend callback.
- Sparkline history duration stays within the existing validated range of 1–300 seconds and is never persisted.
- Use card gauge overrides before conversion defaults for both gauge and sparkline scales.
- Break sparkline segments when a gap is greater than `max(3 * sampling_interval_ms, 100 ms)`.
- Clear history on every transition into `Connecting`; preserve it through silence, failure, disconnecting, and disconnected states.
- Keep a common responsive-grid footprint and a prominent value/unit/state/stale-age presentation for all card types.
- Add no Qt Charts dependency, schema field, format-version change, animation timer, hover interaction, or editor/persistence behavior.
- Do not change the Widgets application behavior.

---

## File Structure

- Modify `src/ui/desktop-quick/dashboard/dashboard_card_model.h`: define display configuration roles, timestamped point transport, history operations, and per-row visualization state.
- Modify `src/ui/desktop-quick/dashboard/dashboard_card_model.cpp`: resolve effective bounds, expose roles, append/prune/replace history, derive segment boundaries, and clear histories.
- Modify `src/ui/desktop-quick/dashboard/dashboard_card_model_test.cpp`: cover configuration projection, history bounds, segmentation, notification precision, and independent rows.
- Modify `src/ui/desktop-quick/dashboard/dashboard_controller.cpp`: clear history when a new connection attempt enters `Connecting`.
- Modify `src/ui/desktop-quick/dashboard/dashboard_controller_test.cpp`: cover clear-on-connect and preserve-on-stale/disconnect lifecycle behavior.
- Create `src/ui/desktop-quick/qml/dashboard/DashboardCardFrame.qml`: own common card chrome, prominent reading, stale age, content slot, and accessibility name.
- Modify `src/ui/desktop-quick/qml/dashboard/NumericCard.qml`: compose the shared frame without changing its public inputs or visible behavior.
- Create `src/ui/desktop-quick/qml/dashboard/SparklineCard.qml`: render bounded elapsed-time segments and clipped markers from model-prepared points.
- Create `src/ui/desktop-quick/qml/dashboard/HorizontalGaugeCard.qml`: render normalized/clamped fill, capped ticks, endpoint labels, and overflow direction.
- Modify `src/ui/desktop-quick/qml/dashboard/DashboardView.qml`: declare visualization roles and choose a delegate component by display type.
- Modify `src/ui/desktop-quick/qml.qrc` and `src/ui/desktop-quick/BUILD.bazel`: package the three new QML components.
- Modify `src/ui/desktop-quick/desktop_quick_application_test.cpp`: test common chrome, delegate selection, geometry inputs, accessibility, and responsive behavior offscreen.
- Modify `src/ui/desktop-quick/dashboard/bundled_dashboard_loader.cpp` and its test: accept every display type already validated by the portable document service.
- Modify `src/ui/desktop-quick/resources/colt-dashboard.ohd`: exercise numeric, sparkline, and horizontal-gauge cards in the shipped fixture.

### Task 1: Project Visualization Configuration Through the Card Model

**Files:**
- Modify: `src/ui/desktop-quick/dashboard/dashboard_card_model.h`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_card_model.cpp`
- Test: `src/ui/desktop-quick/dashboard/dashboard_card_model_test.cpp`

**Interfaces:**
- Consumes: `dashboard::DashboardCard::display_type`, `gauge_bounds`, and `sparkline_history_seconds`; `dashboard::DashboardConversion::{gauge_min,gauge_max,gauge_step}`.
- Produces: QML roles `displayType`, `minimumValue`, `maximumValue`, `stepValue`, and `sparklineHistorySeconds`; QML-visible enum values `Numeric`, `Sparkline`, and `HorizontalGauge` registered through the existing uncreatable `DashboardCardModel` type.

- [ ] **Step 1: Write failing configuration-role tests**

Extend the fixture to contain one card of each type. Give the sparkline conversion bounds `-40, 260, 10`; give the gauge card an override `0, 9000, 250`. Add these assertions:

```cpp
QCOMPARE(role(model, 0, DashboardCardModel::DisplayTypeRole).value<CardDisplayType>(),
         CardDisplayType::Numeric);
QCOMPARE(role(model, 1, DashboardCardModel::DisplayTypeRole).value<CardDisplayType>(),
         CardDisplayType::Sparkline);
QCOMPARE(role(model, 1, DashboardCardModel::MinimumValueRole), -40.0);
QCOMPARE(role(model, 1, DashboardCardModel::MaximumValueRole), 260.0);
QCOMPARE(role(model, 1, DashboardCardModel::StepValueRole), 10.0);
QCOMPARE(role(model, 1, DashboardCardModel::SparklineHistorySecondsRole), 30);
QCOMPARE(role(model, 2, DashboardCardModel::MinimumValueRole), 0.0);
QCOMPARE(role(model, 2, DashboardCardModel::MaximumValueRole), 9000.0);
QCOMPARE(role(model, 2, DashboardCardModel::StepValueRole), 250.0);
QCOMPARE(model.roleNames().value(DashboardCardModel::DisplayTypeRole), QByteArrayLiteral("displayType"));
```

Also assert that numeric/gauge rows expose `0` for `sparklineHistorySeconds`, rather than an invalid QVariant, so required QML delegate properties remain type-stable.

- [ ] **Step 2: Run the model test and verify it fails**

Run:

```bash
bazel test //src/ui/desktop-quick:test_dashboard_card_model --test_output=errors
```

Expected: compilation fails because the new roles and `CardDisplayType` presentation enum do not exist.

- [ ] **Step 3: Add display configuration types and roles**

In `dashboard_card_model.h`, add a QML-safe enum rather than exposing the backend enum directly:

```cpp
enum class CardDisplayType
{
    Numeric,
    Sparkline,
    HorizontalGauge,
};
Q_ENUM(CardDisplayType)
```

Append these roles after the existing roles to avoid renumbering established roles:

```cpp
DisplayTypeRole,
MinimumValueRole,
MaximumValueRole,
StepValueRole,
SparklineHistorySecondsRole,
```

Extend `Row` with:

```cpp
CardDisplayType display_type = CardDisplayType::Numeric;
double minimum_value = 0.0;
double maximum_value = 1.0;
double step_value = 1.0;
int sparkline_history_seconds = 0;
```

Add a private exhaustive conversion helper from `dashboard::CardDisplayType`. During row construction, resolve bounds with:

```cpp
const auto bounds = card.gauge_bounds.value_or(dashboard::GaugeBoundsOverride{
    conversion.gauge_min, conversion.gauge_max, conversion.gauge_step});
```

Populate the roles and role names exactly as asserted. Return the QML-safe enum with `QVariant::fromValue`.

- [ ] **Step 4: Run the model test and verify it passes**

Run:

```bash
bazel test //src/ui/desktop-quick:test_dashboard_card_model --test_output=errors
```

Expected: PASS, including all pre-existing ordering and live/stale tests.

- [ ] **Step 5: Commit the configuration projection**

```bash
git add src/ui/desktop-quick/dashboard/dashboard_card_model.h \
        src/ui/desktop-quick/dashboard/dashboard_card_model.cpp \
        src/ui/desktop-quick/dashboard/dashboard_card_model_test.cpp
git commit -m "feat(desktop-quick): expose dashboard visualization config"
```

### Task 2: Add Bounded Sparkline History and Session Lifecycle

**Files:**
- Modify: `src/ui/desktop-quick/dashboard/dashboard_card_model.h`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_card_model.cpp`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_controller.cpp`
- Test: `src/ui/desktop-quick/dashboard/dashboard_card_model_test.cpp`
- Test: `src/ui/desktop-quick/dashboard/dashboard_controller_test.cpp`

**Interfaces:**
- Consumes: `ReceivedLogSample{logging::LogSample sample, std::uint64_t received_at_ms}`, the document connection's `sampling_interval_ms`, and `ConnectionState` transitions.
- Produces: `SparklinePointsRole` named `sparklinePoints`, `Q_INVOKABLE`-free `void clearSparklineHistories()`, and point maps with keys `elapsedMs`, `value`, and `startsSegment`.

- [ ] **Step 1: Write failing history append and pruning tests**

Add a sparkline fixture with a 2-second history. Apply samples at 1000, 1500, and 3101 ms. Assert that the first point is pruned, the remaining points are relative to the newest point, and a numeric row has no points:

```cpp
const QVariantList points = role(model, sparkline_row, DashboardCardModel::SparklinePointsRole).toList();
QCOMPARE(points.size(), 2);
QCOMPARE(points.at(0).toMap().value("elapsedMs").toLongLong(), -1601);
QCOMPARE(points.at(0).toMap().value("value").toDouble(), 20.0);
QCOMPARE(points.at(1).toMap().value("elapsedMs").toLongLong(), 0);
QCOMPARE(role(model, numeric_row, DashboardCardModel::SparklinePointsRole).toList().size(), 0);
```

Add a same-timestamp test proving the latest point is replaced, not appended. Inspect the `dataChanged` roles and require `SparklinePointsRole` only for sparkline updates.

- [ ] **Step 2: Write failing segmentation and clear tests**

Construct the model with `sampling_interval_ms = 50`. Add points at 1000, 1100, and 1251 ms. Assert `startsSegment` is true for the first and third points and false for the second because the threshold is 150 ms and only gaps greater than it break.

Then call `clearSparklineHistories()` and assert the sparkline list is empty, current formatted values remain unchanged, and `dataChanged` contains only `SparklinePointsRole` for rows that previously had history.

- [ ] **Step 3: Run the model tests and verify they fail**

Run:

```bash
bazel test //src/ui/desktop-quick:test_dashboard_card_model --test_output=errors
```

Expected: compilation fails because `SparklinePointsRole` and `clearSparklineHistories()` do not exist.

- [ ] **Step 4: Implement timestamped history in the model**

Add a private point type and storage:

```cpp
struct SparklinePoint
{
    std::uint64_t timestamp_ms;
    double value;
    bool starts_segment;
};

QVector<SparklinePoint> sparkline_points;
std::uint64_t gap_threshold_ms = 100;
```

Derive each row's threshold at construction:

```cpp
row.gap_threshold_ms = std::max<std::uint64_t>(
    100, static_cast<std::uint64_t>(document.connection.sampling_interval_ms) * 3);
```

When applying a valid sparkline sample:

- replace the final point when timestamps are equal;
- ignore an older timestamp defensively so history remains ordered while still updating the current reading according to existing behavior;
- otherwise append with `starts_segment` true for the first point or a gap strictly greater than the threshold;
- prune points where `timestamp_ms + duration_ms < newest_timestamp_ms`;
- force the new first retained point's `starts_segment` to true.

Serialize `SparklinePointsRole` as a `QVariantList` of `QVariantMap` values relative to the newest point:

```cpp
{{"elapsedMs", static_cast<qlonglong>(point.timestamp_ms) - static_cast<qlonglong>(newest)},
 {"value", point.value},
 {"startsSegment", point.starts_segment}}
```

Add `clearSparklineHistories()` and precise notifications. Do not clear current readings or their timestamps.

- [ ] **Step 5: Run the model tests and verify they pass**

Run:

```bash
bazel test //src/ui/desktop-quick:test_dashboard_card_model --test_output=errors
```

Expected: PASS with bounded history, threshold boundary, replacement, pruning, and notification assertions.

- [ ] **Step 6: Write failing controller lifecycle tests**

In `dashboard_controller_test.cpp`, seed sparkline history through `queueSamples` plus `flushPendingSamples`. Drive the fake connection through the same signal-producing actions used by existing state tests. Assert:

```cpp
// Entering Connecting clears history but retains current value.
QCOMPARE(sparkline_points(dashboard).size(), 0);
QCOMPARE(card_value(dashboard, sparkline_row), QStringLiteral("2000"));
```

In a separate test, seed history and transition through `CarNotResponding`, `Failed`, and `Disconnected`; assert the point count remains unchanged and the reading becomes stale.

- [ ] **Step 7: Run the controller test and verify it fails**

Run:

```bash
bazel test //src/ui/desktop-quick:test_dashboard_controller --test_output=errors
```

Expected: FAIL because entering `Connecting` does not clear histories.

- [ ] **Step 8: Clear history only on `Connecting`**

Update `DashboardController::handleConnectionStateChanged()`:

```cpp
case ConnectionState::Connecting:
    cards_->clearSparklineHistories();
    break;
```

Leave `CarNotResponding`, `Disconnecting`, `Disconnected`, and `Failed` on the existing flush-and-stale path. Do not clear history in those cases or in `Running`.

- [ ] **Step 9: Run focused presentation tests**

Run:

```bash
bazel test //src/ui/desktop-quick:test_dashboard_card_model \
           //src/ui/desktop-quick:test_dashboard_controller \
           --test_output=errors
```

Expected: PASS.

- [ ] **Step 10: Commit bounded history and lifecycle behavior**

```bash
git add src/ui/desktop-quick/dashboard/dashboard_card_model.h \
        src/ui/desktop-quick/dashboard/dashboard_card_model.cpp \
        src/ui/desktop-quick/dashboard/dashboard_card_model_test.cpp \
        src/ui/desktop-quick/dashboard/dashboard_controller.cpp \
        src/ui/desktop-quick/dashboard/dashboard_controller_test.cpp
git commit -m "feat(desktop-quick): retain bounded sparkline history"
```

### Task 3: Extract Shared Card Chrome Without Numeric Regressions

**Files:**
- Create: `src/ui/desktop-quick/qml/dashboard/DashboardCardFrame.qml`
- Modify: `src/ui/desktop-quick/qml/dashboard/NumericCard.qml`
- Modify: `src/ui/desktop-quick/qml.qrc`
- Modify: `src/ui/desktop-quick/BUILD.bazel`
- Test: `src/ui/desktop-quick/desktop_quick_application_test.cpp`

**Interfaces:**
- Consumes: existing `NumericCard` properties `cardTitleText`, `cardValueText`, `cardUnitText`, `cardReadingState`, and `cardLastUpdateAgeText`.
- Produces: `DashboardCardFrame` with the same five required properties plus `default property alias visualizationContent`; readonly state constants/text/colors remain available to composed cards.

- [ ] **Step 1: Strengthen the failing common-frame regression test**

Extend `dashboardRendersCardsInModelOrderAndRetainsReadingsAcrossStates()` to require every numeric card to expose a readonly `usesDashboardCardFrame` property set to `true`, while preserving every existing title/value/unit/state/age and accessibility assertion.

- [ ] **Step 2: Run the application test and verify it fails**

Run:

```bash
bazel test //src/ui/desktop-quick:test_application --test_output=errors
```

Expected: FAIL because `usesDashboardCardFrame` does not exist.

- [ ] **Step 3: Create the shared frame**

Move the `Frame`, background, state constants, `stateText`, `valueColor`, accessible name, and common labels from `NumericCard.qml` into `DashboardCardFrame.qml`. Add a lower content slot after the unit and before stale age:

```qml
default property alias visualizationContent: visualizationHost.data

Item {
    id: visualizationHost
    objectName: "cardVisualizationHost"
    visible: children.length > 0
    Layout.fillWidth: true
    implicitHeight: visible ? 72 : 0
}
```

Keep `objectName: "dashboardCardFrame"` on the standalone frame and preserve `cardTitle`, `cardValue`, `cardUnit`, `cardState`, and `cardAge` names.

Rewrite `NumericCard.qml` as a root `DashboardCardFrame` with `objectName: "numericCard"`, `readonly property bool usesDashboardCardFrame: true`, and property forwarding. The composed instance intentionally overrides the reusable type's standalone object name. It supplies no visualization child, so its height and appearance remain unchanged from the pre-extraction card.

- [ ] **Step 4: Package the new QML file**

Add `qml/dashboard/DashboardCardFrame.qml` to both the `qml_resources.files` list in `BUILD.bazel` and the `/omnihaste` resource entries in `qml.qrc`.

- [ ] **Step 5: Run the application test and verify it passes**

Run:

```bash
bazel test //src/ui/desktop-quick:test_application --test_output=errors
```

Expected: PASS with unchanged numeric values, stale styling inputs, responsive order, and accessible names.

- [ ] **Step 6: Commit the shared card frame**

```bash
git add src/ui/desktop-quick/qml/dashboard/DashboardCardFrame.qml \
        src/ui/desktop-quick/qml/dashboard/NumericCard.qml \
        src/ui/desktop-quick/qml.qrc src/ui/desktop-quick/BUILD.bazel \
        src/ui/desktop-quick/desktop_quick_application_test.cpp
git commit -m "refactor(desktop-quick): share dashboard card frame"
```

### Task 4: Render Sparkline and Horizontal-Gauge Delegates

**Files:**
- Create: `src/ui/desktop-quick/qml/dashboard/SparklineCard.qml`
- Create: `src/ui/desktop-quick/qml/dashboard/HorizontalGaugeCard.qml`
- Modify: `src/ui/desktop-quick/qml/dashboard/DashboardView.qml`
- Modify: `src/ui/desktop-quick/qml.qrc`
- Modify: `src/ui/desktop-quick/BUILD.bazel`
- Test: `src/ui/desktop-quick/desktop_quick_application_test.cpp`

**Interfaces:**
- Consumes: Task 1 roles and enum values plus Task 2 `sparklinePoints`; Task 3 `DashboardCardFrame`.
- Produces: QML objects `sparklineCard`, `sparklineCanvas`, `horizontalGaugeCard`, and `gaugeCanvas`; inspectable derived properties `normalizedValue`, `overflowDirection`, `tickCount`, and `segmentCount` for deterministic offscreen tests.

- [ ] **Step 1: Write failing mixed-delegate offscreen tests**

Change the application test fixture to contain numeric, sparkline, and gauge cards. Require one visual child for each of `numericCard`, `sparklineCard`, and `horizontalGaugeCard`, in document order. Verify all three contain common title/value/unit/state objects and have equal implicit heights.

For the gauge, apply in-range, over-range, and under-range readings and assert:

```cpp
QTRY_COMPARE(gauge->property("normalizedValue").toDouble(), 0.5);
QTRY_COMPARE(gauge->property("overflowDirection").toInt(), 0);
// Above maximum:
QTRY_COMPARE(gauge->property("normalizedValue").toDouble(), 1.0);
QTRY_COMPARE(gauge->property("overflowDirection").toInt(), 1);
// Below minimum:
QTRY_COMPARE(gauge->property("normalizedValue").toDouble(), 0.0);
QTRY_COMPARE(gauge->property("overflowDirection").toInt(), -1);
```

For the sparkline, feed three points containing one gap and require `segmentCount == 2`. Mark the row stale and assert its prominent value and accessible name remain available.

- [ ] **Step 2: Run the application test and verify it fails**

Run:

```bash
bazel test //src/ui/desktop-quick:test_application --test_output=errors
```

Expected: FAIL because all rows still instantiate `NumericCard` and the new QML types do not exist.

- [ ] **Step 3: Implement `SparklineCard.qml`**

Compose `DashboardCardFrame` and declare required inputs matching the common card plus:

```qml
required property real minimumValue
required property real maximumValue
required property real stepValue
required property int historySeconds
required property var points
readonly property int segmentCount: {
    var count = 0
    for (var i = 0; i < points.length; ++i)
        if (points[i].startsSegment)
            ++count
    return count
}
```

Use a Canvas with `objectName: "sparklineCanvas"`. In `onPaint`, return early for invalid size/range or fewer than two points. Map x from `[-historySeconds * 1000, 0]` and y from `[minimumValue, maximumValue]`, clamp y to the canvas, start a new path for `startsSegment`, draw capped horizontal reference lines, and draw small edge triangles for clipped values. Use a hard maximum of 12 reference lines; if `ceil((maximum-minimum)/step) > 12`, increase the rendered stride rather than looping over every step.

Call `requestPaint()` from `onPointsChanged`, bound/step/history changes, width/height changes, and reading-state changes. Dim the Canvas with `opacity: cardReadingState === staleReadingState ? 0.55 : 1.0`. Do not create a Timer.

- [ ] **Step 4: Implement `HorizontalGaugeCard.qml`**

Compose `DashboardCardFrame` and declare the common properties plus minimum, maximum, and step. Expose:

```qml
required property bool hasReading
required property real numericValue
readonly property real rawFraction: hasReading
    ? (numericValue - minimumValue) / (maximumValue - minimumValue) : 0
readonly property real normalizedValue: Math.max(0, Math.min(1, rawFraction))
readonly property int overflowDirection: !hasReading ? 0
    : numericValue < minimumValue ? -1
    : numericValue > maximumValue ? 1 : 0
readonly property int tickCount: Math.min(12,
    Math.max(1, Math.ceil((maximumValue - minimumValue) / stepValue)))
```

Use `objectName: "horizontalGaugeCard"` and a Canvas named `gaugeCanvas`. Draw a neutral track in waiting state, otherwise a fill ending at `normalizedValue`, capped tick marks, endpoint labels, and an edge triangle for `overflowDirection`. Dim retained stale geometry consistently with the sparkline. Invalidate only on relevant property or size changes.

- [ ] **Step 5: Dispatch delegates by model display type**

In `DashboardView.qml`, import `OmniHaste.Dashboard 1.0`, declare all required model properties on the delegate, and replace the fixed `NumericCard` with a `Loader`:

```qml
sourceComponent: cardDelegate.displayType === DashboardCardModel.Sparkline
    ? sparklineComponent
    : cardDelegate.displayType === DashboardCardModel.HorizontalGauge
      ? gaugeComponent : numericComponent
```

Define three local `Component` blocks that forward the common roles and only the visualization-specific roles each component requires. Bind the delegate's `implicitWidth` and `implicitHeight` to `loader.item` with a stable fallback of `240 x 170` during creation.

- [ ] **Step 6: Package both visualization components**

Add `SparklineCard.qml` and `HorizontalGaugeCard.qml` to `qml_resources.files` and `qml.qrc`, next to the existing dashboard components.

- [ ] **Step 7: Run the offscreen application tests**

Run:

```bash
bazel test //src/ui/desktop-quick:test_application --test_output=errors
```

Expected: PASS for typed delegate selection, common chrome, gauge normalization/overflow, sparkline segments, stale accessibility, and responsive grid behavior.

- [ ] **Step 8: Commit the visualization delegates**

```bash
git add src/ui/desktop-quick/qml/dashboard/SparklineCard.qml \
        src/ui/desktop-quick/qml/dashboard/HorizontalGaugeCard.qml \
        src/ui/desktop-quick/qml/dashboard/DashboardView.qml \
        src/ui/desktop-quick/qml.qrc src/ui/desktop-quick/BUILD.bazel \
        src/ui/desktop-quick/desktop_quick_application_test.cpp
git commit -m "feat(desktop-quick): render dashboard visualizations"
```

### Task 5: Enable and Exercise Mixed-Type Bundled Dashboards

**Files:**
- Modify: `src/ui/desktop-quick/dashboard/bundled_dashboard_loader.cpp`
- Modify: `src/ui/desktop-quick/dashboard/bundled_dashboard_loader_test.cpp`
- Modify: `src/ui/desktop-quick/resources/colt-dashboard.ohd`
- Modify: `src/ui/desktop-quick/desktop_quick_application_test.cpp`

**Interfaces:**
- Consumes: portable codec/validation guarantees and all presentation/QML behavior from Tasks 1–4.
- Produces: a shipped Colt fixture containing at least one numeric, sparkline, and horizontal-gauge card; `load_bundled_colt_dashboard()` accepts all portable-v1-valid display types.

- [ ] **Step 1: Replace the numeric-only loader test with a failing mixed-type test**

Change the non-numeric fixture in `bundled_dashboard_loader_test.cpp` into a valid document with numeric, sparkline, and horizontal-gauge cards. Assert successful loading and exact display types/order:

```cpp
ASSERT_TRUE(document.has_value()) << document.error().detail;
ASSERT_EQ(document->cards.size(), 3U);
EXPECT_EQ(document->cards[0].display_type, dashboard::CardDisplayType::Numeric);
EXPECT_EQ(document->cards[1].display_type, dashboard::CardDisplayType::Sparkline);
EXPECT_EQ(document->cards[2].display_type, dashboard::CardDisplayType::HorizontalGauge);
```

Keep a malformed or portable-invalid fixture assertion to prove the loader still returns the document service's actionable error.

- [ ] **Step 2: Run the loader test and verify it fails**

Run:

```bash
bazel test //src/ui/desktop-quick:test_bundled_dashboard_loader --test_output=errors
```

Expected: FAIL with the current `only numeric cards are supported` error.

- [ ] **Step 3: Remove the presentation-layer numeric restriction**

Delete the display-type loop from `load_bundled_colt_dashboard()`. Retain `DashboardDocumentService::load()` and `prepare_dashboard_session()` checks so malformed documents and unusable sessions still fail before application composition.

- [ ] **Step 4: Update the shipped Colt dashboard**

Keep `engine-rpm` numeric. Change `ecu-load` to:

```xml
<card id="ecu-load" channel-id="CDBG_ECU_LOAD" conversion-id="default" display-type="horizontal-gauge" title="ECU Load" order="1" gauge-min="0" gauge-max="100" gauge-step="10"/>
```

Change `knock-sum` to:

```xml
<card id="knock-sum" channel-id="CDBG_KNOCK_SUM" conversion-id="default" display-type="sparkline" title="Knock Sum" order="2" sparkline-history-seconds="30"/>
```

Leave the fourth card numeric. Update loader/application expectations that previously assumed every bundled card was numeric.

- [ ] **Step 5: Run the focused mixed-dashboard tests**

Run:

```bash
bazel test //src/ui/desktop-quick:test_bundled_dashboard_loader \
           //src/ui/desktop-quick:test_application \
           --test_output=errors
```

Expected: PASS and the offscreen application contains all three delegate types from the shipped resource.

- [ ] **Step 6: Run formatting and the complete desktop-quick suite**

Run:

```bash
clang-format --dry-run --Werror \
  src/ui/desktop-quick/dashboard/dashboard_card_model.h \
  src/ui/desktop-quick/dashboard/dashboard_card_model.cpp \
  src/ui/desktop-quick/dashboard/dashboard_card_model_test.cpp \
  src/ui/desktop-quick/dashboard/dashboard_controller.cpp \
  src/ui/desktop-quick/dashboard/dashboard_controller_test.cpp \
  src/ui/desktop-quick/dashboard/bundled_dashboard_loader.cpp \
  src/ui/desktop-quick/dashboard/bundled_dashboard_loader_test.cpp \
  src/ui/desktop-quick/desktop_quick_application_test.cpp
bazel test //src/ui/desktop-quick:all --test_output=errors
bazel build //:fastecu //:fastecu-desktop-quick
git diff --check
```

Expected: formatting check passes; every desktop-quick test passes; both desktop targets build; `git diff --check` prints no errors.

- [ ] **Step 7: Commit mixed-type integration**

```bash
git add src/ui/desktop-quick/dashboard/bundled_dashboard_loader.cpp \
        src/ui/desktop-quick/dashboard/bundled_dashboard_loader_test.cpp \
        src/ui/desktop-quick/resources/colt-dashboard.ohd \
        src/ui/desktop-quick/desktop_quick_application_test.cpp
git commit -m "feat(desktop-quick): ship mixed dashboard cards"
```

- [ ] **Step 8: Record final verification evidence**

Run:

```bash
git status --short
git log -5 --oneline
```

Expected: only changes that predated execution remain unstaged; the five task commits appear in order. Record the exact test/build commands and outcomes in the implementation handoff.
