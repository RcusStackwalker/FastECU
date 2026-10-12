#include "src/platform/desktop/common/transport/desktop_flash_transport_source.h"

#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"
#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"

namespace fastecu::flash
{

DesktopFlashTransportSource::DesktopFlashTransportSource(SerialPortActions *serial) : serial_(serial)
{
}

std::unique_ptr<IKlineFlashTransport> DesktopFlashTransportSource::MakeKline()
{
    return std::make_unique<DesktopKlineFlashTransport>(serial_);
}

std::unique_ptr<ICanFlashTransport> DesktopFlashTransportSource::MakeCan()
{
    return std::make_unique<DesktopCanFlashTransport>(serial_);
}

std::unique_ptr<IMixedCanFlashTransport> DesktopFlashTransportSource::MakeMixedCan()
{
    return std::make_unique<DesktopMixedCanFlashTransport>(serial_);
}

bool DesktopFlashTransportSource::SuppliesProgrammingVoltage() const
{
    return serial_ != nullptr && serial_->GetUseOpenport2Adapter();
}

} // namespace fastecu::flash
