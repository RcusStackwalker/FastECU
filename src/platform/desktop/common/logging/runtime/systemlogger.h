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

    void Run();

  private:
    QString file_path_;
    QString software_name_;
    QString software_version_;

    bool write_syslog_to_file_ = false;
    bool syslog_file_open_ = false;
    bool syslog_file_init_ready_ = false;

    QFile syslog_file_;
    QTextStream syslog_file_outstream_;

    bool WriteSyslog(const QString& msg);

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void sendMessageToLogWindow(QString msg);
    void finished();
    void error(QString err);
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);
    // NOLINTEND(readability-identifier-naming)

  public slots:
    // NOLINTBEGIN(readability-identifier-naming): Qt slots keep Qt's camelBack names
    void enableLogWriteToFile(bool enable);
    void logMessages(const QString& message, bool timestamp, bool linefeed);
    // NOLINTEND(readability-identifier-naming)
};
