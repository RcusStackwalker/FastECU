#include "apps/desktop/desktop_composition.h"

#include <QThread>

#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/logging/systemlogger.h"
#include "src/platform/desktop/common/remote_utility/remote_utility.h"
#include "src/platform/desktop/common/transport/desktop_logging_protocol_registration.h"

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
    // The UI logs through the channel: the logger reads each line's level from
    // the channel's LOG_* signal name, and the channel outlives every sender.
    using fastecu::ui::LogChannel;
    QObject::connect(&log_channel_, &LogChannel::LOG_E, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(&log_channel_, &LogChannel::LOG_W, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(&log_channel_, &LogChannel::LOG_I, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(&log_channel_, &LogChannel::LOG_D, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(&log_channel_, &LogChannel::enable_log_write_to_file, syslogger_.get(),
                     &SystemLogger::enable_log_write_to_file);
    QObject::connect(syslogger_.get(), &SystemLogger::send_message_to_log_window, &log_channel_,
                     &LogChannel::log_window_message);
    QObject::connect(syslog_thread_.get(), &QThread::started, syslogger_.get(), &SystemLogger::run);
    syslog_thread_->start();

    serial_ = make_serial_port_actions(serial_connection_from_args(peer_address, peer_password), *syslogger_);
    connection_ = std::make_unique<fastecu::desktop::connection::AdapterConnection>(*serial_);
    remote_utility_ = std::make_unique<RemoteUtility>(peer_address, peer_password, nullptr, nullptr);

    // The UI reaches the remote utility only through the peer channel. The
    // mirror is dropped while the replica is not valid, as MainWindow did.
    using fastecu::ui::RemotePeer;
    QObject::connect(&remote_peer_, &RemotePeer::wait_requested, remote_utility_.get(), &RemoteUtility::waitForSource,
                     Qt::DirectConnection);
    QObject::connect(&remote_peer_, &RemotePeer::log_window_message, remote_utility_.get(),
                     [utility = remote_utility_.get()](const QString& message)
                     {
                         if (utility->isValid())
                         {
                             utility->send_log_window_message(message);
                         }
                     });
    QObject::connect(&remote_peer_, &RemotePeer::progress, remote_utility_.get(),
                     [utility = remote_utility_.get()](int value)
                     {
                         if (utility->isValid())
                         {
                             utility->set_progressbar_value(value);
                         }
                     });
    QObject::connect(remote_utility_.get(), &RemoteUtility::stateChanged, &remote_peer_, &RemotePeer::stateChanged);

    using fastecu::desktop::logging::LoggingEngine;
    logging_engine_ = std::make_unique<LoggingEngine>();
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_E, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_W, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_I, syslogger_.get(), &SystemLogger::log_messages);
    QObject::connect(logging_engine_.get(), &LoggingEngine::LOG_D, syslogger_.get(), &SystemLogger::log_messages);
    fastecu::desktop::logging::register_desktop_logging_protocols(*logging_engine_, *serial_, logging_clock_);
}

DesktopComposition::~DesktopComposition()
{
    // Dependents first: the engine's transports reference the serial facade,
    // and every service logs to the syslogger.
    logging_engine_.reset();
    remote_utility_.reset();
    connection_.reset();
    serial_.reset();
    // The logger lives on its own thread; stop and join it before deleting
    // it. (Before step 6c neither was ever stopped: SystemLogger::finished,
    // which the old wiring waited on, is never emitted.)
    syslog_thread_->quit();
    syslog_thread_->wait();
    syslogger_.reset();
}

SerialConnection serial_connection_from_args(const QString& host, const QString& password)
{
    if (host.isEmpty())
    {
        return DirectSerial{};
    }
    return RemoteSerial{.address = host, .password = password};
}

MainWindowServices DesktopComposition::services()
{
    return {
        .file_actions = file_actions_,
        .config_repository = file_repository_,
        .file_action_events = file_action_events_,
        .log = log_channel_,
        .connection = *connection_,
        .remote = remote_peer_,
        .logging_engine = *logging_engine_,
    };
}
