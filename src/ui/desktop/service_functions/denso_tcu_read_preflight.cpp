#include "src/ui/desktop/service_functions/denso_tcu_read_preflight.h"

#include <QAbstractButton>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>

#include <optional>
#include <utility>

#include "src/ui/desktop/service_functions/service_function_dialog.h"

namespace fastecu::service_functions
{
namespace
{

std::optional<ServiceFunctionKind> to_service_kind(DensoTcuReadAction action)
{
    switch (action)
    {
    case DensoTcuReadAction::Relearn:
        return ServiceFunctionKind::Relearn;
    case DensoTcuReadAction::ReadParameters:
        return ServiceFunctionKind::ReadParameters;
    case DensoTcuReadAction::SetParameters:
        return ServiceFunctionKind::SetParameters;
    case DensoTcuReadAction::Dump:
    case DensoTcuReadAction::Cancelled:
        return std::nullopt;
    }
    return std::nullopt;
}

bool confirm_tcu_ignition(QWidget *parent)
{
    QMessageBox message_box{QMessageBox::Warning, QObject::tr("Connecting to TCU"),
                            QObject::tr("Turn ignition ON and press OK to start initializing connection to TCU"),
                            QMessageBox::Ok | QMessageBox::Cancel, parent};
    message_box.setDefaultButton(QMessageBox::Ok);
    return message_box.exec() == QMessageBox::Ok;
}

} // namespace

DensoTcuReadAction choose_denso_tcu_read_action(QWidget *parent)
{
    QMessageBox message_box{parent};
    message_box.setText("Choose which option");
    message_box.setInformativeText("Perform TCU ROM Dump, Relearn, Read Parmeter or Set Parameter?");
    QPushButton *dump = message_box.addButton("Dump", QMessageBox::YesRole);
    QPushButton *relearn = message_box.addButton("Relearn", QMessageBox::YesRole);
    QPushButton *read_parameters = message_box.addButton("Read Param", QMessageBox::YesRole);
    QPushButton *set_parameters = message_box.addButton("Set Param", QMessageBox::YesRole);

    message_box.exec();

    const QAbstractButton *selected = message_box.clickedButton();
    if (selected == dump)
    {
        return DensoTcuReadAction::Dump;
    }
    if (selected == relearn)
    {
        return DensoTcuReadAction::Relearn;
    }
    if (selected == read_parameters)
    {
        return DensoTcuReadAction::ReadParameters;
    }
    if (selected == set_parameters)
    {
        return DensoTcuReadAction::SetParameters;
    }
    return DensoTcuReadAction::Cancelled;
}

bool run_denso_tcu_service_action(DensoTcuReadAction action, SerialPortActions *serial, std::string protocol,
                                  QWidget *parent)
{
    if (action == DensoTcuReadAction::Dump)
    {
        return false;
    }
    if (action == DensoTcuReadAction::Cancelled)
    {
        return true;
    }
    if (!confirm_tcu_ignition(parent))
    {
        return true;
    }

    const std::optional<ServiceFunctionKind> service_kind = to_service_kind(action);
    if (!service_kind.has_value())
    {
        return true;
    }
    ServiceFunctionDialog dialog{serial, std::move(protocol), *service_kind, parent};
    dialog.exec();
    return true;
}

} // namespace fastecu::service_functions
