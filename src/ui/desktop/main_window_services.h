#pragma once

class FileActions;
class QtEventSink;
class QtFileRepository;
class RemoteUtility;
class SerialPortActions;
class SystemLogger;
namespace fastecu::desktop::logging
{
class LoggingEngine;
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
    SerialPortActions& serial;
    RemoteUtility& remote_utility;
    fastecu::desktop::logging::LoggingEngine& logging_engine;
};
