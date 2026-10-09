#include "src/ui/desktop/flash/operation/flash_operation_controller.h"

#include <QMessageBox>

#include <string>
#include <utility>

#include "src/backend/flash/flash_operation_request.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"
#include "src/ui/desktop/flash/common/flash_dialog.h"
#include "src/ui/desktop/service_functions/denso_tcu_read_preflight.h"

namespace fastecu::flash
{

FlashOperationController::FlashOperationController(SerialPortActions& serial, QWidget *dialogParent)
    : serial_(serial), dialog_parent_(dialogParent)
{
}

FlashOperationOutcome FlashOperationController::run(const FlashOperationInput& input)
{
    if (input.operation == FlashOperation::kRead && IsDensoTcuProtocol(input.protocol.name))
    {
        using fastecu::service_functions::DensoTcuReadAction;
        const DensoTcuReadAction action = fastecu::service_functions::chooseDensoTcuReadAction(dialog_parent_);
        switch (action)
        {
        case DensoTcuReadAction::kDump:
            emit logI(
                "Read memory with flashmethod '" +
                    QString::fromUtf8(input.protocol.name.data(), static_cast<qsizetype>(input.protocol.name.size())) +
                    "' and kernel '" + QString::fromStdString(input.kernel_path) + "'",
                true, true);
            break;
        case DensoTcuReadAction::kRelearn:
            emit logI("Attempting TCU relearn", true, true);
            break;
        case DensoTcuReadAction::kReadParameters:
            emit logI("Attempting to read TCU parameters", true, true);
            break;
        case DensoTcuReadAction::kSetParameters:
            emit logI("Attempting to set TCU parameters", true, true);
            break;
        case DensoTcuReadAction::kCancelled:
            emit logI("No option selected", true, true);
            break;
        }
        if (fastecu::service_functions::runDensoTcuServiceAction(action, &serial_, std::string(input.protocol.name),
                                                                 dialog_parent_))
        {
            return {.status = FlashOperationStatus::kServiceActionHandled};
        }
    }

    auto workflow = FlashWorkflowFactory::TryCreate({
        .operation = input.operation,
        .protocol = input.protocol,
        .image = input.image,
        .paths = input.paths,
        .display_filename = input.display_filename,
        .serial = &serial_,
    });
    if (!workflow)
    {
        QMessageBox::warning(
            dialog_parent_, tr("Unknown flashmethod"),
            "Unknown flashmethod! Flashmethod \"" +
                QString::fromUtf8(input.protocol.name.data(), static_cast<qsizetype>(input.protocol.name.size())) +
                "\" not yet implemented!");
        return {.status = FlashOperationStatus::kUnsupported};
    }

    FlashDialog flashModule(std::move(workflow), input.operation, QString::fromStdString(input.display_filename),
                            dialog_parent_);
    QObject::connect<void (FlashDialog::*)(QString)>(&flashModule, &FlashDialog::externalLogger, this,
                                                     qOverload<QString>(&FlashOperationController::externalLogger));
    QObject::connect<void (FlashDialog::*)(int)>(&flashModule, &FlashDialog::externalLogger, this,
                                                 qOverload<int>(&FlashOperationController::externalLogger));
    QObject::connect(&flashModule, &FlashDialog::logE, this, &FlashOperationController::logE);
    QObject::connect(&flashModule, &FlashDialog::logW, this, &FlashOperationController::logW);
    QObject::connect(&flashModule, &FlashDialog::logI, this, &FlashOperationController::logI);
    QObject::connect(&flashModule, &FlashDialog::logD, this, &FlashOperationController::logD);

    FlashDialogResult result = flashModule.run();
    return {
        .status = FlashOperationStatus::kCompleted,
        .read_bytes = std::move(result.accepted_read_bytes),
        .rom_id = std::move(result.rom_id),
    };
}

} // namespace fastecu::flash
