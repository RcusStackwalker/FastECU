#pragma once

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/ports/clock.h"
#include "src/ui/desktop-quick/dashboard/dashboard_card_model.h"
#include "src/ui/desktop-quick/dashboard/dashboard_connection_controller.h"

class QTimer;

namespace fastecu::desktop_quick
{

class DashboardController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *cards READ cards NOTIFY documentChanged)
    Q_PROPERTY(QString dashboardTitle READ dashboardTitle NOTIFY documentChanged)
    Q_PROPERTY(bool hasLoadError READ hasLoadError NOTIFY documentChanged)
    Q_PROPERTY(QString loadErrorText READ loadErrorText NOTIFY documentChanged)

  public:
    DashboardController(std::optional<dashboard::DashboardDocument> document, ILoggingEngine& engine,
                        DashboardConnectionController& connection, IClock& clock, QObject *parent = nullptr);

    static std::unique_ptr<DashboardController> fromLoadError(QString error_text, ILoggingEngine& engine,
                                                              DashboardConnectionController& connection, IClock& clock);

    QAbstractItemModel *cards() const;
    QString dashboardTitle() const;
    bool hasLoadError() const;
    QString loadErrorText() const;
    void setDocument(std::optional<dashboard::DashboardDocument> document);

  signals:
    void documentChanged();
    void diagnostic(QString message);

  private slots:
    void queueSamples(QVector<fastecu::logging::LogSample> samples);
    void handleConnectionStateChanged();
    void flushPendingSamples();
    void updateAges();

  private:
    void reconcileAgeTimer();

    std::optional<dashboard::DashboardDocument> document_;
    DashboardCardModel *cards_;
    ILoggingEngine& engine_;
    DashboardConnectionController& connection_;
    IClock& clock_;
    QTimer *flush_timer_;
    QTimer *age_timer_;
    std::unordered_map<std::string, ReceivedLogSample> pending_samples_;
    QString dashboard_title_;
    QString load_error_text_;
    bool has_load_error_ = false;
};

} // namespace fastecu::desktop_quick
