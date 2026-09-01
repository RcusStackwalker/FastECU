#pragma once

#include <QDialog>

#include <memory>
#include <string>

#include "src/backend/service_functions/service_function_types.h"
#include "src/backend/service_functions/tcu_parameter_table.h"

class QCloseEvent;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QVBoxLayout;
class SerialPortActions;

namespace fastecu::service_functions
{

class SerialPortActionsConfigurator;
class ServiceFunctionWorker;
struct ServiceFunctionWorkerResult;

enum class ServiceFunctionKind
{
    Relearn,
    ReadParameters,
    SetParameters,
};

class ServiceFunctionDialog final : public QDialog
{
    Q_OBJECT

  public:
    ServiceFunctionDialog(SerialPortActions *serial, std::string protocol, ServiceFunctionKind kind,
                          QWidget *parent = nullptr);
    ~ServiceFunctionDialog() override;

    TcuParameterValues collectedValues() const;
    void showReadout(const TcuParameterReadout& readout);
    bool askGate(OperatorGateId id);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private slots:
    void startWorker();
    void handleGate(int gateId);

  private:
    void buildParameterForm(QVBoxLayout *layout);
    void buildReadout(QVBoxLayout *layout);
    void workerFinished(ServiceFunctionWorkerResult result);
    void setProgress(int done, int total);

    SerialPortActions *serial_;
    std::string protocol_;
    ServiceFunctionKind kind_;
    std::unique_ptr<SerialPortActionsConfigurator> configurator_;
    std::unique_ptr<ServiceFunctionWorker> worker_;

    QSpinBox *correction_1to2_ = nullptr;
    QSpinBox *correction_2to3_ = nullptr;
    QSpinBox *correction_3to4_ = nullptr;
    QSpinBox *correction_4to5_ = nullptr;
    QSpinBox *correction_forward_brake_ = nullptr;
    QSpinBox *correction_four_wheel_drive_ = nullptr;
    QSpinBox *correction_line_pressure_ = nullptr;
    QSpinBox *temperature_basis_ = nullptr;
    QSpinBox *torque_correction_awd_ = nullptr;

    QTableWidget *readout_ = nullptr;
    QProgressBar *progress_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QPushButton *start_ = nullptr;
};

} // namespace fastecu::service_functions
