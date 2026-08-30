#include <QSignalSpy>
#include <QtTest>

#include <cstdint>
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

        install_document("unprepared.ohd", base_document());
        const Status failed_prepare = harness_->controller.openDocument("unprepared.ohd");

        QVERIFY(!failed_prepare.has_value());
        QCOMPARE(failed_prepare.error(), (Error{ErrorKind::InvalidConfig, "cards: no CDBG log parameters selected"}));
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
};

} // namespace fastecu::desktop_quick

QTEST_GUILESS_MAIN(fastecu::desktop_quick::DashboardDocumentControllerTest)
#include "dashboard_document_controller_test.moc"
