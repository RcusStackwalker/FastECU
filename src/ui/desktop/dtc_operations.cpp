#include "src/ui/desktop/dtc_operations.h"

#include <QPushButton>
#include <QStandardItemModel>

#include <ui_dtc_operations.h>

#include "src/backend/diagnostics/dtc_session.h"
#include "src/backend/ports/event_sink.h"
#include "src/platform/desktop/common/ports/qt_clock.h"

using fastecu::diagnostics::DtcOperation;
using fastecu::diagnostics::DtcRequest;
using fastecu::diagnostics::DtcWorker;
using fastecu::diagnostics::DtcWorkerResult;
using fastecu::diagnostics::ObdProtocol;

DtcOperations::DtcOperations(fastecu::diagnostics::IDiagnosticLink& link, QWidget *parent)
    : QDialog(parent), link_(link), ui{std::make_unique<Ui::DtcOperationsWindow>()}
{
    ui->setupUi(this);

    ui->protocolComboBox->addItem("SSM (K-Line)");
    ui->protocolComboBox->addItem("SSM (CAN)");
    ui->protocolComboBox->addItem("iso9141");
    ui->protocolComboBox->addItem("iso14230");
    ui->protocolComboBox->addItem("iso15765");
    ui->protocolComboBox->setCurrentIndex(2);

    if (auto *model = qobject_cast<QStandardItemModel *>(ui->protocolComboBox->model()); model != nullptr)
    {
        for (int i = 0; i < ui->protocolComboBox->count(); i++)
        {
            if (auto *item = model->item(i); item != nullptr && item->text().startsWith("SSM "))
            {
                item->setEnabled(false);
            }
        }
    }

    connect(ui->readDtcButton, &QPushButton::clicked, this, [this] { start(DtcOperation::Read); });
    connect(ui->clearDtcButton, &QPushButton::clicked, this, [this] { start(DtcOperation::Clear); });
    connect(ui->closeButton, &QPushButton::clicked, this, &QDialog::close);

    this->show();
}

DtcOperations::~DtcOperations() = default;

void DtcOperations::start(DtcOperation operation)
{
    const QString text = ui->protocolComboBox->currentText();
    ObdProtocol protocol = ObdProtocol::Iso9141;
    if (text.startsWith("iso9141"))
    {
        protocol = ObdProtocol::Iso9141;
    }
    else if (text.startsWith("iso14230"))
    {
        protocol = ObdProtocol::Iso14230;
    }
    else if (text.startsWith("iso15765"))
    {
        protocol = ObdProtocol::Iso15765;
    }
    else
    {
        return; // SSM entries are disabled, as before
    }

    setButtonsEnabled(false);
    worker_ = std::make_unique<DtcWorker>(DtcRequest{protocol, operation}, link_, std::make_unique<QtClock>());
    connect(worker_.get(), &DtcWorker::logEvent, this, &DtcOperations::forwardLog, Qt::QueuedConnection);
    connect(worker_.get(), &DtcWorker::completed, this, &DtcOperations::finish, Qt::QueuedConnection);
    worker_->start();
}

void DtcOperations::forwardLog(int level, const QString& message)
{
    if (level == static_cast<int>(fastecu::LogLevel::Error))
    {
        emit LOG_E(message, true, true);
    }
    else if (level == static_cast<int>(fastecu::LogLevel::Warning))
    {
        emit LOG_W(message, true, true);
    }
    else if (level == static_cast<int>(fastecu::LogLevel::Debug))
    {
        emit LOG_D(message, true, true);
    }
    else
    {
        emit LOG_I(message, true, true);
    }
}

void DtcOperations::finish(const DtcWorkerResult& result)
{
    if (!result.success)
    {
        emit LOG_E("DTC operation failed: " + result.error_detail, true, true);
    }
    worker_.reset(); // joins; run() has already returned or is returning
    setButtonsEnabled(true);
}

void DtcOperations::setButtonsEnabled(bool enabled)
{
    ui->readDtcButton->setEnabled(enabled);
    ui->clearDtcButton->setEnabled(enabled);
}

void DtcOperations::closeEvent(QCloseEvent *event)
{
    if (worker_)
    {
        worker_->requestStop();
        worker_->wait();
        worker_.reset();
    }
    // Today's closeEvent reset the facade; the session epilogue already reset
    // after a run, and a second reset is harmless.
    static_cast<void>(link_.reset());
    QDialog::closeEvent(event);
}
