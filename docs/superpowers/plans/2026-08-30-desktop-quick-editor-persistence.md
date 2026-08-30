# Desktop Quick Editor and Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an import-first dashboard editor with transactional document lifecycle, immediate preview, atomic saving, unsaved-change handling, and recent-document restoration.

**Architecture:** A new `DashboardDocumentController` owns the authoritative document, path, dirty state, and transition continuations. A focused `DashboardEditorModel` performs typed candidate-document mutations; committed documents refresh the existing presentation and connection controllers through application-level wiring, while QML remains a declarative side panel and dialog host.

**Tech Stack:** C++23, Qt 6 Core/Quick/QML (`QObject`, `QAbstractListModel`, Qt Quick Controls/Dialogs), portable dashboard services and ports, Bazel 9.1.1, QtTest/GTest

**Spec:** `docs/superpowers/specs/2026-08-30-desktop-quick-editor-persistence-design.md`

## Global Constraints

- Import is the only version-1 New Dashboard workflow; blank-document creation is out of scope.
- QML does not own or directly mutate `DashboardDocument` and contains no domain validation, dirty-state, or transition policy.
- Only imported channels and their embedded conversions may be selected for cards; missing channels never create placeholders.
- Every candidate edit validates and prepares before commit; rejection preserves document, path, dirty state, selection, and preview.
- Open, Import, and Exit share Save, Discard, and Cancel handling.
- Save and Save As update path, dirty state, and recent-document settings only after atomic persistence succeeds.
- Startup restoration failure is recoverable and leaves the Import/Open empty state.
- Document replacement, mutation, and Save actions are disabled while connected; active logging is never reconfigured in place.
- Card selection is stable by card ID across reordering; Move Up and Move Down are the only reorder controls.
- OS `.ohd` associations, drag-and-drop, channel/conversion editing, diagnostic export, and hardware qualification remain out of scope.
- No Qt Quick component can reach transport, acquisition, ECU write, flash, or actuator behavior.
- Every task leaves both `//:fastecu` and `//:fastecu-desktop-quick` buildable.
- Version-1 import uses the established CDBG profile: 500000 bit/s, 11-bit identifiers, stream instance 0, 50 ms sampling, and retry values 100 ms / 3 silences / 3 reconnects / 250 ms; the document name is the selected catalog filename stem.

---

## File map

- Create `src/ui/desktop-quick/dashboard/dashboard_document_controller.{h,cpp}`: authoritative document lifecycle, persistence, recent path, and unsaved continuations.
- Create `src/ui/desktop-quick/dashboard/dashboard_document_controller_test.cpp`: transactional lifecycle and continuation coverage with in-memory ports.
- Create `src/ui/desktop-quick/dashboard/dashboard_editor_model.{h,cpp}`: card list, selected-card properties, available choices, and transactional mutations.
- Create `src/ui/desktop-quick/dashboard/dashboard_editor_model_test.cpp`: add/remove/reorder/configuration/rollback coverage.
- Create `src/ui/desktop-quick/qml/dashboard/DashboardEditorPanel.qml`: persistent document and selected-card editor.
- Create `src/ui/desktop-quick/qml/dashboard/DocumentDialogs.qml`: file dialogs, unsaved decision dialog, and actionable error notification.
- Modify `src/ui/desktop-quick/dashboard/dashboard_controller.{h,cpp}`: accept empty state and replace the preview document after commits.
- Modify `src/ui/desktop-quick/dashboard/dashboard_controller_test.cpp`: replace/empty-state preview behavior.
- Modify `src/ui/desktop-quick/desktop_quick_application.{h,cpp}`: expose document/editor controllers and wire committed documents to presentation/connection.
- Modify `src/ui/desktop-quick/desktop_quick_application_test.cpp`: offscreen document/editor/dialog flows.
- Modify `src/ui/desktop-quick/qml/shell/ApplicationShell.qml`: compose the dashboard and persistent editor panel.
- Modify `src/ui/desktop-quick/qml.qrc` and `src/ui/desktop-quick/BUILD.bazel`: package and build new units/tests.
- Modify `apps/desktop-quick/main.cpp` and `apps/desktop-quick/BUILD.bazel`: construct ports/controllers and perform startup restoration.

### Task 1: Make Dashboard Presentation Replaceable

**Files:**
- Modify: `src/ui/desktop-quick/dashboard/dashboard_controller.h`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_controller.cpp`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_controller_test.cpp`
- Modify: `src/ui/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: `dashboard::DashboardDocument`, `DashboardCardModel`, and the existing coalesced logging behavior.
- Produces: `DashboardController(std::optional<dashboard::DashboardDocument>, ...)`, `void setDocument(std::optional<dashboard::DashboardDocument>)`, and notifying title/load-error/card state suitable for document replacement.

- [ ] **Step 1: Write failing empty/replacement tests**

Add tests that construct with `std::nullopt`, require zero cards and title `"No dashboard open"`, then call `setDocument(first)` and `setDocument(second)` and require the title/card rows to match only the latest document. Preserve the existing sample-coalescing assertions after replacement, and assert samples for IDs absent from the latest document do not appear.

```cpp
DashboardController controller(std::nullopt, logging, connection, clock);
QCOMPARE(controller.cards()->rowCount(), 0);
QCOMPARE(controller.dashboardTitle(), QStringLiteral("No dashboard open"));

controller.setDocument(first_document());
QCOMPARE(controller.cards()->rowCount(), 2);
QCOMPARE(controller.dashboardTitle(), QStringLiteral("First"));

controller.setDocument(second_document());
QCOMPARE(controller.cards()->rowCount(), 1);
QCOMPARE(controller.dashboardTitle(), QStringLiteral("Second"));
```

- [ ] **Step 2: Run the focused test and confirm the fixed-document API fails**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_controller --test_output=errors`

Expected: FAIL to compile because the constructor requires a document and `setDocument` does not exist.

- [ ] **Step 3: Implement replaceable presentation state**

Change the title/card properties from `CONSTANT` to `NOTIFY documentChanged`, add:

```cpp
Q_PROPERTY(QString dashboardTitle READ dashboardTitle NOTIFY documentChanged)
Q_PROPERTY(bool hasLoadError READ hasLoadError NOTIFY documentChanged)
Q_PROPERTY(QString loadErrorText READ loadErrorText NOTIFY documentChanged)

void setDocument(std::optional<dashboard::DashboardDocument> document);

signals:
    void documentChanged();
```

On replacement, clear pending samples and history, replace the card model's document rows, set the title or empty-state title, clear load errors, reconcile timers, and emit `documentChanged`. Do not alter logging-engine ownership or connection transitions.

- [ ] **Step 4: Run controller tests and application compilation**

Run:

```bash
bazel test //src/ui/desktop-quick:test_dashboard_controller --test_output=errors
bazel build //:fastecu //:fastecu-desktop-quick
```

Expected: PASS.

- [ ] **Step 5: Commit replaceable preview support**

```bash
git add src/ui/desktop-quick/dashboard/dashboard_controller.h \
        src/ui/desktop-quick/dashboard/dashboard_controller.cpp \
        src/ui/desktop-quick/dashboard/dashboard_controller_test.cpp \
        src/ui/desktop-quick/BUILD.bazel
git commit -m "refactor(desktop-quick): replace dashboard preview document"
```

### Task 2: Add Transactional Document Persistence and Restoration

**Files:**
- Create: `src/ui/desktop-quick/dashboard/dashboard_document_controller.h`
- Create: `src/ui/desktop-quick/dashboard/dashboard_document_controller.cpp`
- Create: `src/ui/desktop-quick/dashboard/dashboard_document_controller_test.cpp`
- Modify: `src/ui/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: `dashboard::DashboardDocumentService`, `dashboard::prepare_dashboard_session`, `ISettings`, and `DashboardConnectionController::ConnectionState`.
- Produces: authoritative optional document/path/dirty state; `importDocument`, `openDocument`, `save`, `saveAs`, `restoreRecentDocument`, and a transactional `commitCandidate` seam used by Task 4.

- [ ] **Step 1: Write failing lifecycle tests**

Define a QtTest fixture using `InMemoryFileRepository`, `InMemoryAtomicFileWriter`, `InMemorySettings`, and a real `DashboardDocumentService`. Cover these exact state transitions:

```cpp
QVERIFY(!controller.hasDocument());
QVERIFY(controller.importDocument("logger.xml", import_defaults()).has_value());
QVERIFY(controller.hasDocument());
QVERIFY(controller.isDirty());
QCOMPARE(controller.currentPath(), QString{});

QVERIFY(controller.saveAs("dashboard.ohd").has_value());
QVERIFY(!controller.isDirty());
QCOMPARE(controller.currentPath(), QStringLiteral("dashboard.ohd"));
QCOMPARE(settings.get("desktop-quick/recent-dashboard"), std::optional<std::string>{"dashboard.ohd"});
```

Also assert that failed import/open/prepare/save and cancelled path selection leave the complete prior state unchanged; failed Save As does not adopt its path; a semantic no-op candidate does not become dirty; and non-disconnected state returns `InvalidConfig` with detail `"disconnect before editing the dashboard"`.

- [ ] **Step 2: Run the new test target and verify it fails**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_document_controller --test_output=errors`

Expected: FAIL because the controller target does not exist.

- [ ] **Step 3: Implement the controller state and operations**

Declare the public contract:

```cpp
class DashboardDocumentController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY stateChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY stateChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY stateChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY stateChanged)
    Q_PROPERTY(bool editingEnabled READ editingEnabled NOTIFY stateChanged)

  public:
    static constexpr std::string_view recentPathKey = "desktop-quick/recent-dashboard";

    DashboardDocumentController(dashboard::DashboardDocumentService& documents, ISettings& settings,
                                QObject *parent = nullptr);
    const std::optional<dashboard::DashboardDocument>& document() const;
    Status importDocument(std::string_view handle, const dashboard::LegacyCdbgImportDefaults& defaults);
    Status openDocument(std::string_view handle);
    Status save();
    Status saveAs(std::string_view handle);
    Status restoreRecentDocument();
    Status commitCandidate(dashboard::DashboardDocument candidate, std::string selected_card_id);
    void setConnectionState(ConnectionState state);

  signals:
    void documentCommitted();
    void stateChanged();
    void errorOccurred(QString operation, QString detail);
```

Use a private snapshot structure for tests and rollback reasoning. Validate and call `prepare_dashboard_session(candidate)` before `commitCandidate` changes authoritative state. Emit `documentCommitted` only after a successful replacement/mutation. Persist the recent path only after successful Open, Save, or Save As. `restoreRecentDocument()` returns success without a document when the setting is absent, and returns the original structured error without clearing the stored key when restoration fails.

- [ ] **Step 4: Run lifecycle tests**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_document_controller --test_output=errors`

Expected: PASS.

- [ ] **Step 5: Run portable and desktop build gates**

Run:

```bash
bazel test //src/backend/dashboard:all //src/ui/desktop-quick:test_dashboard_document_controller
bazel build //:fastecu //:fastecu-desktop-quick
```

Expected: PASS.

- [ ] **Step 6: Commit transactional persistence**

```bash
git add src/ui/desktop-quick/dashboard/dashboard_document_controller.h \
        src/ui/desktop-quick/dashboard/dashboard_document_controller.cpp \
        src/ui/desktop-quick/dashboard/dashboard_document_controller_test.cpp \
        src/ui/desktop-quick/BUILD.bazel
git commit -m "feat(desktop-quick): own dashboard document lifecycle"
```

### Task 3: Centralize Unsaved-Change Continuations

**Files:**
- Modify: `src/ui/desktop-quick/dashboard/dashboard_document_controller.h`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_document_controller.cpp`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_document_controller_test.cpp`

**Interfaces:**
- Consumes: Task 2 document operations and state.
- Produces: `PendingDocumentAction`, `UnsavedDecision`, request/complete APIs, path-request signals, one unsaved prompt, and `exitApproved`.

- [ ] **Step 1: Write failing continuation-table tests**

Add data-driven tests for dirty and clean Import, Open, and Exit requests. For dirty state, require one `unsavedDecisionRequested` signal and no replacement/exit before a decision. Then verify:

```cpp
controller.resolveUnsaved(UnsavedDecision::Cancel);  // retains state; clears pending action
controller.resolveUnsaved(UnsavedDecision::Discard); // emits the original path request or exitApproved
controller.resolveUnsaved(UnsavedDecision::Save);    // requests Save As when untitled
```

For titled documents, Save must persist first and continue only on success. A save failure must keep the pending action and allow Save, Discard, or Cancel again. Assert a second transition request while one is pending returns `InvalidConfig`, `"a document action is already pending"`.

- [ ] **Step 2: Run the focused test and verify the missing state machine fails**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_document_controller --test_output=errors`

Expected: FAIL to compile because the continuation enums and methods do not exist.

- [ ] **Step 3: Implement the explicit transition contract**

Add:

```cpp
enum class PendingDocumentAction { None, Import, Open, Exit };
enum class UnsavedDecision { Save, Discard, Cancel };
Q_ENUM(UnsavedDecision)

Q_INVOKABLE void requestImport();
Q_INVOKABLE void requestOpen();
Q_INVOKABLE void requestExit();
Q_INVOKABLE void resolveUnsaved(UnsavedDecision decision);
Q_INVOKABLE void cancelPathRequest();
Q_INVOKABLE void completeImportPath(const QString& path);
Q_INVOKABLE void completeOpenPath(const QString& path);
Q_INVOKABLE void completeSavePath(const QString& path);

signals:
    void importPathRequested();
    void openPathRequested();
    void savePathRequested();
    void unsavedDecisionRequested();
    void exitApproved();
```

Keep one private pending action and one `continuePendingAction()` function. Clean requests continue immediately. `completeImportPath` builds `LegacyCdbgImportDefaults` using the Global Constraints profile and the selected filename stem. Save As completion calls `saveAs` and continues only on success. File-dialog cancellation clears only the pending action and never changes document state.

- [ ] **Step 4: Run continuation and controller tests**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_document_controller --test_output=errors`

Expected: PASS.

- [ ] **Step 5: Commit the unsaved transition machine**

```bash
git add src/ui/desktop-quick/dashboard/dashboard_document_controller.h \
        src/ui/desktop-quick/dashboard/dashboard_document_controller.cpp \
        src/ui/desktop-quick/dashboard/dashboard_document_controller_test.cpp
git commit -m "feat(desktop-quick): resolve unsaved dashboard actions"
```

### Task 4: Add the Transactional Card Editor Model

**Files:**
- Create: `src/ui/desktop-quick/dashboard/dashboard_editor_model.h`
- Create: `src/ui/desktop-quick/dashboard/dashboard_editor_model.cpp`
- Create: `src/ui/desktop-quick/dashboard/dashboard_editor_model_test.cpp`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_document_controller.h`
- Modify: `src/ui/desktop-quick/dashboard/dashboard_document_controller.cpp`
- Modify: `src/ui/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: Task 2 `document()`, `commitCandidate(...)`, and selected stable card ID.
- Produces: a QML-facing card list, channel/conversion/display choices, selected-card properties, and typed add/remove/move/configuration commands.

- [ ] **Step 1: Write failing editor-model tests**

Cover role names `cardId`, `title`, `channelName`, `conversionId`, and `displayType`; exact document order; stable selection after moves; next/previous selection after remove; and availability properties. Add tests for:

```cpp
editor.addCard("CDBG_ENGINE_RPM", "rpm");
editor.setSelectedTitle("Tachometer");
editor.setSelectedDisplayType(CardDisplayType::Sparkline);
editor.setSelectedSparklineHistorySeconds(30);
editor.moveSelectedUp();
```

Require channel choices to contain only document channels and conversion choices to contain only the selected channel's embedded conversions. Changing channel selects its first conversion. Test gauge override and sparkline history edits, duplicate stable-ID prevention, semantic no-ops, invalid bounds/history rollback, and all operations rejected while connected.

- [ ] **Step 2: Run the new model test and verify it fails**

Run: `bazel test //src/ui/desktop-quick:test_dashboard_editor_model --test_output=errors`

Expected: FAIL because the target and type do not exist.

- [ ] **Step 3: Implement the model and typed mutations**

Declare `DashboardEditorModel : public QAbstractListModel` with:

```cpp
Q_PROPERTY(QString selectedCardId READ selectedCardId WRITE selectCard NOTIFY selectionChanged)
Q_PROPERTY(QVariantList channelChoices READ channelChoices NOTIFY choicesChanged)
Q_PROPERTY(QVariantList conversionChoices READ conversionChoices NOTIFY choicesChanged)
Q_PROPERTY(bool canAdd READ canAdd NOTIFY availabilityChanged)
Q_PROPERTY(bool canRemove READ canRemove NOTIFY availabilityChanged)
Q_PROPERTY(bool canMoveUp READ canMoveUp NOTIFY availabilityChanged)
Q_PROPERTY(bool canMoveDown READ canMoveDown NOTIFY availabilityChanged)
```

Expose invokables `addCard`, `removeSelected`, `moveSelectedUp`, `moveSelectedDown`, `setSelectedChannel`, `setSelectedConversion`, `setSelectedTitle`, `setSelectedDisplayType`, `setSelectedGaugeBounds`, and `setSelectedSparklineHistorySeconds`. Generate new IDs as `<channel-id>-<positive integer>` using the first unused suffix. Renumber every card's `order` after add/remove/move. Each command copies the document and calls `commitCandidate`; never mutate controller state in place.

- [ ] **Step 4: Run editor and document tests**

Run:

```bash
bazel test //src/ui/desktop-quick:test_dashboard_editor_model \
           //src/ui/desktop-quick:test_dashboard_document_controller \
           --test_output=errors
```

Expected: PASS.

- [ ] **Step 5: Commit the editor model**

```bash
git add src/ui/desktop-quick/dashboard/dashboard_editor_model.h \
        src/ui/desktop-quick/dashboard/dashboard_editor_model.cpp \
        src/ui/desktop-quick/dashboard/dashboard_editor_model_test.cpp \
        src/ui/desktop-quick/dashboard/dashboard_document_controller.h \
        src/ui/desktop-quick/dashboard/dashboard_document_controller.cpp \
        src/ui/desktop-quick/BUILD.bazel
git commit -m "feat(desktop-quick): edit dashboard cards transactionally"
```

### Task 5: Compose the Persistent Editor and Document Dialogs

**Files:**
- Create: `src/ui/desktop-quick/qml/dashboard/DashboardEditorPanel.qml`
- Create: `src/ui/desktop-quick/qml/dashboard/DocumentDialogs.qml`
- Modify: `src/ui/desktop-quick/qml/shell/ApplicationShell.qml`
- Modify: `src/ui/desktop-quick/qml.qrc`
- Modify: `src/ui/desktop-quick/BUILD.bazel`
- Modify: `src/ui/desktop-quick/desktop_quick_application_test.cpp`

**Interfaces:**
- Consumes: `dashboardDocuments`, `dashboardEditor`, `dashboardPresentation`, and `dashboardConnection` context properties.
- Produces: persistent side-panel controls, native file-path requests, one Save/Discard/Cancel dialog, and non-destructive error notification.

- [ ] **Step 1: Add failing offscreen UI tests**

Extend the fixture with document/editor controllers and require objects named:

```text
dashboardEditorPanel, importDashboardButton, openDashboardButton,
saveDashboardButton, saveAsDashboardButton, dashboardDirtyIndicator,
editorCardList, addCardButton, removeCardButton, moveCardUpButton,
moveCardDownButton, cardChannelCombo, cardConversionCombo,
cardDisplayTypeCombo, gaugeSettings, sparklineSettings,
unsavedChangesDialog, documentErrorBanner
```

Test panel persistence beside `dashboardView`, immediate preview after an editor command, selected-card stability after Move Up/Down, type-specific field visibility, and all mutation/document controls disabled with `"Disconnect to edit the dashboard"` while Running. Exercise Save/Discard/Cancel by invoking dialog buttons and assert the controller continuation signals.

- [ ] **Step 2: Run the application test and verify it fails**

Run: `bazel test //src/ui/desktop-quick:test_application --test_output=errors`

Expected: FAIL because the panel/dialog QML and context properties do not exist.

- [ ] **Step 3: Implement the persistent side panel**

Create a `Frame` with `Layout.preferredWidth: 340`, `Layout.fillHeight: true`, and a scrollable selected-card form. Bind enabled state only to controller/model properties. Use ComboBox value roles rather than array indexes for channel, conversion, and display type. Use Buttons for Move Up/Move Down with accessible names; do not add drag-and-drop.

Keep gauge controls visible only for `HorizontalGauge` and sparkline history only for `Sparkline`. Edits call typed model invokables on editing completion, not on every keystroke.

- [ ] **Step 4: Implement dialogs and shell composition**

`DocumentDialogs.qml` owns Qt Quick Dialogs `FileDialog` instances for CDBG import, Open `.ohd`, and Save As `.ohd`. Convert selected URLs to local file paths before calling controller completion methods. Wire the shared unsaved dialog to `resolveUnsaved(Save|Discard|Cancel)`. Show `errorOccurred(operation, detail)` in a dismissible banner; do not replace the dashboard.

Change the workspace body to a `RowLayout` containing `DashboardView` and `DashboardEditorPanel`, with the existing `ConnectionPanel` below both. Package both files in `qml.qrc` and `qml_resources`.

- [ ] **Step 5: Run offscreen application tests**

Run: `bazel test //src/ui/desktop-quick:test_application --test_output=errors`

Expected: PASS.

- [ ] **Step 6: Build both desktop applications**

Run: `bazel build //:fastecu //:fastecu-desktop-quick`

Expected: PASS.

- [ ] **Step 7: Commit the editor UI**

```bash
git add src/ui/desktop-quick/qml/dashboard/DashboardEditorPanel.qml \
        src/ui/desktop-quick/qml/dashboard/DocumentDialogs.qml \
        src/ui/desktop-quick/qml/shell/ApplicationShell.qml \
        src/ui/desktop-quick/qml.qrc src/ui/desktop-quick/BUILD.bazel \
        src/ui/desktop-quick/desktop_quick_application_test.cpp
git commit -m "feat(desktop-quick): compose dashboard editor panel"
```

### Task 6: Wire Production Lifecycle and End-to-End Restoration

**Files:**
- Modify: `src/ui/desktop-quick/desktop_quick_application.h`
- Modify: `src/ui/desktop-quick/desktop_quick_application.cpp`
- Modify: `src/ui/desktop-quick/desktop_quick_application_test.cpp`
- Modify: `apps/desktop-quick/main.cpp`
- Modify: `apps/desktop-quick/BUILD.bazel`
- Modify: `src/ui/desktop-quick/BUILD.bazel`

**Interfaces:**
- Consumes: all earlier controller/model contracts and Qt desktop port adapters.
- Produces: committed-document fan-out to preview/connection, startup restoration, close-event continuation, and the full import/edit/save/reload workflow.

- [ ] **Step 1: Write failing application wiring tests**

Change `load_root` to accept `DashboardDocumentController&` and `DashboardEditorModel&`. In the offscreen fixture, assert all four context properties refer to the supplied objects. Commit a candidate and require both `DashboardController` and `DashboardConnectionController` to receive the new document.

Add an end-to-end fake-port test:

```text
import valid CDBG catalog
add numeric, sparkline, and horizontal-gauge cards
move one card up
Save As "edited.ohd"
construct a fresh controller with the same settings/repository
restore recent document
assert identical card order, types, and configuration
```

Add a close-request test: dirty close opens the unsaved dialog; Cancel keeps the window; Discard emits `exitApproved` and closes it.

- [ ] **Step 2: Run application and new controller/model tests; verify wiring failures**

Run:

```bash
bazel test //src/ui/desktop-quick:test_application \
           //src/ui/desktop-quick:test_dashboard_document_controller \
           //src/ui/desktop-quick:test_dashboard_editor_model \
           --test_output=errors
```

Expected: FAIL because production composition still loads the bundled Colt document directly and does not expose the new controllers.

- [ ] **Step 3: Wire committed documents exactly once**

Extend `load_root` and set context properties:

```cpp
engine.rootContext()->setContextProperty(QStringLiteral("dashboardDocuments"), &dashboard_documents);
engine.rootContext()->setContextProperty(QStringLiteral("dashboardEditor"), &dashboard_editor);
```

Connect `DashboardDocumentController::documentCommitted` once in application composition. The handler reads `document()`, passes the same optional document to `DashboardController::setDocument` and `DashboardConnectionController::setDocument`, then tells `DashboardEditorModel` to reset from the committed document. Connect connection `stateChanged` to `setConnectionState` and editor availability refresh.

- [ ] **Step 4: Replace bundled startup with recent restoration**

In `apps/desktop-quick/main.cpp`, construct `QtSettings`, `DashboardDocumentController`, empty `DashboardController`, and `DashboardEditorModel`. Call `restoreRecentDocument()` before `load_root`. If no recent path exists, leave the Import/Open empty state. If restoration fails, emit the controller error and remain in that empty state rather than silently substituting the bundled dashboard.

Install a QML close handler that rejects the first close event, calls `requestExit`, and closes only after `exitApproved`; guard the second close so it cannot reopen the prompt.

- [ ] **Step 5: Run focused and package tests**

Run:

```bash
bazel test //src/ui/desktop-quick:all --test_output=errors
bazel test //src/backend/dashboard:all --test_output=errors
```

Expected: PASS.

- [ ] **Step 6: Run repository verification gates**

Run:

```bash
bazel test --config=release //... --test_output=errors
bazel build --config=release //:fastecu //:fastecu-desktop-quick
prek run --all-files
bazel run //:clang_tidy_report_changed
git diff --check
```

Expected: PASS. Platform-incompatible tests may be reported as skipped; no changed-file clang-tidy finding is accepted.

- [ ] **Step 7: Commit production lifecycle integration**

```bash
git add src/ui/desktop-quick/desktop_quick_application.h \
        src/ui/desktop-quick/desktop_quick_application.cpp \
        src/ui/desktop-quick/desktop_quick_application_test.cpp \
        src/ui/desktop-quick/BUILD.bazel \
        apps/desktop-quick/main.cpp apps/desktop-quick/BUILD.bazel
git commit -m "feat(desktop-quick): persist edited dashboards"
```
