#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <cstdint>
#include <optional>

#include "src/platform/desktop/common/logging/logging_csv_file.h"
#include "src/platform/desktop/common/logging/runtime/logging_engine.h"

namespace fastecu::desktop::logging
{
struct DesktopLoggingRunStart
{
    LogSessionConfig session;
    fastecu::logging::LoggingRunSnapshot snapshot;
    QString csv_directory;
    QString csv_name_stem;
};

struct DesktopLoggingRunView
{
    std::optional<fastecu::logging::LoggingRunSnapshot> active_snapshot;
    DesktopLoggerValues values;
    bool acquisition_running = false;
    bool csv_enabled = false;
    bool csv_open = false;
    QString csv_path;
};

// Calls and notifications belong to the engine's owning Qt thread. The engine
// must outlive this window-owned consumer; View returns an independent copy.
class DesktopLoggingRun final : public QObject
{
    Q_OBJECT
  public:
    explicit DesktopLoggingRun(LoggingEngine& engine, LoggingCsvFile csvFile = LoggingCsvFile{});
    ~DesktopLoggingRun() override;
    Status start(DesktopLoggingRunStart request);
    void stop();
    void setCsvEnabled(bool enabled, const QString& directory, const QString& nameStem);
    void shutdown();
    DesktopLoggingRunView view() const;

  signals:
    void presentationChanged();
    void sessionEnded(SessionEndReason reason, QString message);
    void sampleError(QString message);
    void csvError(QString message);
    void shutdownError(QString message);

  private:
    void handleSamples(const QVector<fastecu::logging::LogSample>& samples);
    void handleCompletion(SessionEndReason reason, const QString& message);
    std::optional<QString> beginCsv(const QString& directory, const QString& nameStem);
    std::optional<QString> releaseRun();
    void reportCsvError(const QString& message);

    LoggingEngine& engine_;
    LoggingCsvFile csv_file_;
    std::optional<fastecu::logging::LoggingRunSnapshot> active_snapshot_;
    DesktopLoggerValues values_;
    QElapsedTimer elapsed_;
    QString csv_path_;
    std::uint64_t run_id_ = 0;
    bool csv_enabled_ = false;
    bool csv_failed_ = false;
    bool shutdown_ = false;
};
} // namespace fastecu::desktop::logging
