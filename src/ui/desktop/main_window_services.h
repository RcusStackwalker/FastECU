#pragma once
#include <string>

class FileActions;
class QtEventSink;
class QtFileRepository;
namespace fastecu::logging
{
class LoggerModel;
class LoggerDefinitionService;
} // namespace fastecu::logging
namespace fastecu::config
{
class ConfigSession;
}
namespace fastecu::calibration
{
class CalibrationWorkspace;
class RomSaveUseCase;
} // namespace fastecu::calibration
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
class RemotePeer;
} // namespace fastecu::ui

// Long-lived services MainWindow uses but does not own. A composition root
// (apps/desktop's DesktopComposition, or a test fixture) builds them, keeps
// them alive for MainWindow's whole lifetime, and passes this struct to its
// constructor.
// Composition-supplied application metadata.
struct ApplicationIdentity
{
    std::string name;
    std::string title;
    std::string version;
};

struct MainWindowServices
{
    const ApplicationIdentity& application;
    fastecu::config::ConfigSession& config; // initialized before MainWindow is built
    FileActions& file_actions;
    fastecu::calibration::CalibrationWorkspace& calibrations;
    fastecu::calibration::RomSaveUseCase& rom_save;
    fastecu::logging::LoggerModel& logger_model;
    fastecu::logging::LoggerDefinitionService& logger_definitions;
    QtFileRepository& config_repository;
    QtEventSink& file_action_events;
    fastecu::ui::LogChannel& log;
    fastecu::desktop::connection::AdapterConnection& connection;
    fastecu::ui::RemotePeer& remote;
    fastecu::desktop::logging::LoggingEngine& logging_engine;
};
