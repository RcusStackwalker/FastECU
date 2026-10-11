#include "src/ui/desktop/service_functions/denso_tcu_read_preflight.h"

#include <QAbstractButton>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>

#include <optional>
#include <utility>

#include "src/ui/desktop/service_functions/dialog/service_function_dialog.h"

namespace fastecu::service_functions
{
namespace
{

std::optional<ServiceFunctionKind> toServiceKind(DensoTcuReadAction action)
{
    switch (action)
    {
    case DensoTcuReadAction::kRelearn:
        return ServiceFunctionKind::kRelearn;
    case DensoTcuReadAction::kReadParameters:
        return ServiceFunctionKind::kReadParameters;
    case DensoTcuReadAction::kSetParameters:
        return ServiceFunctionKind::kSetParameters;
    case DensoTcuReadAction::kDump:
    case DensoTcuReadAction::kCancelled:
        return std::nullopt;
    }
    return std::nullopt;
}

bool confirmTcuIgnition(QWidget *parent)
{
    QMessageBox messageBox{QMessageBox::Warning, QObject::tr("Connecting to TCU"),
                           QObject::tr("Turn ignition ON and press OK to start initializing connection to TCU"),
                           QMessageBox::Ok | QMessageBox::Cancel, parent};
    messageBox.setDefaultButton(QMessageBox::Ok);
    return messageBox.exec() == QMessageBox::Ok;
}

} // namespace

DensoTcuReadAction chooseDensoTcuReadAction(QWidget *parent)
{
    QMessageBox messageBox{parent};
    messageBox.setText("Choose which option");
    messageBox.setInformativeText("Perform TCU ROM Dump, Relearn, Read Parmeter or Set Parameter?");
    QPushButton *dump = messageBox.addButton("Dump", QMessageBox::YesRole);
    QPushButton *relearn = messageBox.addButton("Relearn", QMessageBox::YesRole);
    QPushButton *readParameters = messageBox.addButton("Read Param", QMessageBox::YesRole);
    QPushButton *setParameters = messageBox.addButton("Set Param", QMessageBox::YesRole);

    messageBox.exec();

    const QAbstractButton *selected = messageBox.clickedButton();
    if (selected == dump)
    {
        return DensoTcuReadAction::kDump;
    }
    if (selected == relearn)
    {
        return DensoTcuReadAction::kRelearn;
    }
    if (selected == readParameters)
    {
        return DensoTcuReadAction::kReadParameters;
    }
    if (selected == setParameters)
    {
        return DensoTcuReadAction::kSetParameters;
    }
    return DensoTcuReadAction::kCancelled;
}

bool runDensoTcuServiceAction(DensoTcuReadAction action, SerialPortActions *serial, std::string protocol,
                              QWidget *parent)
{
    if (action == DensoTcuReadAction::kDump)
    {
        return false;
    }
    if (action == DensoTcuReadAction::kCancelled)
    {
        return true;
    }
    if (!confirmTcuIgnition(parent))
    {
        return true;
    }

    const std::optional<ServiceFunctionKind> serviceKind = toServiceKind(action);
    if (!serviceKind.has_value())
    {
        return true;
    }
    ServiceFunctionDialog dialog{serial, std::move(protocol), *serviceKind, parent};
    dialog.exec();
    return true;
}

} // namespace fastecu::service_functions
