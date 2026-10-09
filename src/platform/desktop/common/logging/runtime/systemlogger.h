#pragma once

#include <QApplication>
#include <QDebug>
#include <QFile>
#include <QMetaMethod>
#include <QTime>

class SystemLogger : public QObject
{
    Q_OBJECT

  public:
    SystemLogger(QString file_path, QString sw_name, QString sw_ver, QObject *parent = nullptr);
    ~SystemLogger();

    void run();

  private:
    QString file_path_;
    QString software_name_;
    QString software_version_;

    bool write_syslog_to_file_ = false;
    bool syslog_file_open_ = false;
    bool syslog_file_init_ready_ = false;

    QFile syslog_file_;
    QTextStream syslog_file_outstream_;

    bool write_syslog(const QString& msg);

  signals:
    void send_message_to_log_window(QString msg);
    void finished();
    void error(QString err);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  public slots:
    void enable_log_write_to_file(bool enable);
    void log_messages(const QString& message, bool timestamp, bool linefeed);
};
