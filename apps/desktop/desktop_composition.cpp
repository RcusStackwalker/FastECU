#include "apps/desktop/desktop_composition.h"

#include <QThread>

#include "apps/desktop/default_config_root.h"

#include "src/platform/desktop/common/logging/logging_engine.h"
#include "src/platform/desktop/common/logging/systemlogger.h"
#include "src/platform/desktop/common/remote_utility/remote_utility.h"
#include "src/platform/desktop/common/transport/desktop_logging_protocol_registration.h"

namespace
{
const ApplicationIdentity kApplication{.name = "FastECU", .title = "FastECU", .version = "0.1.0-beta.5"};
} // namespace

DesktopComposition::DesktopComposition(const QString& peer_address, const QString& peer_password,
                                       const QString& config_root)
    : config_(file_system_, resource_bundle_, file_repository_, startup_events_)
{
    const QString root = config_root.isEmpty() ? default_config_root() : config_root;
    if (fastecu::Status initialized = config_.initialize(root.toStdString(), kApplication.version);
        !initialized.has_value())
    {
        // Required configuration is missing or broken: build nothing that
        // could log, spawn a thread, or reach an ECU. main() presents it.
        startup_error_ = initialized.error();
        return;
    }

    file_actions_ = std::make_unique<FileActions>(file_system_, resource_bundle_, file_repository_, file_writer_,
                                                  file_action_events_, config_);

    syslog_thread_ = std::make_unique<QThread>();
    syslogger_ = std::make_unique<SystemLogger>(
        QString::fromStdString(config_.effective_paths().syslog_files_directory),
        QString::fromStdString(kApplication.name), QString::fromStdString(kApplication.version));
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
    // and every service logs to the syslogger. After a failed start none of
    // these exist.
    logging_engine_.reset();
    remote_utility_.reset();
    connection_.reset();
    serial_.reset();
    // The logger lives on its own thread; stop and join it before deleting
    // it. (Before step 6c neither was ever stopped: SystemLogger::finished,
    // which the old wiring waited on, is never emitted.)
    if (syslog_thread_)
    {
        syslog_thread_->quit();
        syslog_thread_->wait();
    }
    syslogger_.reset();
    syslog_thread_.reset();
    file_actions_.reset(); // before config_, which it references
}

bool DesktopComposition::started() const
{
    return !startup_error_.has_value();
}

const std::optional<fastecu::Error>& DesktopComposition::startup_error() const
{
    return startup_error_;
}

QStringList DesktopComposition::startup_warnings() const
{
    return startup_events_.warnings();
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
        .application = kApplication,
        .config = config_,
        .file_actions = *file_actions_,
        .config_repository = file_repository_,
        .file_action_events = file_action_events_,
        .log = log_channel_,
        .connection = *connection_,
        .remote = remote_peer_,
        .logging_engine = *logging_engine_,
    };
}
