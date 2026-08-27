#include "src/platform/desktop/common/connection/socketcan_adapter_provider.h"

#include <gtest/gtest.h>

#include <linux/can.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/socket.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fastecu::desktop::connection
{

class SocketCanProviderHarness;

class SocketCanAdapterProviderTestAccess
{
  public:
    static std::unique_ptr<SocketCanAdapterProvider> make(SocketCanProviderHarness& harness);
};

class SocketCanProviderHarness
{
  public:
    struct SocketCall
    {
        int domain;
        int type;
        int protocol;
    };

    struct FilterCall
    {
        int fd;
        std::uint32_t id;
        std::uint32_t mask;
    };

    std::vector<detail::SocketCanInterfaceEntry> interfaces{
        {1, "lo"},
        {2, "eth0"},
        {7, "can0"},
    };
    std::unordered_map<std::string, unsigned short> hardware_types{
        {"lo", ARPHRD_LOOPBACK},
        {"eth0", ARPHRD_ETHER},
        {"can0", ARPHRD_CAN},
    };
    std::unordered_map<std::string, unsigned int> interface_flags{{"can0", IFF_UP}};
    Result<std::uint32_t> bitrate{500000U};
    Result<unsigned int> resolved_index{7U};
    int next_fd{41};
    std::optional<SocketCall> socket_call;
    std::optional<std::pair<int, std::string>> index_call;
    std::optional<FilterCall> filter_call;
    std::optional<std::pair<int, unsigned int>> bind_call;
    int socket_calls{0};
    int bitrate_calls{0};
    std::optional<unsigned int> bitrate_index;
    int close_calls{0};
    int closed_fd{-1};
};

std::unique_ptr<SocketCanAdapterProvider> SocketCanAdapterProviderTestAccess::make(SocketCanProviderHarness& harness)
{
    SocketCanAdapterProvider::Dependencies dependencies{
        .interfaces = [&harness]() -> Result<std::vector<detail::SocketCanInterfaceEntry>>
        { return harness.interfaces; },
        .hardware_type = [&harness](std::string_view name) -> Result<unsigned short>
        {
            const auto found = harness.hardware_types.find(std::string(name));
            if (found == harness.hardware_types.end())
            {
                return fail(ErrorKind::Disconnected, "interface disappeared");
            }
            return found->second;
        },
        .flags = [&harness](std::string_view name) -> Result<unsigned int>
        {
            const auto found = harness.interface_flags.find(std::string(name));
            if (found == harness.interface_flags.end())
            {
                return 0U;
            }
            return found->second;
        },
        .bitrate =
            [&harness](unsigned int index)
        {
            ++harness.bitrate_calls;
            harness.bitrate_index = index;
            return harness.bitrate;
        },
        .socket =
            [&harness](int domain, int type, int protocol)
        {
            ++harness.socket_calls;
            harness.socket_call = SocketCanProviderHarness::SocketCall{domain, type, protocol};
            return harness.next_fd;
        },
        .interface_index = [&harness](int fd, std::string_view name) -> Result<unsigned int>
        {
            harness.index_call = std::pair{fd, std::string(name)};
            return harness.resolved_index;
        },
        .set_filter = [&harness](int fd, std::uint32_t id, std::uint32_t mask) -> Status
        {
            harness.filter_call = SocketCanProviderHarness::FilterCall{fd, id, mask};
            return {};
        },
        .bind = [&harness](int fd, unsigned int index) -> Status
        {
            harness.bind_call = std::pair{fd, index};
            return {};
        },
        .transport =
            {
                .send = [](int, const void *, std::size_t size, int) { return static_cast<std::ptrdiff_t>(size); },
                .poll = [](int, short, int, short&) { return 0; },
                .recv = [](int, void *, std::size_t, int) { return std::ptrdiff_t{-1}; },
                .close =
                    [&harness](int fd)
                {
                    ++harness.close_calls;
                    harness.closed_fd = fd;
                    return 0;
                },
            },
    };
    return std::unique_ptr<SocketCanAdapterProvider>(
        new SocketCanAdapterProvider(std::move(dependencies), SocketCanAdapterProvider::TestingTag{}));
}

namespace
{

dashboard::CdbgConnectionProfile profile(dashboard::CanIdentifierWidth width = dashboard::CanIdentifierWidth::Standard,
                                         std::uint32_t reply_id = 0x456)
{
    return {
        .protocol = dashboard::DashboardProtocol::Cdbg,
        .transport = dashboard::DashboardTransport::RawCan,
        .bitrate = 500000,
        .identifier_width = width,
        .request_id = 0x123,
        .reply_id = reply_id,
        .stream_instance = 1,
        .sampling_interval_ms = 100,
        .retry = {.poll_timeout_ms = 100, .silence_threshold = 3, .reconnect_attempts = 2, .reconnect_period_ms = 250},
        .preferred_adapter = std::nullopt,
    };
}

std::string discover_can0(SocketCanAdapterProvider& provider)
{
    const auto discovered = provider.discover();
    EXPECT_TRUE(discovered.has_value());
    if (!discovered.has_value() || discovered->empty())
    {
        return {};
    }
    return discovered->front().candidate_id;
}

TEST(SocketCanAdapterProvider, DiscoveryIncludesOnlyCanHardwareWithStablePresentation)
{
    SocketCanProviderHarness harness;
    harness.interfaces.push_back({8, "vcan0"});
    harness.hardware_types.emplace("vcan0", ARPHRD_CAN);
    harness.interface_flags.emplace("vcan0", IFF_UP);
    auto provider = SocketCanAdapterProviderTestAccess::make(harness);

    const auto result = provider->discover();

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ(provider->kind(), dashboard::AdapterKind::SocketCan);
    EXPECT_EQ((*result)[0].kind, dashboard::AdapterKind::SocketCan);
    EXPECT_EQ((*result)[0].vendor, "Linux");
    EXPECT_EQ((*result)[0].display_name, "can0");
    EXPECT_EQ((*result)[0].label, "Linux can0");
    EXPECT_EQ((*result)[1].display_name, "vcan0");
}

TEST(SocketCanAdapterProvider, OpenRejectsCandidateNotIssuedByDiscovery)
{
    SocketCanProviderHarness harness;
    auto provider = SocketCanAdapterProviderTestAccess::make(harness);
    ASSERT_TRUE(provider->discover().has_value());

    const auto result = provider->open("socketcan:stale", profile());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(harness.socket_calls, 0);
}

TEST(SocketCanAdapterProvider, OpenRejectsInterfaceWhoseIndexChangedSinceDiscovery)
{
    SocketCanProviderHarness harness;
    auto provider = SocketCanAdapterProviderTestAccess::make(harness);
    const std::string candidate_id = discover_can0(*provider);
    harness.resolved_index = 8U;

    const auto result = provider->open(candidate_id, profile());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(harness.index_call, (std::pair{41, std::string("can0")}));
    EXPECT_EQ(harness.bitrate_calls, 0);
    EXPECT_FALSE(harness.filter_call.has_value());
    EXPECT_FALSE(harness.bind_call.has_value());
    EXPECT_EQ(harness.close_calls, 1);
    EXPECT_EQ(harness.closed_fd, 41);
}

TEST(SocketCanAdapterProvider, OpenCreatesNonblockingRawSocketFiltersAndBindsSelectedInterface)
{
    SocketCanProviderHarness harness;
    auto provider = SocketCanAdapterProviderTestAccess::make(harness);
    const std::string candidate_id = discover_can0(*provider);

    auto result = provider->open(candidate_id, profile());

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(harness.socket_call.has_value());
    EXPECT_EQ(harness.socket_call->domain, PF_CAN);
    EXPECT_EQ(harness.socket_call->type & SOCK_RAW, SOCK_RAW);
    EXPECT_EQ(harness.socket_call->type & SOCK_NONBLOCK, SOCK_NONBLOCK);
    EXPECT_EQ(harness.socket_call->protocol, CAN_RAW);
    EXPECT_EQ(harness.index_call, (std::pair{41, std::string("can0")}));
    ASSERT_TRUE(harness.filter_call.has_value());
    EXPECT_EQ(harness.filter_call->fd, 41);
    EXPECT_EQ(harness.filter_call->id, 0x456U);
    EXPECT_EQ(harness.filter_call->mask, (CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG));
    EXPECT_EQ(harness.bind_call, (std::pair{41, 7U}));
    EXPECT_EQ(harness.close_calls, 0);

    std::unique_ptr<cdbg::ICanTransport> transport = std::move(**result).into_transport();
    ASSERT_NE(transport, nullptr);
    EXPECT_TRUE(transport->isOpen());
    EXPECT_EQ(harness.close_calls, 0);
    transport.reset();
    EXPECT_EQ(harness.close_calls, 1);
    EXPECT_EQ(harness.closed_fd, 41);
}

TEST(SocketCanAdapterProvider, ExtendedReplyFilterRequiresAnExactExtendedDataIdentifier)
{
    SocketCanProviderHarness harness;
    auto provider = SocketCanAdapterProviderTestAccess::make(harness);
    const std::string candidate_id = discover_can0(*provider);

    const auto result = provider->open(candidate_id, profile(dashboard::CanIdentifierWidth::Extended, 0x18DAF110));

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(harness.filter_call.has_value());
    EXPECT_EQ(harness.filter_call->id, (0x18DAF110U | CAN_EFF_FLAG));
    EXPECT_EQ(harness.filter_call->mask, (CAN_EFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG));
}

TEST(SocketCanAdapterProvider, DownInterfaceIsDisconnectedBeforeOpeningASocket)
{
    SocketCanProviderHarness harness;
    harness.interface_flags["can0"] = 0;
    auto provider = SocketCanAdapterProviderTestAccess::make(harness);
    const std::string candidate_id = discover_can0(*provider);

    const auto result = provider->open(candidate_id, profile());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(harness.socket_calls, 0);
}

TEST(SocketCanAdapterProvider, BitrateMismatchIsUnsupportedWithoutFilteringBindingOrReconfiguring)
{
    SocketCanProviderHarness harness;
    harness.bitrate = 250000U;
    auto provider = SocketCanAdapterProviderTestAccess::make(harness);
    const std::string candidate_id = discover_can0(*provider);

    const auto result = provider->open(candidate_id, profile());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_EQ(harness.socket_calls, 1);
    EXPECT_EQ(harness.bitrate_index, 7U);
    EXPECT_FALSE(harness.filter_call.has_value());
    EXPECT_FALSE(harness.bind_call.has_value());
    EXPECT_EQ(harness.close_calls, 1);
}

} // namespace
} // namespace fastecu::desktop::connection
