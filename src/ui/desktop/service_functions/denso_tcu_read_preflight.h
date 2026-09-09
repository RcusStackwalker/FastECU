#pragma once

#include <string>

class SerialPortActions;
class QWidget;

namespace fastecu::service_functions
{

enum class DensoTcuReadAction
{
    Dump,
    Relearn,
    ReadParameters,
    SetParameters,
    Cancelled,
};

DensoTcuReadAction choose_denso_tcu_read_action(QWidget *parent);

// Returns false only when the caller must continue into the ROM-dump flash
// workflow. Every other result is fully handled here and stops flash routing.
bool run_denso_tcu_service_action(DensoTcuReadAction action, SerialPortActions *serial, std::string protocol,
                                  QWidget *parent);

} // namespace fastecu::service_functions
