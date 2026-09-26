#include "src/platform/desktop/common/serial/direct_serial_backend.h"

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

std::unique_ptr<SerialBackend> make_direct_serial_backend()
{
    return std::make_unique<SerialPortActionsDirect>();
}
