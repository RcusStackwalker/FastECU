#include <QSignalSpy>
#include <QtTest>

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/backend/dashboard/dashboard_codec.h"
#include "src/backend/ports/testing/in_memory_atomic_file_writer.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/in_memory_settings.h"
#include "src/ui/desktop-quick/dashboard/dashboard_document_controller.h"

namespace fastecu::desktop_quick
{
namespace
{

constexpr std::string_view kLegacyCatalog = R"(<logger><protocols><protocol id="CDBG"><parameters>
  <parameter id="P1" name="First" desc="First description" length="2" enabled="1">
    <address>0x1234</address><conversions>
      <conversion units="rpm" expr="x*2" format="0" gauge_min="0" gauge_max="8000" gauge_step="500"/>
    </conversions>
  </parameter>
</parameters></protocol></protocols></logger>)";

std::vector<std::uint8_t> bytes_of(std::string_view text)
{
    return {text.begin(), text.end()};
}

dashboard::LegacyCdbgImportDefaults import_defaults()
{
    return dashboard::LegacyCdbgImportDefaults{
        .document_name = "Imported Colt",
        .bitrate = 500000,
        .identifier_width = dashboard::CanIdentifierWidth::Standard,
        .stream_instance = 2,
        .sampling_interval_ms = 25,
        .retry = dashboard::RetryPolicy{120, 4, 5, 300},
    };
}

dashboard::DashboardDocument base_document(std::string name = "Open Dashboard")
{
    return dashboard::DashboardDocument{
        .metadata = dashboard::DocumentMetadata{.format_version = 1, .name = std::move(name)},
        .connection =
            dashboard::CdbgConnectionProfile{
                .protocol = dashboard::DashboardProtocol::Cdbg,
                .transport = dashboard::DashboardTransport::RawCan,
                .bitrate = 500000,
                .identifier_width = dashboard::CanIdentifierWidth::Standard,
                .request_id = 0x630,
                .reply_id = 0x631,
                .stream_instance = 0,
                .sampling_interval_ms = 50,
                .retry = dashboard::RetryPolicy{100, 3, 3, 250},
                .preferred_adapter = std::nullopt,
            },
        .channels =
            {
                dashboard::DashboardChannel{
                    .id = "CDBG_ENGINE_RPM",
                    .name = "Engine RPM",
                    .description = "engine_rpm uint16",
                    .address = 0x804cfc,
                    .length = 2,
                    .raw_assembly = dashboard::RawAssembly::UnsignedIntegerDecimal,
                    .conversions =
                        {
                            dashboard::DashboardConversion{
                                .id = "conversion-1",
                                .expression = "x*1000/256",
                                .unit = "rpm",
                                .precision = 0,
                                .gauge_min = 0.0,
                                .gauge_max = 8000.0,
                                .gauge_step = 500.0,
                            },
                        },
                },
            },
        .cards = {},
    };
}

dashboard::DashboardDocument openable_document(std::string name = "Open Dashboard")
{
    auto document = base_document(std::move(name));
    document.cards = {
        dashboard::DashboardCard{
            .id = "rpm",
            .channel_id = "CDBG_ENGINE_RPM",
            .conversion_id = "conversion-1",
            .display_type = dashboard::CardDisplayType::Numeric,
            .title = std::nullopt,
            .order = 0,
            .gauge_bounds = std::nullopt,
            .sparkline_history_seconds = std::nullopt,
        },
    };
    return document;
}

struct ControllerState
{
    std::optional<dashboard::DashboardDocument> document;
    QString current_path;
    QString display_name;
    bool dirty;
    bool editing_enabled;

    bool operator==(const ControllerState&) const = default;
};

ControllerState state_of(const DashboardDocumentController& controller)
{
    return ControllerState{
        .document = controller.document(),
        .current_path = controller.currentPath(),
        .display_name = controller.displayName(),
        .dirty = controller.isDirty(),
        .editing_enabled = controller.editingEnabled(),
    };
}

struct Harness
{
    InMemoryFileRepository repository;
    InMemoryAtomicFileWriter writer;
    InMemorySettings settings;
    dashboard::DashboardDocumentService documents{repository, writer};
    DashboardDocumentController controller{documents, settings};
};

enum class RequestedAction
{
    Import,
    Open,
    Exit,
};

void request_action(DashboardDocumentController& controller, RequestedAction action)
{
    switch (action)
    {
    case RequestedAction::Import:
        controller.requestImport();
        return;
    case RequestedAction::Open:
        controller.requestOpen();
        return;
    case RequestedAction::Exit:
        controller.requestExit();
        return;
    }
}

int continuation_count(RequestedAction action, const QSignalSpy& import_requested, const QSignalSpy& open_requested,
                       const QSignalSpy& exit_approved)
{
    switch (action)
    {
    case RequestedAction::Import:
        return import_requested.count();
    case RequestedAction::Open:
        return open_requested.count();
    case RequestedAction::Exit:
        return exit_approved.count();
    }
    return 0;
}

} // namespace

class DashboardDocumentControllerTest : public QObject
{
    Q_OBJECT

  private:
    void install_document(std::string_view handle, const dashboard::DashboardDocument& document)
    {
        const auto encoded = dashboard::encode_dashboard_document(document);
        QVERIFY2(encoded.has_value(), encoded.error().detail.c_str());
        harness_->repository.files[std::string(handle)] = *encoded;
    }

    void install_clean_document()
    {
        install_document("original.ohd", openable_document("Original"));
        QVERIFY(harness_->controller.openDocument("original.ohd").has_value());
    }

    void install_dirty_titled_document()
    {
        install_clean_document();
        auto changed = *harness_->controller.document();
        changed.metadata.name = "Changed";
        QVERIFY(harness_->controller.commitCandidate(std::move(changed), "rpm").has_value());
    }

    void install_dirty_untitled_document()
    {
        harness_->repository.files["logger.xml"] = bytes_of(kLegacyCatalog);
        QVERIFY(harness_->controller.importDocument("logger.xml", import_defaults()).has_value());
    }

    std::unique_ptr<Harness> harness_;

  private slots:
    void init()
    {
        harness_ = std::make_unique<Harness>();
    }

    void importThenSaveAsCommitsTheExactLifecycleState()
    {
        harness_->repository.files["logger.xml"] = bytes_of(kLegacyCatalog);

        QVERIFY(!harness_->controller.hasDocument());
        QVERIFY(harness_->controller.importDocument("logger.xml", import_defaults()).has_value());
        QVERIFY(harness_->controller.hasDocument());
        QVERIFY(harness_->controller.isDirty());
        QCOMPARE(harness_->controller.currentPath(), QString{});

        QVERIFY(harness_->controller.saveAs("dashboard.ohd").has_value());
        QVERIFY(!harness_->controller.isDirty());
        QCOMPARE(harness_->controller.currentPath(), QStringLiteral("dashboard.ohd"));
        QCOMPARE(harness_->settings.get("desktop-quick/recent-dashboard"), std::optional<std::string>{"dashboard.ohd"});
    }

    void failedAndCancelledImportsPreserveTheCompletePriorState()
    {
        harness_->repository.files["logger.xml"] = bytes_of(kLegacyCatalog);
        QVERIFY(harness_->controller.importDocument("logger.xml", import_defaults()).has_value());
        const ControllerState before = state_of(harness_->controller);
        QSignalSpy committed(&harness_->controller, &DashboardDocumentController::documentCommitted);

        const Error expected{ErrorKind::Timeout, "catalog read timed out"};
        harness_->repository.next_read_result = std::unexpected(expected);
        const Status failed = harness_->controller.importDocument("missing.xml", import_defaults());

        QVERIFY(!failed.has_value());
        QCOMPARE(failed.error(), expected);
        QCOMPARE(state_of(harness_->controller), before);
        QCOMPARE(committed.count(), 0);

        const Status cancelled = harness_->controller.importDocument("", import_defaults());
        QVERIFY(!cancelled.has_value());
        QCOMPARE(cancelled.error().kind, ErrorKind::Cancelled);
        QCOMPARE(state_of(harness_->controller), before);
        QCOMPARE(committed.count(), 0);
    }

    void openPreparesBeforeCommitAndPersistsRecentPathOnlyOnSuccess()
    {
        const dashboard::DashboardDocument expected = openable_document();
        install_document("dashboard.ohd", expected);
        QSignalSpy committed(&harness_->controller, &DashboardDocumentController::documentCommitted);

        const Status status = harness_->controller.openDocument("dashboard.ohd");

        QVERIFY2(status.has_value(), status.error().detail.c_str());
        QCOMPARE(harness_->controller.document(), std::optional<dashboard::DashboardDocument>{expected});
        QCOMPARE(harness_->controller.currentPath(), QStringLiteral("dashboard.ohd"));
        QCOMPARE(harness_->controller.displayName(), QStringLiteral("dashboard.ohd"));
        QVERIFY(!harness_->controller.isDirty());
        QCOMPARE(harness_->settings.get(DashboardDocumentController::recentPathKey),
                 std::optional<std::string>{"dashboard.ohd"});
        QCOMPARE(committed.count(), 1);
    }

    void cardlessDocumentsCanBeOpenedAndRestored()
    {
        install_document("source.ohd", openable_document());
        QVERIFY(harness_->controller.openDocument("source.ohd").has_value());
        dashboard::DashboardDocument cardless = *harness_->controller.document();
        cardless.cards.clear();
        QVERIFY(harness_->controller.commitCandidate(cardless, {}).has_value());
        QVERIFY(harness_->controller.saveAs("cardless.ohd").has_value());
        harness_->repository.files["cardless.ohd"] = harness_->writer.files.at("cardless.ohd");

        const Status opened = harness_->controller.openDocument("cardless.ohd");

        QVERIFY2(opened.has_value(), opened.error().detail.c_str());
        QCOMPARE(harness_->controller.document(), std::optional<dashboard::DashboardDocument>{cardless});
        QCOMPARE(harness_->controller.selectedCardId(), QString{});
        QCOMPARE(harness_->controller.currentPath(), QStringLiteral("cardless.ohd"));
        QVERIFY(!harness_->controller.isDirty());

        Harness restoring;
        restoring.repository.files["cardless.ohd"] = harness_->writer.files.at("cardless.ohd");
        restoring.settings.set(DashboardDocumentController::recentPathKey, "cardless.ohd");

        const Status restored = restoring.controller.restoreRecentDocument();

        QVERIFY2(restored.has_value(), restored.error().detail.c_str());
        QCOMPARE(restoring.controller.document(), std::optional<dashboard::DashboardDocument>{cardless});
        QCOMPARE(restoring.controller.selectedCardId(), QString{});
        QCOMPARE(restoring.controller.currentPath(), QStringLiteral("cardless.ohd"));
        QVERIFY(!restoring.controller.isDirty());
    }

    void failedOpenAndFailedPreparationPreserveTheCompletePriorState()
    {
        install_document("original.ohd", openable_document("Original"));
        QVERIFY(harness_->controller.openDocument("original.ohd").has_value());
        harness_->settings.set(DashboardDocumentController::recentPathKey, "known-good.ohd");
        const ControllerState before = state_of(harness_->controller);
        QSignalSpy committed(&harness_->controller, &DashboardDocumentController::documentCommitted);

        const Error expected{ErrorKind::Disconnected, "document unavailable"};
        harness_->repository.next_read_result = std::unexpected(expected);
        const Status failed_read = harness_->controller.openDocument("unavailable.ohd");

        QVERIFY(!failed_read.has_value());
        QCOMPARE(failed_read.error(), expected);
        QCOMPARE(state_of(harness_->controller), before);
        QCOMPARE(harness_->settings.get(DashboardDocumentController::recentPathKey),
                 std::optional<std::string>{"known-good.ohd"});

        dashboard::DashboardDocument unprepared = openable_document();
        unprepared.connection.retry.poll_timeout_ms = static_cast<std::uint32_t>(std::numeric_limits<int>::max()) + 1U;
        install_document("unprepared.ohd", unprepared);
        const Status failed_prepare = harness_->controller.openDocument("unprepared.ohd");

        QVERIFY(!failed_prepare.has_value());
        QCOMPARE(failed_prepare.error(),
                 (Error{ErrorKind::InvalidConfig,
                        "connection.retry.poll-timeout-ms: exceeds the generic logging policy integer range"}));
        QCOMPARE(state_of(harness_->controller), before);
        QCOMPARE(harness_->settings.get(DashboardDocumentController::recentPathKey),
                 std::optional<std::string>{"known-good.ohd"});
        QCOMPARE(committed.count(), 0);
    }

    void cancelledOpenAndSaveAsPreserveTheCompletePriorState()
    {
        harness_->repository.files["logger.xml"] = bytes_of(kLegacyCatalog);
        QVERIFY(harness_->controller.importDocument("logger.xml", import_defaults()).has_value());
        const ControllerState before = state_of(harness_->controller);
        QSignalSpy committed(&harness_->controller, &DashboardDocumentController::documentCommitted);

        const Status cancelled_open = harness_->controller.openDocument("");
        const Status cancelled_save = harness_->controller.saveAs("");

        QVERIFY(!cancelled_open.has_value());
        QCOMPARE(cancelled_open.error().kind, ErrorKind::Cancelled);
        QVERIFY(!cancelled_save.has_value());
        QCOMPARE(cancelled_save.error().kind, ErrorKind::Cancelled);
        QCOMPARE(state_of(harness_->controller), before);
        QCOMPARE(harness_->settings.get(DashboardDocumentController::recentPathKey), std::optional<std::string>{});
        QCOMPARE(committed.count(), 0);
    }

    void failedSavePreservesDocumentPathDirtyStateAndRecentSetting()
    {
        const dashboard::DashboardDocument original = openable_document();
        install_document("original.ohd", original);
        QVERIFY(harness_->controller.openDocument("original.ohd").has_value());
        auto candidate = original;
        candidate.metadata.name = "Changed";
        QVERIFY(harness_->controller.commitCandidate(candidate, "rpm").has_value());
        harness_->settings.set(DashboardDocumentController::recentPathKey, "known-good.ohd");
        const ControllerState before = state_of(harness_->controller);

        const Error expected{ErrorKind::Internal, "disk full"};
        harness_->writer.replace_error = expected;
        const Status failed = harness_->controller.save();

        QVERIFY(!failed.has_value());
        QCOMPARE(failed.error(), expected);
        QCOMPARE(state_of(harness_->controller), before);
        QCOMPARE(harness_->settings.get(DashboardDocumentController::recentPathKey),
                 std::optional<std::string>{"known-good.ohd"});
    }

    void failedSaveAsDoesNotAdoptItsPath()
    {
        harness_->repository.files["logger.xml"] = bytes_of(kLegacyCatalog);
        QVERIFY(harness_->controller.importDocument("logger.xml", import_defaults()).has_value());
        const ControllerState before = state_of(harness_->controller);
        harness_->settings.set(DashboardDocumentController::recentPathKey, "known-good.ohd");
        const Error expected{ErrorKind::Internal, "permission denied"};
        harness_->writer.replace_error = expected;

        const Status failed = harness_->controller.saveAs("not-adopted.ohd");

        QVERIFY(!failed.has_value());
        QCOMPARE(failed.error(), expected);
        QCOMPARE(state_of(harness_->controller), before);
        QCOMPARE(harness_->controller.currentPath(), QString{});
        QCOMPARE(harness_->settings.get(DashboardDocumentController::recentPathKey),
                 std::optional<std::string>{"known-good.ohd"});
    }

    void commitCandidatePreparesBeforeReplacingAndSemanticNoOpStaysClean()
    {
        const dashboard::DashboardDocument original = openable_document();
        install_document("original.ohd", original);
        QVERIFY(harness_->controller.openDocument("original.ohd").has_value());
        QSignalSpy committed(&harness_->controller, &DashboardDocumentController::documentCommitted);

        QVERIFY(harness_->controller.commitCandidate(original, "rpm").has_value());
        QVERIFY(!harness_->controller.isDirty());
        QCOMPARE(committed.count(), 0);

        auto invalid = original;
        invalid.cards.front().conversion_id = "missing";
        const ControllerState before_invalid = state_of(harness_->controller);
        const Status failed = harness_->controller.commitCandidate(std::move(invalid), "rpm");

        QVERIFY(!failed.has_value());
        QCOMPARE(failed.error(), (Error{ErrorKind::InvalidConfig,
                                        "cards[rpm].conversion-id: does not reference a conversion on the channel"}));
        QCOMPARE(state_of(harness_->controller), before_invalid);
        QCOMPARE(committed.count(), 0);

        auto changed = original;
        changed.metadata.name = "Edited Dashboard";
        QVERIFY(harness_->controller.commitCandidate(changed, "rpm").has_value());
        QVERIFY(harness_->controller.isDirty());
        QCOMPARE(harness_->controller.document(), std::optional<dashboard::DashboardDocument>{changed});
        QCOMPARE(committed.count(), 1);
    }

    void connectedStateRejectsEveryLifecycleMutationWithTheRequiredError()
    {
        harness_->repository.files["logger.xml"] = bytes_of(kLegacyCatalog);
        install_document("dashboard.ohd", openable_document());
        QVERIFY(harness_->controller.openDocument("dashboard.ohd").has_value());
        const ControllerState before = state_of(harness_->controller);
        harness_->controller.setConnectionState(ConnectionState::Running);
        const ControllerState connected = state_of(harness_->controller);
        QVERIFY(before.editing_enabled);
        QVERIFY(!connected.editing_enabled);
        harness_->settings.kv.erase(std::string(DashboardDocumentController::recentPathKey));

        const std::vector<Status> statuses = {
            harness_->controller.importDocument("logger.xml", import_defaults()),
            harness_->controller.openDocument("dashboard.ohd"),
            harness_->controller.save(),
            harness_->controller.saveAs("other.ohd"),
            harness_->controller.restoreRecentDocument(),
            harness_->controller.commitCandidate(openable_document("Changed"), "rpm"),
        };

        for (const Status& status : statuses)
        {
            QVERIFY(!status.has_value());
            QCOMPARE(status.error(), (Error{ErrorKind::InvalidConfig, "disconnect before editing the dashboard"}));
        }
        QCOMPARE(state_of(harness_->controller), connected);
    }

    void restoreWithoutRecentPathIsANoOpAndFailedRestoreKeepsTheStoredPath()
    {
        QVERIFY(harness_->controller.restoreRecentDocument().has_value());
        QVERIFY(!harness_->controller.hasDocument());

        harness_->settings.set(DashboardDocumentController::recentPathKey, "missing.ohd");
        const Error expected{ErrorKind::Timeout, "recent document timed out"};
        harness_->repository.next_read_result = std::unexpected(expected);

        const Status failed = harness_->controller.restoreRecentDocument();

        QVERIFY(!failed.has_value());
        QCOMPARE(failed.error(), expected);
        QVERIFY(!harness_->controller.hasDocument());
        QCOMPARE(harness_->settings.get(DashboardDocumentController::recentPathKey),
                 std::optional<std::string>{"missing.ohd"});
    }

    void standaloneSavePersistsATitledDocumentOrRequestsAPathForAnUntitledDocument()
    {
        install_dirty_titled_document();
        QSignalSpy save_requested(&harness_->controller, &DashboardDocumentController::savePathRequested);

        harness_->controller.requestSave();

        QCOMPARE(save_requested.count(), 0);
        QCOMPARE(harness_->writer.replace_calls.size(), std::size_t{1});
        QCOMPARE(harness_->writer.replace_calls.front().handle, std::string{"original.ohd"});
        QVERIFY(!harness_->controller.isDirty());

        install_dirty_untitled_document();
        harness_->controller.requestSave();

        QCOMPARE(save_requested.count(), 1);
        harness_->controller.completeSavePath(QStringLiteral("imported.ohd"));
        QCOMPARE(harness_->controller.currentPath(), QStringLiteral("imported.ohd"));
        QVERIFY(!harness_->controller.isDirty());
    }

    void standaloneSaveAsCancellationAndFailureClearOnlyTheirOwnPendingRequest()
    {
        install_dirty_titled_document();
        QSignalSpy save_requested(&harness_->controller, &DashboardDocumentController::savePathRequested);
        QSignalSpy errors(&harness_->controller, &DashboardDocumentController::errorOccurred);

        harness_->controller.requestSaveAs();
        QCOMPARE(save_requested.count(), 1);
        harness_->controller.cancelPathRequest();
        harness_->controller.requestSaveAs();
        QCOMPARE(save_requested.count(), 2);

        harness_->writer.replace_error = Error{ErrorKind::Internal, "permission denied"};
        harness_->controller.completeSavePath(QStringLiteral("not-adopted.ohd"));
        QCOMPARE(errors.count(), 1);
        QCOMPARE(harness_->controller.currentPath(), QStringLiteral("original.ohd"));
        QVERIFY(harness_->controller.isDirty());

        harness_->writer.replace_error.reset();
        harness_->controller.requestSaveAs();
        QCOMPARE(save_requested.count(), 3);
        harness_->controller.completeSavePath(QStringLiteral("adopted.ohd"));
        QCOMPARE(harness_->controller.currentPath(), QStringLiteral("adopted.ohd"));
        QVERIFY(!harness_->controller.isDirty());
    }

    void cleanRequestsContinueImmediately_data()
    {
        QTest::addColumn<int>("requested_action");
        QTest::newRow("import") << static_cast<int>(RequestedAction::Import);
        QTest::newRow("open") << static_cast<int>(RequestedAction::Open);
        QTest::newRow("exit") << static_cast<int>(RequestedAction::Exit);
    }

    void cleanRequestsContinueImmediately()
    {
        QFETCH(int, requested_action);
        const auto action = static_cast<RequestedAction>(requested_action);
        install_clean_document();
        const ControllerState before = state_of(harness_->controller);
        QSignalSpy unsaved(&harness_->controller, &DashboardDocumentController::unsavedDecisionRequested);
        QSignalSpy import_requested(&harness_->controller, &DashboardDocumentController::importPathRequested);
        QSignalSpy open_requested(&harness_->controller, &DashboardDocumentController::openPathRequested);
        QSignalSpy exit_approved(&harness_->controller, &DashboardDocumentController::exitApproved);

        request_action(harness_->controller, action);

        QCOMPARE(unsaved.count(), 0);
        QCOMPARE(continuation_count(action, import_requested, open_requested, exit_approved), 1);
        QCOMPARE(import_requested.count() + open_requested.count() + exit_approved.count(), 1);
        QCOMPARE(state_of(harness_->controller), before);
    }

    void dirtyDecisionTable_data()
    {
        QTest::addColumn<int>("requested_action");
        QTest::addColumn<int>("decision");
        for (const auto [action_name, action] :
             {std::pair{"import", RequestedAction::Import}, std::pair{"open", RequestedAction::Open},
              std::pair{"exit", RequestedAction::Exit}})
        {
            QTest::newRow(qPrintable(QStringLiteral("%1-cancel").arg(action_name)))
                << static_cast<int>(action) << static_cast<int>(UnsavedDecision::Cancel);
            QTest::newRow(qPrintable(QStringLiteral("%1-discard").arg(action_name)))
                << static_cast<int>(action) << static_cast<int>(UnsavedDecision::Discard);
            QTest::newRow(qPrintable(QStringLiteral("%1-save").arg(action_name)))
                << static_cast<int>(action) << static_cast<int>(UnsavedDecision::Save);
        }
    }

    void dirtyDecisionTable()
    {
        QFETCH(int, requested_action);
        QFETCH(int, decision);
        const auto action = static_cast<RequestedAction>(requested_action);
        const auto unsaved_decision = static_cast<UnsavedDecision>(decision);
        install_dirty_untitled_document();
        const ControllerState before = state_of(harness_->controller);
        QSignalSpy unsaved(&harness_->controller, &DashboardDocumentController::unsavedDecisionRequested);
        QSignalSpy import_requested(&harness_->controller, &DashboardDocumentController::importPathRequested);
        QSignalSpy open_requested(&harness_->controller, &DashboardDocumentController::openPathRequested);
        QSignalSpy save_requested(&harness_->controller, &DashboardDocumentController::savePathRequested);
        QSignalSpy exit_approved(&harness_->controller, &DashboardDocumentController::exitApproved);

        request_action(harness_->controller, action);

        QCOMPARE(unsaved.count(), 1);
        QCOMPARE(import_requested.count() + open_requested.count() + save_requested.count() + exit_approved.count(), 0);
        QCOMPARE(state_of(harness_->controller), before);

        harness_->controller.resolveUnsaved(unsaved_decision);

        QCOMPARE(state_of(harness_->controller), before);
        if (unsaved_decision == UnsavedDecision::Discard)
        {
            QCOMPARE(continuation_count(action, import_requested, open_requested, exit_approved), 1);
            QCOMPARE(save_requested.count(), 0);
        }
        else if (unsaved_decision == UnsavedDecision::Save)
        {
            QCOMPARE(save_requested.count(), 1);
            QCOMPARE(import_requested.count() + open_requested.count() + exit_approved.count(), 0);
        }
        else
        {
            QCOMPARE(import_requested.count() + open_requested.count() + save_requested.count() + exit_approved.count(),
                     0);
            harness_->controller.requestExit();
            QCOMPARE(unsaved.count(), 2);
        }
    }

    void saveAsCompletionContinuesTheOriginalAction_data()
    {
        QTest::addColumn<int>("requested_action");
        QTest::newRow("import") << static_cast<int>(RequestedAction::Import);
        QTest::newRow("open") << static_cast<int>(RequestedAction::Open);
        QTest::newRow("exit") << static_cast<int>(RequestedAction::Exit);
    }

    void saveAsCompletionContinuesTheOriginalAction()
    {
        QFETCH(int, requested_action);
        const auto action = static_cast<RequestedAction>(requested_action);
        install_dirty_untitled_document();
        QSignalSpy import_requested(&harness_->controller, &DashboardDocumentController::importPathRequested);
        QSignalSpy open_requested(&harness_->controller, &DashboardDocumentController::openPathRequested);
        QSignalSpy save_requested(&harness_->controller, &DashboardDocumentController::savePathRequested);
        QSignalSpy exit_approved(&harness_->controller, &DashboardDocumentController::exitApproved);

        request_action(harness_->controller, action);
        harness_->controller.resolveUnsaved(UnsavedDecision::Save);
        QCOMPARE(save_requested.count(), 1);

        harness_->controller.completeSavePath(QStringLiteral("saved.ohd"));

        QCOMPARE(harness_->writer.replace_calls.size(), std::size_t{1});
        QCOMPARE(harness_->writer.replace_calls.front().handle, std::string{"saved.ohd"});
        QCOMPARE(harness_->controller.currentPath(), QStringLiteral("saved.ohd"));
        QVERIFY(!harness_->controller.isDirty());
        QCOMPARE(continuation_count(action, import_requested, open_requested, exit_approved), 1);
    }

    void titledSavePersistsBeforeContinuing_data()
    {
        QTest::addColumn<int>("requested_action");
        QTest::newRow("import") << static_cast<int>(RequestedAction::Import);
        QTest::newRow("open") << static_cast<int>(RequestedAction::Open);
        QTest::newRow("exit") << static_cast<int>(RequestedAction::Exit);
    }

    void titledSavePersistsBeforeContinuing()
    {
        QFETCH(int, requested_action);
        const auto action = static_cast<RequestedAction>(requested_action);
        install_dirty_titled_document();
        QSignalSpy import_requested(&harness_->controller, &DashboardDocumentController::importPathRequested);
        QSignalSpy open_requested(&harness_->controller, &DashboardDocumentController::openPathRequested);
        QSignalSpy save_requested(&harness_->controller, &DashboardDocumentController::savePathRequested);
        QSignalSpy exit_approved(&harness_->controller, &DashboardDocumentController::exitApproved);

        request_action(harness_->controller, action);
        harness_->controller.resolveUnsaved(UnsavedDecision::Save);

        QCOMPARE(save_requested.count(), 0);
        QCOMPARE(harness_->writer.replace_calls.size(), std::size_t{1});
        QCOMPARE(harness_->writer.replace_calls.front().handle, std::string{"original.ohd"});
        QVERIFY(!harness_->controller.isDirty());
        QCOMPARE(continuation_count(action, import_requested, open_requested, exit_approved), 1);
    }

    void failedSaveRetainsTheOriginalActionForEveryResolution_data()
    {
        QTest::addColumn<int>("retry_decision");
        QTest::newRow("save") << static_cast<int>(UnsavedDecision::Save);
        QTest::newRow("discard") << static_cast<int>(UnsavedDecision::Discard);
        QTest::newRow("cancel") << static_cast<int>(UnsavedDecision::Cancel);
    }

    void failedSaveRetainsTheOriginalActionForEveryResolution()
    {
        QFETCH(int, retry_decision);
        const auto decision = static_cast<UnsavedDecision>(retry_decision);
        install_dirty_titled_document();
        const ControllerState dirty_state = state_of(harness_->controller);
        harness_->writer.replace_error = Error{ErrorKind::Internal, "disk full"};
        QSignalSpy open_requested(&harness_->controller, &DashboardDocumentController::openPathRequested);
        QSignalSpy unsaved(&harness_->controller, &DashboardDocumentController::unsavedDecisionRequested);

        harness_->controller.requestOpen();
        QCOMPARE(unsaved.count(), 1);
        harness_->controller.resolveUnsaved(UnsavedDecision::Save);

        QCOMPARE(harness_->writer.replace_calls.size(), std::size_t{1});
        QCOMPARE(open_requested.count(), 0);
        QCOMPARE(state_of(harness_->controller), dirty_state);
        QCOMPARE(unsaved.count(), 2);

        harness_->writer.replace_error.reset();
        harness_->controller.resolveUnsaved(decision);

        if (decision == UnsavedDecision::Save)
        {
            QCOMPARE(harness_->writer.replace_calls.size(), std::size_t{2});
            QVERIFY(!harness_->controller.isDirty());
            QCOMPARE(open_requested.count(), 1);
        }
        else if (decision == UnsavedDecision::Discard)
        {
            QCOMPARE(state_of(harness_->controller), dirty_state);
            QCOMPARE(open_requested.count(), 1);
        }
        else
        {
            QCOMPARE(state_of(harness_->controller), dirty_state);
            QCOMPARE(open_requested.count(), 0);
            QSignalSpy exit_approved(&harness_->controller, &DashboardDocumentController::exitApproved);
            harness_->controller.requestExit();
            harness_->controller.resolveUnsaved(UnsavedDecision::Discard);
            QCOMPARE(exit_approved.count(), 1);
        }
    }

    void failedSaveAsRetainsTheOriginalAction_data()
    {
        QTest::addColumn<int>("recovery_decision");
        QTest::newRow("retry") << static_cast<int>(UnsavedDecision::Save);
        QTest::newRow("discard") << static_cast<int>(UnsavedDecision::Discard);
        QTest::newRow("cancel") << static_cast<int>(UnsavedDecision::Cancel);
    }

    void failedSaveAsRetainsTheOriginalAction()
    {
        QFETCH(int, recovery_decision);
        const auto decision = static_cast<UnsavedDecision>(recovery_decision);
        install_dirty_untitled_document();
        const ControllerState dirty_state = state_of(harness_->controller);
        harness_->writer.replace_error = Error{ErrorKind::Internal, "permission denied"};
        QSignalSpy exit_approved(&harness_->controller, &DashboardDocumentController::exitApproved);
        QSignalSpy save_requested(&harness_->controller, &DashboardDocumentController::savePathRequested);
        QSignalSpy unsaved(&harness_->controller, &DashboardDocumentController::unsavedDecisionRequested);

        harness_->controller.requestExit();
        QCOMPARE(unsaved.count(), 1);
        harness_->controller.resolveUnsaved(UnsavedDecision::Save);
        QCOMPARE(save_requested.count(), 1);
        harness_->controller.completeSavePath(QStringLiteral("not-adopted.ohd"));

        QCOMPARE(exit_approved.count(), 0);
        QCOMPARE(state_of(harness_->controller), dirty_state);
        QCOMPARE(unsaved.count(), 2);

        harness_->writer.replace_error.reset();
        harness_->controller.resolveUnsaved(decision);
        if (decision == UnsavedDecision::Save)
        {
            QCOMPARE(save_requested.count(), 2);
            harness_->controller.completeSavePath(QStringLiteral("adopted.ohd"));
            QCOMPARE(exit_approved.count(), 1);
            QCOMPARE(harness_->controller.currentPath(), QStringLiteral("adopted.ohd"));
            QVERIFY(!harness_->controller.isDirty());
        }
        else if (decision == UnsavedDecision::Discard)
        {
            QCOMPARE(exit_approved.count(), 1);
            QCOMPARE(state_of(harness_->controller), dirty_state);
        }
        else
        {
            QCOMPARE(exit_approved.count(), 0);
            harness_->controller.requestExit();
            QCOMPARE(unsaved.count(), 3);
        }
    }

    void aSecondTransitionCannotReplaceThePendingAction_data()
    {
        QTest::addColumn<int>("first_action");
        QTest::addColumn<int>("second_action");
        const std::vector<std::pair<const char *, RequestedAction>> actions = {
            {"import", RequestedAction::Import}, {"open", RequestedAction::Open}, {"exit", RequestedAction::Exit}};
        for (const auto& [first_name, first] : actions)
        {
            for (const auto& [second_name, second] : actions)
            {
                QTest::newRow(qPrintable(QStringLiteral("%1-then-%2").arg(first_name, second_name)))
                    << static_cast<int>(first) << static_cast<int>(second);
            }
        }
    }

    void aSecondTransitionCannotReplaceThePendingAction()
    {
        QFETCH(int, first_action);
        QFETCH(int, second_action);
        const auto first = static_cast<RequestedAction>(first_action);
        const auto second = static_cast<RequestedAction>(second_action);
        install_dirty_untitled_document();
        QSignalSpy unsaved(&harness_->controller, &DashboardDocumentController::unsavedDecisionRequested);
        QSignalSpy errors(&harness_->controller, &DashboardDocumentController::errorOccurred);
        QSignalSpy import_requested(&harness_->controller, &DashboardDocumentController::importPathRequested);
        QSignalSpy open_requested(&harness_->controller, &DashboardDocumentController::openPathRequested);
        QSignalSpy exit_approved(&harness_->controller, &DashboardDocumentController::exitApproved);

        request_action(harness_->controller, first);
        request_action(harness_->controller, second);

        QCOMPARE(unsaved.count(), 1);
        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.at(0).size(), 3);
        QCOMPARE(errors.at(0).at(1).toString(), QStringLiteral("a document action is already pending"));
        QCOMPARE(errors.at(0).at(2).value<ErrorKind>(), ErrorKind::InvalidConfig);

        harness_->controller.resolveUnsaved(UnsavedDecision::Discard);
        QCOMPARE(continuation_count(first, import_requested, open_requested, exit_approved), 1);
        if (second != first)
        {
            QCOMPARE(continuation_count(second, import_requested, open_requested, exit_approved), 0);
        }
    }

    void cancellingAnyPathRequestPreservesStateAndClearsThePendingAction_data()
    {
        QTest::addColumn<int>("path_kind");
        QTest::newRow("import") << 0;
        QTest::newRow("open") << 1;
        QTest::newRow("save-as") << 2;
    }

    void cancellingAnyPathRequestPreservesStateAndClearsThePendingAction()
    {
        QFETCH(int, path_kind);
        if (path_kind == 2)
        {
            install_dirty_untitled_document();
            harness_->controller.requestOpen();
            harness_->controller.resolveUnsaved(UnsavedDecision::Save);
        }
        else
        {
            install_clean_document();
            request_action(harness_->controller, path_kind == 0 ? RequestedAction::Import : RequestedAction::Open);
        }
        const ControllerState before = state_of(harness_->controller);

        harness_->controller.cancelPathRequest();

        QCOMPARE(state_of(harness_->controller), before);
        QSignalSpy errors(&harness_->controller, &DashboardDocumentController::errorOccurred);
        harness_->controller.requestExit();
        QCOMPARE(errors.count(), 0);
    }

    void completedImportUsesTheEstablishedProfileAndFilenameStem()
    {
        const QString path = QStringLiteral("catalogs/Imported.Catalog.xml");
        harness_->repository.files[path.toStdString()] = bytes_of(kLegacyCatalog);

        harness_->controller.requestImport();
        harness_->controller.completeImportPath(path);

        QVERIFY(harness_->controller.hasDocument());
        QCOMPARE(harness_->controller.document()->metadata.name, std::string{"Imported.Catalog"});
        const dashboard::CdbgConnectionProfile& profile = harness_->controller.document()->connection;
        QCOMPARE(profile.bitrate, std::uint32_t{500000});
        QCOMPARE(profile.identifier_width, dashboard::CanIdentifierWidth::Standard);
        QCOMPARE(profile.stream_instance, std::uint8_t{0});
        QCOMPARE(profile.sampling_interval_ms, std::uint32_t{50});
        QCOMPARE(profile.retry, (dashboard::RetryPolicy{100, 3, 3, 250}));
        QCOMPARE(harness_->controller.currentPath(), QString{});
        QVERIFY(harness_->controller.isDirty());
    }

    void completedOpenUsesTheTransactionalLifecycleAndClearsItsPendingAction()
    {
        const dashboard::DashboardDocument expected = openable_document("Selected");
        install_document("selected.ohd", expected);
        QSignalSpy errors(&harness_->controller, &DashboardDocumentController::errorOccurred);

        harness_->controller.requestOpen();
        harness_->controller.completeOpenPath(QStringLiteral("selected.ohd"));

        QCOMPARE(harness_->controller.document(), std::optional<dashboard::DashboardDocument>{expected});
        QCOMPARE(harness_->controller.currentPath(), QStringLiteral("selected.ohd"));
        QVERIFY(!harness_->controller.isDirty());
        harness_->controller.requestExit();
        QCOMPARE(errors.count(), 0);
    }

    void connectedRequestsPreserveTheLifecycleContract()
    {
        install_dirty_untitled_document();
        harness_->controller.setConnectionState(ConnectionState::Running);
        const ControllerState connected = state_of(harness_->controller);
        QSignalSpy import_requested(&harness_->controller, &DashboardDocumentController::importPathRequested);
        QSignalSpy open_requested(&harness_->controller, &DashboardDocumentController::openPathRequested);
        QSignalSpy save_requested(&harness_->controller, &DashboardDocumentController::savePathRequested);
        QSignalSpy unsaved(&harness_->controller, &DashboardDocumentController::unsavedDecisionRequested);
        QSignalSpy errors(&harness_->controller, &DashboardDocumentController::errorOccurred);

        harness_->controller.requestImport();
        harness_->controller.requestOpen();
        harness_->controller.requestSave();
        harness_->controller.requestSaveAs();

        QCOMPARE(import_requested.count() + open_requested.count() + save_requested.count(), 0);
        QCOMPARE(unsaved.count(), 0);
        QCOMPARE(errors.count(), 4);
        QCOMPARE(errors.at(0).at(1).toString(), QStringLiteral("disconnect before editing the dashboard"));
        QCOMPARE(errors.at(0).at(2).value<ErrorKind>(), ErrorKind::InvalidConfig);
        QCOMPARE(errors.at(1).at(1).toString(), QStringLiteral("disconnect before editing the dashboard"));
        QCOMPARE(errors.at(1).at(2).value<ErrorKind>(), ErrorKind::InvalidConfig);
        QCOMPARE(errors.at(2).at(1).toString(), QStringLiteral("disconnect before editing the dashboard"));
        QCOMPARE(errors.at(2).at(2).value<ErrorKind>(), ErrorKind::InvalidConfig);
        QCOMPARE(errors.at(3).at(1).toString(), QStringLiteral("disconnect before editing the dashboard"));
        QCOMPARE(errors.at(3).at(2).value<ErrorKind>(), ErrorKind::InvalidConfig);
        QCOMPARE(state_of(harness_->controller), connected);

        harness_->controller.requestExit();
        QCOMPARE(unsaved.count(), 1);
    }
};

} // namespace fastecu::desktop_quick

QTEST_GUILESS_MAIN(fastecu::desktop_quick::DashboardDocumentControllerTest)
#include "dashboard_document_controller_test.moc"
