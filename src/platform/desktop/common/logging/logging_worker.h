#pragma once

#include <QMetaType>
#include <QThread>
#include <QVector>

#include "src/backend/logging/logging_session.h"
#include "src/backend/logging/logging_use_case.h"
#include "src/backend/ports/event_sink.h"
#include "src/platform/desktop/common/ports/qt_cancellation_token.h"

class LoggingWorker final : public QThread
{
    Q_OBJECT
  public:
    LoggingWorker(fastecu::logging::LoggingSession session,
                  fastecu::logging::LoggingProtocol *protocol,
                  fastecu::IEventSink& diagnostics, QObject *parent = nullptr);
    ~LoggingWorker() override;

    void requestStop();

  signals:
    void stateChanged(fastecu::logging::LoggingState state);
    void samplesReady(QVector<fastecu::logging::LogSample> samples);
    void sessionFinished(fastecu::Status result);

  protected:
    void run() override;

  private:
    fastecu::logging::LoggingUseCase use_case_;
    const fastecu::logging::LoggingSession session_;
    fastecu::logging::LoggingProtocol *protocol_;
    QtCancellationToken cancellation_;
    fastecu::IEventSink& diagnostics_;
    fastecu::Status result_{};
};

Q_DECLARE_METATYPE(fastecu::logging::LoggingState)
Q_DECLARE_METATYPE(fastecu::logging::LogSample)
Q_DECLARE_METATYPE(QVector<fastecu::logging::LogSample>)
Q_DECLARE_METATYPE(fastecu::Status)
