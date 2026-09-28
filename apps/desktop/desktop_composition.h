#pragma once

#include <memory>
#include <optional>

#include <QString>
#include <QStringList>

#include "apps/desktop/startup_event_sink.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/backend/calibration/session/rom_open.h"
#include "src/backend/config/config_session.h"
#include "src/backend/logging/logger_model.h"
#include "src/backend/logging/logger_definition_service.h"
#include "src/backend/definition/definition_service.h"
#include "src/backend/definitions/file_actions.h"
#include "src/backend/ports/error.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/platform/desktop/common/serial/desktop_serial_factory.h"
#include "src/platform/desktop/common/connection/adapter_connection.h"
#include "src/ui/desktop/main_window_services.h"
#include "src/ui/desktop/channels/log_channel.h"
#include "src/ui/desktop/channels/remote_peer.h"

class QThread;
class RemoteUtility;
class SystemLogger;
namespace fastecu::desktop::logging
{
class LoggingEngine;
}

// The desktop application's composition root: builds and owns every
// long-lived service MainWindow uses, and hands MainWindow non-owning
// references to them. Must outlive the MainWindow it serves.
//
// The configuration session is initialized first. If that fails, nothing
// else is built -- no logger, thread, or ECU connection -- and started()
// is false; main() presents startup_error() and exits.
class DesktopComposition
{
    friend class DesktopCompositionTest;

  public:
    // An empty config_root uses the platform's default FastECU directory.
    DesktopComposition(const QString& peer_address, const QString& peer_password, const QString& config_root = {});
    ~DesktopComposition();

    DesktopComposition(const DesktopComposition&) = delete;
    DesktopComposition& operator=(const DesktopComposition&) = delete;

    bool started() const;
    const std::optional<fastecu::Error>& startup_error() const;
    // Nonfatal configuration warnings raised while starting.
    QStringList startup_warnings() const;

    // Requires started().
    MainWindowServices services();

  private:
    QtFileSystem file_system_;
    QtResourceBundle resource_bundle_;
    QtFileRepository file_repository_;
    QtAtomicFileWriter file_writer_;
    QtEventSink file_action_events_;
    StartupEventSink startup_events_;
    fastecu::config::ConfigSession config_;
    fastecu::logging::LoggerModel logger_model_;
    fastecu::logging::LoggerDefinitionService logger_definitions_{file_repository_, resource_bundle_, file_writer_};
    std::optional<fastecu::Error> startup_error_;
    std::unique_ptr<FileActions> file_actions_;
    std::unique_ptr<fastecu::definition::DefinitionService> definition_service_;
    std::unique_ptr<fastecu::calibration::RomOpenUseCase> rom_open_;
    std::unique_ptr<fastecu::calibration::CalibrationWorkspace> calibration_workspace_;
    fastecu::ui::LogChannel log_channel_;
    fastecu::ui::RemotePeer remote_peer_;
    std::unique_ptr<QThread> syslog_thread_;
    std::unique_ptr<SystemLogger> syslogger_;
    OwnedSerialPortActions serial_;
    std::unique_ptr<fastecu::desktop::connection::AdapterConnection> connection_;
    std::unique_ptr<RemoteUtility> remote_utility_;
    QtClock logging_clock_;
    std::unique_ptr<fastecu::desktop::logging::LoggingEngine> logging_engine_;
};

// The desktop app's direct/remote rule: no --host means the local adapter.
SerialConnection serial_connection_from_args(const QString& host, const QString& password);
