#pragma once

#include <memory>

#include <QString>

#include "src/backend/definitions/file_actions.h"
#include "src/platform/desktop/common/ports/qt_atomic_file_writer.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_event_sink.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/ports/qt_file_system.h"
#include "src/platform/desktop/common/ports/qt_resource_bundle.h"
#include "src/platform/desktop/common/serial/desktop_serial_factory.h"
#include "src/ui/desktop/main_window_services.h"

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
class DesktopComposition
{
  public:
    // An empty config_root uses the platform's default FastECU directory.
    DesktopComposition(const QString& peer_address, const QString& peer_password, const QString& config_root = {});
    ~DesktopComposition();

    DesktopComposition(const DesktopComposition&) = delete;
    DesktopComposition& operator=(const DesktopComposition&) = delete;

    MainWindowServices services();

  private:
    QtFileSystem file_system_;
    QtResourceBundle resource_bundle_;
    QtFileRepository file_repository_;
    QtAtomicFileWriter file_writer_;
    QtEventSink file_action_events_;
    FileActions file_actions_;
    std::unique_ptr<QThread> syslog_thread_;
    std::unique_ptr<SystemLogger> syslogger_;
    OwnedSerialPortActions serial_;
    std::unique_ptr<RemoteUtility> remote_utility_;
    QtClock logging_clock_;
    std::unique_ptr<fastecu::desktop::logging::LoggingEngine> logging_engine_;
};
