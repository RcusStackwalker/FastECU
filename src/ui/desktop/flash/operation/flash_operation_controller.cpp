#include "src/ui/desktop/flash/operation/flash_operation_controller.h"

#include <QMessageBox>

#include <utility>

#include "src/backend/flash/flash_operation_request.h"
#include "src/platform/desktop/common/flash/flash_workflow.h"
#include "src/ui/desktop/flash/common/flash_dialog.h"
#include "src/ui/desktop/service_functions/denso_tcu_read_preflight.h"

namespace fastecu::flash
{

FlashOperationController::FlashOperationController(SerialPortActions& serial, QWidget *dialog_parent)
    : serial_(serial), dialog_parent_(dialog_parent)
{
}

FlashOperationOutcome FlashOperationController::run(const FlashOperationInput& input)
{
    if (input.operation == FlashOperation::Read && is_denso_tcu_protocol(input.protocol))
    {
        using fastecu::service_functions::DensoTcuReadAction;
        const DensoTcuReadAction action = fastecu::service_functions::choose_denso_tcu_read_action(dialog_parent_);
        switch (action)
        {
        case DensoTcuReadAction::Dump:
            emit LOG_I("Read memory with flashmethod '" + QString::fromStdString(input.protocol) + "' and kernel '" +
                           QString::fromStdString(input.kernel_path) + "'",
                       true, true);
            break;
        case DensoTcuReadAction::Relearn:
            emit LOG_I("Attempting TCU relearn", true, true);
            break;
        case DensoTcuReadAction::ReadParameters:
            emit LOG_I("Attempting to read TCU parameters", true, true);
            break;
        case DensoTcuReadAction::SetParameters:
            emit LOG_I("Attempting to set TCU parameters", true, true);
            break;
        case DensoTcuReadAction::Cancelled:
            emit LOG_I("No option selected", true, true);
            break;
        }
        if (fastecu::service_functions::run_denso_tcu_service_action(action, &serial_, input.protocol, dialog_parent_))
        {
            return {.status = FlashOperationStatus::ServiceActionHandled};
        }
    }

    auto workflow = FlashWorkflowFactory::tryCreate({
        .operation = input.operation,
        .protocol = input.protocol,
        .mcu = input.mcu,
        .image = input.image,
        .paths = input.paths,
        .display_filename = input.display_filename,
        .serial = &serial_,
    });
    if (!workflow)
    {
        QMessageBox::warning(dialog_parent_, tr("Unknown flashmethod"),
                             "Unknown flashmethod! Flashmethod \"" + QString::fromStdString(input.protocol) +
                                 "\" not yet implemented!");
        return {.status = FlashOperationStatus::Unsupported};
    }

    FlashDialog flash_module(std::move(workflow), input.operation, QString::fromStdString(input.display_filename),
                             dialog_parent_);
    QObject::connect<void (FlashDialog::*)(QString)>(&flash_module, &FlashDialog::external_logger, this,
                                                     qOverload<QString>(&FlashOperationController::external_logger));
    QObject::connect<void (FlashDialog::*)(int)>(&flash_module, &FlashDialog::external_logger, this,
                                                 qOverload<int>(&FlashOperationController::external_logger));
    QObject::connect(&flash_module, &FlashDialog::LOG_E, this, &FlashOperationController::LOG_E);
    QObject::connect(&flash_module, &FlashDialog::LOG_W, this, &FlashOperationController::LOG_W);
    QObject::connect(&flash_module, &FlashDialog::LOG_I, this, &FlashOperationController::LOG_I);
    QObject::connect(&flash_module, &FlashDialog::LOG_D, this, &FlashOperationController::LOG_D);

    FlashDialogResult result = flash_module.run();
    return {
        .status = FlashOperationStatus::Completed,
        .read_bytes = std::move(result.accepted_read_bytes),
        .rom_id = std::move(result.rom_id),
    };
}

} // namespace fastecu::flash
