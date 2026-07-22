#pragma once
#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>
#include "src/backend/ports/result.h"

// One converted, display-ready value for a single FileActions::LogValuesStructure
// entry (identified by its index into that struct's parallel QStringLists).
struct LogSample
{
    int logValueIndex;
    QString displayValue;
};

// Outcome of one successful LoggingProtocol::poll() call. Transport faults are
// reported as fastecu::Error, not as a status here.
struct PollData
{
    bool responded = false;     // false == car silent this cycle (was NoResponse)
    QVector<LogSample> samples; // valid when responded; already expression-converted
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
    virtual fastecu::Status start() = 0;
    virtual fastecu::Result<PollData> poll(int timeoutMs) = 0;
    virtual void stop() = 0;
};

Q_DECLARE_METATYPE(LogSample)
Q_DECLARE_METATYPE(QVector<LogSample>)
Q_DECLARE_METATYPE(LoggingStatus)
Q_DECLARE_METATYPE(SessionEndReason)
