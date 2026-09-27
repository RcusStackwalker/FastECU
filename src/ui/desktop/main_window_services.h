#pragma once

class FileActions;
class QtEventSink;
class QtFileRepository;
class RemoteUtility;
class SystemLogger;
namespace fastecu::desktop::connection
{
class AdapterConnection;
}
namespace fastecu::desktop::logging
{
class LoggingEngine;
}
namespace fastecu::ui
{
class LogChannel;
}

// Long-lived services MainWindow uses but does not own. A composition root
// (apps/desktop's DesktopComposition, or a test fixture) builds them, keeps
// them alive for MainWindow's whole lifetime, and passes this struct to its
// constructor.
struct MainWindowServices
{
    FileActions& file_actions; // set_base_dirs already applied
    QtFileRepository& config_repository;
    QtEventSink& file_action_events;
    SystemLogger& syslogger;
    fastecu::ui::LogChannel& log;
    fastecu::desktop::connection::AdapterConnection& connection;
    RemoteUtility& remote_utility;
    fastecu::desktop::logging::LoggingEngine& logging_engine;
};
