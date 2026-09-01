#include "src/ui/desktop/service_functions/service_function_dialog.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <array>
#include <cstdint>
#include <utility>

#include "src/backend/service_functions/read_parameters_session.h"
#include "src/backend/service_functions/relearn_session.h"
#include "src/backend/service_functions/service_function_session.h"
#include "src/backend/service_functions/set_parameters_session.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/service_functions/serial_facade_configurator.h"
#include "src/platform/desktop/common/service_functions/service_function_worker.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"

namespace fastecu::service_functions
{
namespace
{

QSpinBox *addSpinBox(QFormLayout& form, const QString& label, const char *name, int maximum)
{
    auto *spin = new QSpinBox;
    spin->setObjectName(QString::fromLatin1(name));
    spin->setRange(0, maximum);
    form.addRow(label, spin);
    return spin;
}

} // namespace

ServiceFunctionDialog::ServiceFunctionDialog(SerialPortActions *serial, std::string protocol, ServiceFunctionKind kind,
                                             QWidget *parent)
    : QDialog(parent), serial_(serial), protocol_(std::move(protocol)), kind_(kind),
      configurator_(std::make_unique<SerialPortActionsConfigurator>(serial))
{
    auto *layout = new QVBoxLayout(this);
    switch (kind_)
    {
    case ServiceFunctionKind::Relearn:
        setWindowTitle(tr("TCU Relearn"));
        break;
    case ServiceFunctionKind::ReadParameters:
        setWindowTitle(tr("Read TCU Parameters"));
        buildReadout(layout);
        break;
    case ServiceFunctionKind::SetParameters:
        setWindowTitle(tr("Set TCU Parameters"));
        buildParameterForm(layout);
        break;
    }

    progress_ = new QProgressBar;
    progress_->setObjectName("progress");
    progress_->setRange(0, 100);
    progress_->setValue(0);
    layout->addWidget(progress_);

    log_ = new QPlainTextEdit;
    log_->setObjectName("log");
    log_->setReadOnly(true);
    layout->addWidget(log_);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    start_ = buttons->addButton(tr("Start"), QDialogButtonBox::ActionRole);
    connect(start_, &QPushButton::clicked, this, &ServiceFunctionDialog::startWorker);
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::close);
    layout->addWidget(buttons);
}

ServiceFunctionDialog::~ServiceFunctionDialog() = default;

void ServiceFunctionDialog::buildParameterForm(QVBoxLayout *layout)
{
    auto *form = new QFormLayout;
    correction_1to2_ = addSpinBox(*form, tr("1->2 Pressure Correction (DC):"), "correction_1to2", 255);
    correction_2to3_ = addSpinBox(*form, tr("2->3 Pressure Correction (HLRC):"), "correction_2to3", 255);
    correction_3to4_ = addSpinBox(*form, tr("3->4 Pressure Correction (IC):"), "correction_3to4", 255);
    correction_4to5_ = addSpinBox(*form, tr("4->5 Pressure Correction (FB):"), "correction_4to5", 255);
    correction_forward_brake_ =
        addSpinBox(*form, tr("Fwd Brake Pressure Correction:"), "correction_forward_brake", 255);
    correction_four_wheel_drive_ =
        addSpinBox(*form, tr("4WD Pressure Correction:"), "correction_four_wheel_drive", 255);
    correction_line_pressure_ = addSpinBox(*form, tr("Line Pressure Correction:"), "correction_line_pressure", 255);
    temperature_basis_ = addSpinBox(*form, tr("Temp Basis for Corrections:"), "temperature_basis", 255);
    torque_correction_awd_ = addSpinBox(*form, tr("AWD Torque Correction:"), "torque_correction_awd", 65535);
    layout->addLayout(form);
}

void ServiceFunctionDialog::buildReadout(QVBoxLayout *layout)
{
    readout_ = new QTableWidget(9, 2);
    readout_->setObjectName("readout");
    readout_->setHorizontalHeaderLabels({tr("Parameter"), tr("Value")});
    readout_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    readout_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(readout_);
}

TcuParameterValues ServiceFunctionDialog::collectedValues() const
{
    if (kind_ != ServiceFunctionKind::SetParameters)
    {
        return {};
    }
    return TcuParameterValues{
        .correction_1to2 = static_cast<bytes::Byte>(correction_1to2_->value()),
        .correction_2to3 = static_cast<bytes::Byte>(correction_2to3_->value()),
        .correction_3to4 = static_cast<bytes::Byte>(correction_3to4_->value()),
        .correction_4to5 = static_cast<bytes::Byte>(correction_4to5_->value()),
        .correction_forward_brake = static_cast<bytes::Byte>(correction_forward_brake_->value()),
        .correction_four_wheel_drive = static_cast<bytes::Byte>(correction_four_wheel_drive_->value()),
        .correction_line_pressure = static_cast<bytes::Byte>(correction_line_pressure_->value()),
        .temperature_basis = static_cast<bytes::Byte>(temperature_basis_->value()),
        .torque_correction_awd = static_cast<std::uint16_t>(torque_correction_awd_->value()),
    };
}

void ServiceFunctionDialog::showReadout(const TcuParameterReadout& readout)
{
    if (readout_ == nullptr)
    {
        return;
    }
    const std::array rows{
        std::pair{tr("Input Clutch Pressure Correction"), static_cast<unsigned int>(readout.input_clutch)},
        std::pair{tr("High Low Reverse Clutch Pressure Correction"),
                  static_cast<unsigned int>(readout.high_low_reverse_clutch)},
        std::pair{tr("Direct Clutch Pressure Correction"), static_cast<unsigned int>(readout.direct_clutch)},
        std::pair{tr("Front Brake Pressure Correction"), static_cast<unsigned int>(readout.front_brake)},
        std::pair{tr("Correction of AWD Clutch Torque"), static_cast<unsigned int>(readout.awd_clutch_torque)},
        std::pair{tr("Forward Brake Pressure Correction"), static_cast<unsigned int>(readout.forward_brake)},
        std::pair{tr("4WD Pressure Correction"), static_cast<unsigned int>(readout.four_wheel_drive)},
        std::pair{tr("Line Pressure Correction"), static_cast<unsigned int>(readout.line_pressure)},
        std::pair{tr("Temperature basis for above Pressure Corrections"),
                  static_cast<unsigned int>(readout.temperature_basis)},
    };
    int row = 0;
    for (const auto& [label, value] : rows)
    {
        readout_->setItem(row, 0, new QTableWidgetItem(label));
        readout_->setItem(row, 1, new QTableWidgetItem(QString::number(value)));
        ++row;
    }
}

bool ServiceFunctionDialog::askGate(OperatorGateId id)
{
    QString text;
    switch (id)
    {
    case OperatorGateId::RelearnStaticSetup:
        // legacy :649-651
        text = tr("Engine must be at operating temperature. Car must be off the ground! "
                  "Start with Engine off, Ignition on, stick in P, press OK to continue");
        break;
    case OperatorGateId::RelearnEngineRunning:
        // legacy :736
        text = tr("Start Engine, let revs settle, move stick into D, fully press brake, press OK to continue");
        break;
    }
    return QMessageBox::information(this, tr("TCU Relearn"), text, QMessageBox::Ok | QMessageBox::Cancel,
                                    QMessageBox::Cancel) == QMessageBox::Ok;
}

void ServiceFunctionDialog::startWorker()
{
    if (serial_ == nullptr || worker_ != nullptr)
    {
        return;
    }

    std::unique_ptr<ServiceFunctionSession> session;
    switch (kind_)
    {
    case ServiceFunctionKind::Relearn:
        session = std::make_unique<RelearnSession>(protocol_);
        break;
    case ServiceFunctionKind::ReadParameters:
        session = std::make_unique<ReadParametersSession>(protocol_);
        break;
    case ServiceFunctionKind::SetParameters:
        session = std::make_unique<SetParametersSession>(protocol_, collectedValues());
        break;
    }

    worker_ =
        std::make_unique<ServiceFunctionWorker>(std::move(session), std::make_unique<FastEcuSsmTransport>(serial_),
                                                std::make_unique<QtClock>(), configurator_.get());
    connect(worker_.get(), &ServiceFunctionWorker::progressChanged, this, &ServiceFunctionDialog::setProgress);
    connect(worker_.get(), &ServiceFunctionWorker::logEvent, this,
            [this](int, const QString& message) { log_->appendPlainText(message); });
    connect(worker_.get(), &ServiceFunctionWorker::gateRequested, this, &ServiceFunctionDialog::handleGate);
    connect(worker_.get(), &ServiceFunctionWorker::finished, this, &ServiceFunctionDialog::workerFinished);
    start_->setEnabled(false);
    worker_->start();
}

void ServiceFunctionDialog::handleGate(int gateId)
{
    const bool accepted = askGate(static_cast<OperatorGateId>(gateId));
    if (worker_ != nullptr)
    {
        worker_->answerGate(gateId, accepted);
    }
}

void ServiceFunctionDialog::workerFinished(ServiceFunctionWorkerResult result)
{
    if (result.success && result.outcome.has_value())
    {
        if (const auto *readout = std::get_if<TcuParameterReadout>(&*result.outcome); readout != nullptr)
        {
            showReadout(*readout);
        }
        log_->appendPlainText(tr("Finished"));
        return;
    }
    log_->appendPlainText(result.error_detail);
}

void ServiceFunctionDialog::setProgress(int done, int total)
{
    progress_->setRange(0, total);
    progress_->setValue(done);
}

void ServiceFunctionDialog::closeEvent(QCloseEvent *event)
{
    if (worker_ != nullptr)
    {
        worker_->requestStop();
    }
    QDialog::closeEvent(event);
}

} // namespace fastecu::service_functions
