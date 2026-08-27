#include "src/platform/desktop/common/connection/socketcan_adapter_provider.h"

#include "src/platform/desktop/common/connection/socketcan_transport.h"

#include <linux/can.h>
#include <linux/can/netlink.h>
#include <linux/can/raw.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace fastecu::desktop::connection
{
namespace
{

constexpr std::string_view kCandidatePrefix = "socketcan:";
constexpr unsigned short kNetlinkAttributeTypeMask = 0x3FFF;

Error platform_error(std::string operation, int error_number)
{
    switch (error_number)
    {
    case EBADF:
    case ENETDOWN:
    case ENETRESET:
    case ENETUNREACH:
    case ENODEV:
    case ENXIO:
        return {ErrorKind::Disconnected, std::move(operation) + " failed: interface disconnected"};
    default:
        return {ErrorKind::Internal, std::move(operation) + " failed"};
    }
}

class NativeDescriptor
{
  public:
    explicit NativeDescriptor(int fd) : fd_(fd)
    {
    }

    ~NativeDescriptor()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    NativeDescriptor(const NativeDescriptor&) = delete;
    NativeDescriptor& operator=(const NativeDescriptor&) = delete;

    int get() const
    {
        return fd_;
    }

  private:
    int fd_;
};

class InjectedDescriptor
{
  public:
    InjectedDescriptor(int fd, const detail::SocketCanActions& actions) : fd_(fd), actions_(&actions)
    {
    }

    ~InjectedDescriptor()
    {
        if (fd_ >= 0 && actions_->close)
        {
            actions_->close(fd_);
        }
    }

    InjectedDescriptor(const InjectedDescriptor&) = delete;
    InjectedDescriptor& operator=(const InjectedDescriptor&) = delete;

    int release()
    {
        return std::exchange(fd_, -1);
    }

  private:
    int fd_;
    const detail::SocketCanActions *actions_;
};

class OpenedSocketCanAdapter final : public OpenedCanAdapter
{
  public:
    OpenedSocketCanAdapter(int fd, dashboard::CanIdentifierWidth width, detail::SocketCanActions actions)
        : fd_(fd), width_(width), actions_(std::move(actions))
    {
    }

    ~OpenedSocketCanAdapter() override
    {
        if (fd_ >= 0 && actions_.close)
        {
            actions_.close(fd_);
        }
    }

    std::unique_ptr<cdbg::ICanTransport> into_transport() && override
    {
        auto transport = std::make_unique<SocketCanTransport>(fd_, width_, actions_);
        fd_ = -1;
        return transport;
    }

  private:
    int fd_;
    dashboard::CanIdentifierWidth width_;
    detail::SocketCanActions actions_;
};

Result<ifreq> interface_request(std::string_view name, unsigned long request)
{
    if (name.empty() || name.size() >= IFNAMSIZ)
    {
        return fail(ErrorKind::InvalidConfig, "SocketCAN interface name is invalid");
    }

    const int control_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (control_fd < 0)
    {
        return std::unexpected(platform_error("SocketCAN control socket", errno));
    }
    NativeDescriptor descriptor(control_fd);

    ifreq interface{};
    std::memcpy(interface.ifr_name, name.data(), name.size());
    if (::ioctl(control_fd, request, &interface) < 0)
    {
        return std::unexpected(platform_error("SocketCAN interface query", errno));
    }
    return interface;
}

Result<unsigned int> interface_index(int fd, std::string_view name)
{
    if (name.empty() || name.size() >= IFNAMSIZ)
    {
        return fail(ErrorKind::InvalidConfig, "SocketCAN interface name is invalid");
    }

    ifreq interface{};
    std::memcpy(interface.ifr_name, name.data(), name.size());
    if (::ioctl(fd, SIOCGIFINDEX, &interface) < 0)
    {
        return std::unexpected(platform_error("SocketCAN interface-index query", errno));
    }
    return static_cast<unsigned int>(interface.ifr_ifindex);
}

std::optional<std::uint32_t> can_bitrate_from_link_message(const nlmsghdr& message, unsigned int interface_index)
{
    if (message.nlmsg_type != RTM_NEWLINK)
    {
        return std::nullopt;
    }

    const auto *interface = static_cast<const ifinfomsg *>(NLMSG_DATA(&message));
    if (interface->ifi_index != static_cast<int>(interface_index))
    {
        return std::nullopt;
    }

    int link_attributes_length = IFLA_PAYLOAD(&message);
    for (const rtattr *link_attribute = IFLA_RTA(interface); RTA_OK(link_attribute, link_attributes_length);
         link_attribute = RTA_NEXT(link_attribute, link_attributes_length))
    {
        if ((link_attribute->rta_type & kNetlinkAttributeTypeMask) != IFLA_LINKINFO)
        {
            continue;
        }

        int info_attributes_length = RTA_PAYLOAD(link_attribute);
        for (const rtattr *info_attribute = static_cast<const rtattr *>(RTA_DATA(link_attribute));
             RTA_OK(info_attribute, info_attributes_length);
             info_attribute = RTA_NEXT(info_attribute, info_attributes_length))
        {
            if ((info_attribute->rta_type & kNetlinkAttributeTypeMask) != IFLA_INFO_DATA)
            {
                continue;
            }

            int can_attributes_length = RTA_PAYLOAD(info_attribute);
            for (const rtattr *can_attribute = static_cast<const rtattr *>(RTA_DATA(info_attribute));
                 RTA_OK(can_attribute, can_attributes_length);
                 can_attribute = RTA_NEXT(can_attribute, can_attributes_length))
            {
                if ((can_attribute->rta_type & kNetlinkAttributeTypeMask) != IFLA_CAN_BITTIMING ||
                    RTA_PAYLOAD(can_attribute) < static_cast<int>(sizeof(can_bittiming)))
                {
                    continue;
                }

                can_bittiming timing{};
                std::memcpy(&timing, RTA_DATA(can_attribute), sizeof(timing));
                return timing.bitrate;
            }
        }
    }
    return std::nullopt;
}

Result<std::uint32_t> query_can_bitrate(unsigned int interface_index_value)
{
    const int netlink_fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (netlink_fd < 0)
    {
        return std::unexpected(platform_error("SocketCAN bitrate netlink socket", errno));
    }
    NativeDescriptor descriptor(netlink_fd);

    sockaddr_nl local{.nl_family = AF_NETLINK};
    if (::bind(netlink_fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) < 0)
    {
        return std::unexpected(platform_error("SocketCAN bitrate netlink bind", errno));
    }

    struct LinkRequest
    {
        nlmsghdr header;
        ifinfomsg interface;
    } request{};
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    request.header.nlmsg_type = RTM_GETLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST;
    request.header.nlmsg_seq = 1;
    request.interface.ifi_family = AF_UNSPEC;
    request.interface.ifi_index = static_cast<int>(interface_index_value);

    sockaddr_nl kernel{.nl_family = AF_NETLINK};
    if (::sendto(netlink_fd, &request, request.header.nlmsg_len, 0, reinterpret_cast<const sockaddr *>(&kernel),
                 sizeof(kernel)) < 0)
    {
        return std::unexpected(platform_error("SocketCAN bitrate netlink request", errno));
    }

    alignas(nlmsghdr) std::array<std::byte, 8192> response{};
    const std::ptrdiff_t received = ::recv(netlink_fd, response.data(), response.size(), 0);
    if (received < 0)
    {
        return std::unexpected(platform_error("SocketCAN bitrate netlink response", errno));
    }
    if (received == 0)
    {
        return fail(ErrorKind::Disconnected, "SocketCAN bitrate query returned no response");
    }

    int remaining = static_cast<int>(received);
    for (const nlmsghdr *message = reinterpret_cast<const nlmsghdr *>(response.data()); NLMSG_OK(message, remaining);
         message = NLMSG_NEXT(message, remaining))
    {
        if (message->nlmsg_type == NLMSG_ERROR)
        {
            const auto *error = static_cast<const nlmsgerr *>(NLMSG_DATA(message));
            if (error->error != 0)
            {
                return std::unexpected(platform_error("SocketCAN bitrate query", -error->error));
            }
            continue;
        }
        if (const auto bitrate = can_bitrate_from_link_message(*message, interface_index_value))
        {
            return *bitrate;
        }
    }

    return fail(ErrorKind::Unsupported, "kernel did not report the SocketCAN bitrate");
}

detail::SocketCanProviderActions production_dependencies()
{
    return {
        .interfaces = []() -> Result<std::vector<detail::SocketCanInterfaceEntry>>
        {
            struct if_nameindex *raw_interfaces = ::if_nameindex();
            if (raw_interfaces == nullptr)
            {
                return std::unexpected(platform_error("SocketCAN interface enumeration", errno));
            }
            std::unique_ptr<struct if_nameindex, decltype(&::if_freenameindex)> interfaces(raw_interfaces,
                                                                                           &::if_freenameindex);

            std::vector<detail::SocketCanInterfaceEntry> result;
            for (const struct if_nameindex *interface = interfaces.get();
                 interface->if_index != 0 || interface->if_name != nullptr; ++interface)
            {
                if (interface->if_name != nullptr)
                {
                    result.push_back({interface->if_index, interface->if_name});
                }
            }
            return result;
        },
        .hardware_type = [](std::string_view name) -> Result<unsigned short>
        {
            const auto interface = interface_request(name, SIOCGIFHWADDR);
            if (!interface.has_value())
            {
                return std::unexpected(interface.error());
            }
            return static_cast<unsigned short>(interface->ifr_hwaddr.sa_family);
        },
        .flags = [](std::string_view name) -> Result<unsigned int>
        {
            const auto interface = interface_request(name, SIOCGIFFLAGS);
            if (!interface.has_value())
            {
                return std::unexpected(interface.error());
            }
            return static_cast<unsigned int>(interface->ifr_flags);
        },
        .bitrate = [](unsigned int index) { return query_can_bitrate(index); },
        .socket = [](int domain, int type, int protocol) { return ::socket(domain, type, protocol); },
        .interface_index = [](int fd, std::string_view name) { return interface_index(fd, name); },
        .set_filter = [](int fd, std::uint32_t id, std::uint32_t mask) -> Status
        {
            const can_filter filter{.can_id = id, .can_mask = mask};
            if (::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)) < 0)
            {
                return std::unexpected(platform_error("SocketCAN receive-filter setup", errno));
            }
            return {};
        },
        .bind = [](int fd, unsigned int index) -> Status
        {
            const sockaddr_can address{.can_family = AF_CAN, .can_ifindex = static_cast<int>(index)};
            if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
            {
                return std::unexpected(platform_error("SocketCAN interface bind", errno));
            }
            return {};
        },
        .transport = detail::production_socketcan_actions(),
    };
}

Result<std::pair<std::uint32_t, std::uint32_t>> receive_filter(const dashboard::CdbgConnectionProfile& profile)
{
    switch (profile.identifier_width)
    {
    case dashboard::CanIdentifierWidth::Standard:
        if (profile.reply_id > CAN_SFF_MASK)
        {
            return fail(ErrorKind::InvalidConfig, "standard SocketCAN reply identifier exceeds 11 bits");
        }
        return std::pair{profile.reply_id, std::uint32_t{CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG}};
    case dashboard::CanIdentifierWidth::Extended:
        if (profile.reply_id > CAN_EFF_MASK)
        {
            return fail(ErrorKind::InvalidConfig, "extended SocketCAN reply identifier exceeds 29 bits");
        }
        return std::pair{profile.reply_id | CAN_EFF_FLAG, std::uint32_t{CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG}};
    }
    return fail(ErrorKind::InvalidConfig, "unknown SocketCAN identifier width");
}

} // namespace

SocketCanAdapterProvider::SocketCanAdapterProvider() : SocketCanAdapterProvider(production_dependencies(), TestingTag{})
{
}

SocketCanAdapterProvider::SocketCanAdapterProvider(Dependencies dependencies, TestingTag)
    : dependencies_(std::move(dependencies))
{
}

dashboard::AdapterKind SocketCanAdapterProvider::kind() const
{
    return dashboard::AdapterKind::SocketCan;
}

Result<std::vector<LocalAdapterDescriptor>> SocketCanAdapterProvider::discover()
{
    try
    {
        if (!dependencies_.interfaces || !dependencies_.hardware_type)
        {
            return fail(ErrorKind::Internal, "SocketCAN discovery actions are unavailable");
        }
        const auto interfaces = dependencies_.interfaces();
        if (!interfaces.has_value())
        {
            return std::unexpected(interfaces.error());
        }

        std::vector<LocalAdapterDescriptor> candidates;
        std::unordered_map<std::string, InterfaceEntry> issued;
        for (const InterfaceEntry& interface : *interfaces)
        {
            const auto hardware_type = dependencies_.hardware_type(interface.name);
            if (!hardware_type.has_value())
            {
                return std::unexpected(hardware_type.error());
            }
            if (*hardware_type != ARPHRD_CAN)
            {
                continue;
            }

            const std::string candidate_id = std::string(kCandidatePrefix) + interface.name;
            if (!issued.emplace(candidate_id, interface).second)
            {
                continue;
            }
            candidates.push_back({
                .candidate_id = candidate_id,
                .kind = dashboard::AdapterKind::SocketCan,
                .vendor = "Linux",
                .display_name = interface.name,
                .label = "Linux " + interface.name,
            });
        }

        issued_interfaces_ = std::move(issued);
        return candidates;
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "SocketCAN discovery threw an unknown exception");
    }
}

Result<std::unique_ptr<OpenedCanAdapter>>
SocketCanAdapterProvider::open(std::string_view candidate_id, const dashboard::CdbgConnectionProfile& profile)
{
    const auto selected = issued_interfaces_.find(std::string(candidate_id));
    if (selected == issued_interfaces_.end())
    {
        return fail(ErrorKind::InvalidConfig, "SocketCAN candidate was not issued by this provider");
    }

    try
    {
        if (!dependencies_.flags || !dependencies_.bitrate || !dependencies_.socket || !dependencies_.interface_index ||
            !dependencies_.set_filter || !dependencies_.bind || !dependencies_.transport.close)
        {
            return fail(ErrorKind::Internal, "SocketCAN open actions are unavailable");
        }

        const auto flags = dependencies_.flags(selected->second.name);
        if (!flags.has_value())
        {
            return std::unexpected(flags.error());
        }
        if ((*flags & IFF_UP) == 0)
        {
            return fail(ErrorKind::Disconnected, "SocketCAN interface is down");
        }

        const auto filter = receive_filter(profile);
        if (!filter.has_value())
        {
            return std::unexpected(filter.error());
        }

        const int fd = dependencies_.socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
        if (fd < 0)
        {
            return std::unexpected(platform_error("SocketCAN raw socket", errno));
        }
        InjectedDescriptor descriptor(fd, dependencies_.transport);

        const auto resolved_index = dependencies_.interface_index(fd, selected->second.name);
        if (!resolved_index.has_value())
        {
            return std::unexpected(resolved_index.error());
        }
        if (*resolved_index != selected->second.index)
        {
            return fail(ErrorKind::InvalidConfig, "SocketCAN candidate changed since discovery");
        }

        const auto bitrate = dependencies_.bitrate(*resolved_index);
        if (!bitrate.has_value())
        {
            return std::unexpected(bitrate.error());
        }
        if (*bitrate != profile.bitrate)
        {
            return fail(ErrorKind::Unsupported, "SocketCAN interface bitrate does not match the dashboard profile");
        }

        if (const Status filtered = dependencies_.set_filter(fd, filter->first, filter->second); !filtered.has_value())
        {
            return std::unexpected(filtered.error());
        }
        if (const Status bound = dependencies_.bind(fd, *resolved_index); !bound.has_value())
        {
            return std::unexpected(bound.error());
        }

        std::unique_ptr<OpenedCanAdapter> opened =
            std::make_unique<OpenedSocketCanAdapter>(fd, profile.identifier_width, dependencies_.transport);
        descriptor.release();
        return opened;
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "SocketCAN open threw an unknown exception");
    }
}

} // namespace fastecu::desktop::connection
