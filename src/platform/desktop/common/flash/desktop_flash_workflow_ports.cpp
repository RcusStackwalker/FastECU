#include "src/platform/desktop/common/flash/desktop_flash_workflow_ports.h"

#include <memory>

#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/transport/desktop_flash_transport_source.h"

namespace fastecu::flash
{

FlashWorkflowPorts DesktopFlashWorkflowPorts(SerialPortActions *serial)
{
    return {.files = std::make_unique<QtFileRepository>(),
            .transports = std::make_unique<DesktopFlashTransportSource>(serial)};
}

} // namespace fastecu::flash
