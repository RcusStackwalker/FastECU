#pragma once

#include "src/platform/desktop/common/connection/local_adapter.h"
#include "src/platform/desktop/common/connection/socketcan_actions.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fastecu::desktop::connection
{

class SocketCanAdapterProviderTestAccess;

namespace detail
{

struct SocketCanInterfaceEntry
{
    unsigned int index;
    std::string name;
};

struct SocketCanProviderActions
{
    std::function<Result<std::vector<SocketCanInterfaceEntry>>()> interfaces;
    std::function<Result<unsigned short>(std::string_view)> hardware_type;
    std::function<Result<unsigned int>(std::string_view)> flags;
    std::function<Result<std::uint32_t>(unsigned int)> bitrate;
    std::function<int(int, int, int)> socket;
    std::function<Result<unsigned int>(int, std::string_view)> interface_index;
    std::function<Status(int, std::uint32_t, std::uint32_t)> set_filter;
    std::function<Status(int, unsigned int)> bind;
    SocketCanActions transport;
};

} // namespace detail

class SocketCanAdapterProvider final : public ILocalAdapterProvider
{
  public:
    SocketCanAdapterProvider();

    dashboard::AdapterKind kind() const override;
    Result<std::vector<LocalAdapterDescriptor>> discover() override;
    Result<std::unique_ptr<OpenedCanAdapter>> open(std::string_view candidate_id,
                                                   const dashboard::CdbgConnectionProfile& profile) override;

  private:
    using InterfaceEntry = detail::SocketCanInterfaceEntry;
    using Dependencies = detail::SocketCanProviderActions;

    struct TestingTag
    {
    };

    SocketCanAdapterProvider(Dependencies dependencies, TestingTag);

    Dependencies dependencies_;
    std::unordered_map<std::string, InterfaceEntry> issued_interfaces_;

    friend class SocketCanAdapterProviderTestAccess;
};

} // namespace fastecu::desktop::connection
