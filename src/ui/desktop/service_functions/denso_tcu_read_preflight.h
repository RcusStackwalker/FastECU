#pragma once

#include <string>

class SerialPortActions;
class QWidget;

namespace fastecu::service_functions
{

enum class DensoTcuReadAction
{
    kDump,
    kRelearn,
    kReadParameters,
    kSetParameters,
    kCancelled,
};

DensoTcuReadAction chooseDensoTcuReadAction(QWidget *parent);

// Returns false only when the caller must continue into the ROM-dump flash
// workflow. Every other result is fully handled here and stops flash routing.
bool runDensoTcuServiceAction(DensoTcuReadAction action, SerialPortActions *serial, std::string protocol,
                              QWidget *parent);

} // namespace fastecu::service_functions
