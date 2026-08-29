#include "src/ui/desktop-quick/dashboard/dashboard_controller.h"

#include <QTimer>

#include <cmath>
#include <utility>

namespace fastecu::desktop_quick
{

DashboardController::DashboardController(dashboard::DashboardDocument document, ILoggingEngine& engine,
                                         DashboardConnectionController& connection, IClock& clock, QObject *parent)
    : QObject(parent), document_(std::move(document)), cards_(new DashboardCardModel(document_, this)), engine_(engine),
      connection_(connection), clock_(clock), flush_timer_(new QTimer(this)), age_timer_(new QTimer(this)),
      dashboard_title_(QString::fromStdString(document_.metadata.name))
{
    flush_timer_->setInterval(33);
    flush_timer_->setSingleShot(true);
    flush_timer_->setTimerType(Qt::PreciseTimer);
    age_timer_->setInterval(1000);

    connect(&engine_, &ILoggingEngine::valuesUpdated, this, &DashboardController::queueSamples);
    connect(&connection_, &DashboardConnectionController::stateChanged, this,
            &DashboardController::handleConnectionStateChanged);
    connect(flush_timer_, &QTimer::timeout, this, &DashboardController::flushPendingSamples);
    connect(age_timer_, &QTimer::timeout, this, &DashboardController::updateAges);
}

std::unique_ptr<DashboardController> DashboardController::fromLoadError(QString error_text, ILoggingEngine& engine,
                                                                        DashboardConnectionController& connection,
                                                                        IClock& clock, QObject *parent)
{
    auto controller =
        std::make_unique<DashboardController>(dashboard::DashboardDocument{}, engine, connection, clock, parent);
    controller->has_load_error_ = true;
    controller->load_error_text_ = std::move(error_text);
    return controller;
}

QAbstractItemModel *DashboardController::cards() const
{
    return cards_;
}

QString DashboardController::dashboardTitle() const
{
    return dashboard_title_;
}

bool DashboardController::hasLoadError() const
{
    return has_load_error_;
}

QString DashboardController::loadErrorText() const
{
    return load_error_text_;
}

void DashboardController::queueSamples(QVector<logging::LogSample> samples)
{
    for (logging::LogSample& sample : samples)
    {
        if (!cards_->containsChannel(sample.channel_id))
        {
            emit diagnostic(QStringLiteral("Ignored sample for unknown dashboard channel '%1'")
                                .arg(QString::fromStdString(sample.channel_id)));
            continue;
        }
        if (!std::isfinite(sample.numeric_value))
        {
            emit diagnostic(QStringLiteral("Ignored non-finite sample for dashboard channel '%1'")
                                .arg(QString::fromStdString(sample.channel_id)));
            continue;
        }

        std::string channel_id = sample.channel_id;
        pending_samples_.insert_or_assign(std::move(channel_id), std::move(sample));
    }

    if (!pending_samples_.empty() && !flush_timer_->isActive())
    {
        flush_timer_->start();
    }
    reconcileAgeTimer();
}

void DashboardController::handleConnectionStateChanged()
{
    switch (connection_.state())
    {
    case ConnectionState::CarNotResponding:
    case ConnectionState::Disconnecting:
    case ConnectionState::Disconnected:
    case ConnectionState::Failed:
        flush_timer_->stop();
        flushPendingSamples();
        cards_->markReceivedRowsStale();
        break;
    case ConnectionState::Connecting:
    case ConnectionState::AdapterSelectionRequired:
    case ConnectionState::Running:
        break;
    }
    reconcileAgeTimer();
}

void DashboardController::flushPendingSamples()
{
    flush_timer_->stop();
    if (pending_samples_.empty())
    {
        reconcileAgeTimer();
        return;
    }

    QVector<logging::LogSample> samples;
    samples.reserve(static_cast<qsizetype>(pending_samples_.size()));
    for (auto& [channel_id, sample] : pending_samples_)
    {
        static_cast<void>(channel_id);
        samples.push_back(std::move(sample));
    }
    pending_samples_.clear();

    cards_->applySamples(samples, clock_.now_ms(), connection_.state() == ConnectionState::Running);
    reconcileAgeTimer();
}

void DashboardController::updateAges()
{
    cards_->updateAges(clock_.now_ms());
    reconcileAgeTimer();
}

void DashboardController::reconcileAgeTimer()
{
    if (cards_->hasReceivedRows())
    {
        if (!age_timer_->isActive())
        {
            age_timer_->start();
        }
        return;
    }
    age_timer_->stop();
}

} // namespace fastecu::desktop_quick
