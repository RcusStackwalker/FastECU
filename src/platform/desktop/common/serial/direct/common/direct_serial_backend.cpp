#include "src/platform/desktop/common/serial/direct_serial_backend.h"

#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

std::unique_ptr<SerialBackend> MakeDirectSerialBackend()
{
    return std::make_unique<SerialPortActionsDirect>();
}
