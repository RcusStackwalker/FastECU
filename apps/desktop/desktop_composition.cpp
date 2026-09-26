#include "apps/desktop/desktop_composition.h"

#include <QThread>

#include "src/platform/desktop/common/logging/systemlogger.h"

DesktopComposition::DesktopComposition(const QString& config_root)
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
}

DesktopComposition::~DesktopComposition()
{
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
    };
}
