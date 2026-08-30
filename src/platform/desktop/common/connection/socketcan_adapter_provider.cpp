#include "src/platform/desktop/common/connection/socketcan_adapter_provider.h"

namespace fastecu::desktop::connection
{

SocketCanAdapterProvider::SocketCanAdapterProvider() = default;

dashboard::AdapterKind SocketCanAdapterProvider::kind() const
{
    return dashboard::AdapterKind::SocketCan;
}

Result<std::vector<LocalAdapterDescriptor>> SocketCanAdapterProvider::discover()
{
    issued_interfaces_.clear();
    return std::vector<LocalAdapterDescriptor>{};
}

Result<std::unique_ptr<OpenedCanAdapter>> SocketCanAdapterProvider::open(std::string_view,
                                                                         const dashboard::CdbgConnectionProfile&)
{
    return fail(ErrorKind::Unsupported, "SocketCAN is available only on Linux");
}

} // namespace fastecu::desktop::connection
