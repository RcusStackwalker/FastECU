#pragma once
#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

// One converted, display-ready value for a single FileActions::LogValuesStructure
// entry (identified by its index into that struct's parallel QStringLists).
struct LogSample
{
    int logValueIndex;
    QString displayValue;
};

// Outcome of one LoggingProtocol::poll() call.
struct PollResult
{
    enum class Status
    {
        Ok,
        NoResponse,
        TransportError
    };
    Status status = Status::NoResponse;
    QVector<LogSample> samples; // valid when status == Ok; already expression-converted
    QString errorMessage;       // valid when status == TransportError
};

// Recoverable connectivity state of an active logging session.
enum class LoggingStatus
{
    Running,
    CarNotResponding
};

// Why a logging session's thread ended.
enum class SessionEndReason
{
    StoppedByUser,
    HandshakeFailed,
    AdapterDisconnected
};

// One implementation per wire protocol (SSM, MUT/DMA, Cdbg, ...). Polling-type
// protocols send a request and block for the response inside poll(); setup-and-listen
// protocols block until the next pushed frame arrives. Either way, poll() is called
// back-to-back by LoggingWorker with no imposed delay between calls.
class LoggingProtocol
{
  public:
    virtual ~LoggingProtocol() = default;
    virtual bool start(QString *errorOut) = 0;
    virtual PollResult poll(int timeoutMs) = 0;
    virtual void stop() = 0;
};

Q_DECLARE_METATYPE(LogSample)
Q_DECLARE_METATYPE(QVector<LogSample>)
Q_DECLARE_METATYPE(LoggingStatus)
Q_DECLARE_METATYPE(SessionEndReason)
