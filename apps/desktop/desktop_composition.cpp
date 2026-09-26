#include "apps/desktop/desktop_composition.h"

#include <QThread>

#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/logging/systemlogger.h"
#include "src/platform/desktop/common/remote_utility/remote_utility.h"

DesktopComposition::DesktopComposition(const QString& peer_address, const QString& peer_password,
                                       const QString& config_root)
    : file_actions_(file_system_, resource_bundle_, file_repository_, file_writer_, file_action_events_)
{
    FileActions::ConfigValuesStructure *config = &file_actions_.ConfigValuesStruct;
    file_actions_.set_base_dirs(config,
                                (config_root.isEmpty() ? config->base_config_directory : config_root).toStdString());

    syslog_thread_ = std::make_unique<QThread>();
    syslogger_ =
        std::make_unique<SystemLogger>(config->syslog_files_directory, config->software_name, config->software_version);
    syslogger_->moveToThread(syslog_thread_.get());
    QObject::connect(syslog_thread_.get(), &QThread::started, syslogger_.get(), &SystemLogger::run);
    syslog_thread_->start();

    serial_ = make_serial_port_actions(peer_address, peer_password, *syslogger_);
    remote_utility_ = std::make_unique<RemoteUtility>(peer_address, peer_password, nullptr, nullptr);

    using fastecu::desktop::logging::LoggingEngine;
    logging_engine_ = std::make_unique<LoggingEngine>();
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_E, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_W, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_I, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_D, syslogger_.get(), &SystemLogger::log_messages);
}

DesktopComposition::~DesktopComposition()
{
    // Dependents first: the engine's transports reference the serial facade,
    // and every service logs to the syslogger.
    logging_engine_.reset();
    remote_utility_.reset();
    serial_.reset();
    // The logger lives on its own thread; stop and join it before deleting
    // it. (Before step 6c neither was ever stopped: SystemLogger::finished,
    // which the old wiring waited on, is never emitted.)
    syslog_thread_->quit();
    syslog_thread_->wait();
    syslogger_.reset();
}

MainWindowServices DesktopComposition::services()
{
    return {
        .file_actions = file_actions_,
        .config_repository = file_repository_,
        .file_action_events = file_action_events_,
        .syslogger = *syslogger_,
        .serial = *serial_,
        .remote_utility = *remote_utility_,
        .logging_engine = *logging_engine_,
        .logging_clock = logging_clock_,
    };
}
