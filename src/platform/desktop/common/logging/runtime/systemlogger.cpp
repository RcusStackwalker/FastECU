#include "src/platform/desktop/common/logging/runtime/systemlogger.h"

#include <utility>

SystemLogger::SystemLogger(QString file_path, QString software_name, QString software_version, QObject *parent)
    : QObject(parent), file_path_(std::move(file_path)), software_name_(std::move(software_name)),
      software_version_(std::move(software_version))
{
    QObject::connect(this, &SystemLogger::LOG_E, this, &SystemLogger::log_messages);
    QObject::connect(this, &SystemLogger::LOG_W, this, &SystemLogger::log_messages);
    QObject::connect(this, &SystemLogger::LOG_I, this, &SystemLogger::log_messages);
    QObject::connect(this, &SystemLogger::LOG_D, this, &SystemLogger::log_messages);
}

SystemLogger::~SystemLogger()
{
    if (syslog_file_open_)
    {
        syslog_file_.close();
    }
}

void SystemLogger::Run()
{
    emit LOG_I("SystemLogger started...", true, true);
}

void SystemLogger::enable_log_write_to_file(bool enable)
{
    write_syslog_to_file_ = enable;
}

void SystemLogger::log_messages(const QString& message, bool timestamp, bool linefeed)
{
    QString msg;

    QDateTime date_time = date_time.currentDateTime();

    QString date_time_string = date_time.toString("[yyyy-MM-dd hh':'mm':'ss'.'zzz'] ");
    QMetaMethod meta_method;

    if (sender())
    {
        meta_method = sender()->metaObject()->method(senderSignalIndex());
    }
    else
    {
        return;
    }

    // qDebug() << "metaMethod.name:" << metaMethod.name();

    // Check if timestamp added
    if (timestamp)
    {
        msg += date_time_string;

        // Check log type
        if (meta_method.name() == "LOG_E")
        {
            msg += "(EE) ";
        }
        else if (meta_method.name() == "LOG_W")
        {
            msg += "(WW) ";
        }
        else if (meta_method.name() == "LOG_I")
        {
            msg += "(II) ";
        }
        else if (meta_method.name() == "LOG_D")
        {
            msg += "(DD) ";
        }
    }
    msg += message;

    qDebug() << msg;

    // Check if linefeed added
    if (linefeed)
    {
        msg += "\n";
    }

    if (meta_method.name() != "LOG_D")
    {
        emit send_message_to_log_window(msg);
    }

    if (write_syslog_to_file_)
    {
        WriteSyslog(msg);
    }
}

bool SystemLogger::WriteSyslog(const QString& msg)
{
    // Open file for writing if needed
    if (!syslog_file_open_)
    {
        QDateTime date_time = date_time.currentDateTime();
        QString date_time_string = date_time.toString("yyyy-MM-dd_hh'h'mm'm'ss's'");

        QString syslog_file_name = file_path_;
        if (file_path_.at(file_path_.length() - 1) != '/')
        {
            syslog_file_name.append("/");
        }
        syslog_file_name.append("log_fastecu_" + date_time_string + ".txt");

        syslog_file_.setFileName(syslog_file_name);

        // qDebug() << "Create logfile: " << syslog_file_name;
        if (!syslog_file_.open(QIODevice::WriteOnly))
        {
            qDebug() << "Cannot open log file for writing";
            qDebug() << syslog_file_.errorString() + ": " + syslog_file_.fileName();
            return false;
        }

        syslog_file_open_ = true;
        syslog_file_init_ready_ = true;
        syslog_file_outstream_.setDevice(&syslog_file_);
        syslog_file_outstream_ << software_name_ + " v" + software_version_ +
                                      ", system log file, start time: " + date_time_string;
        syslog_file_outstream_ << "\n";
    }

    syslog_file_outstream_ << msg;
    syslog_file_outstream_.flush();

    return true;
}
