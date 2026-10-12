#pragma once

#include "src/platform/desktop/common/flash/flash_workflow.h"

class SerialPortActions;

namespace fastecu::flash
{

// The desktop workflow ports: kernel files through QtFileRepository, and
// transports over `serial`, which must outlive every attempt the workflow
// hands out. A null serial is accepted (see DesktopFlashTransportSource).
FlashWorkflowPorts DesktopFlashWorkflowPorts(SerialPortActions *serial);

} // namespace fastecu::flash
