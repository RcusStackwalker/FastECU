#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <optional>

#include "src/platform/desktop/common/logging/logging_csv_file.h"
#include "src/platform/desktop/common/logging/runtime/logging_engine.h"

namespace fastecu::desktop::logging
{
struct DesktopLoggingRunStart
{
    LogSessionConfig session;
    DesktopLoggingSnapshot snapshot;
    QString csv_directory;
    QString csv_name_stem;
};

struct DesktopLoggingRunView
{
    std::optional<DesktopLoggingSnapshot> active_snapshot;
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
    explicit DesktopLoggingRun(LoggingEngine& engine, LoggingCsvFile csv_file = LoggingCsvFile{});
    ~DesktopLoggingRun() override;
    Status Start(DesktopLoggingRunStart request);
    void Stop();
    void SetCsvEnabled(bool enabled, const QString& directory, const QString& name_stem);
    void Shutdown();
    DesktopLoggingRunView View() const;

  signals:
    // NOLINTBEGIN(readability-identifier-naming): Qt signals keep Qt's camelBack names
    void presentationChanged();
    void sessionEnded(SessionEndReason reason, QString message);
    void sampleError(QString message);
    void csvError(QString message);
    void shutdownError(QString message);
    // NOLINTEND(readability-identifier-naming)

  private:
    void HandleSamples(const QVector<fastecu::logging::LogSample>& samples);
    void HandleCompletion(SessionEndReason reason, const QString& message);
    std::optional<QString> BeginCsv(const QString& directory, const QString& name_stem);
    std::optional<QString> ReleaseRun();
    void ReportCsvError(const QString& message);

    LoggingEngine& engine_;
    LoggingCsvFile csv_file_;
    std::optional<DesktopLoggingSnapshot> active_snapshot_;
    DesktopLoggerValues values_;
    QElapsedTimer elapsed_;
    QString csv_path_;
    bool csv_enabled_ = false;
    bool csv_failed_ = false;
    bool shutdown_ = false;
};
} // namespace fastecu::desktop::logging
