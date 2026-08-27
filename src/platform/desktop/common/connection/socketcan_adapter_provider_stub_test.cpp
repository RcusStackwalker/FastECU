#include "src/platform/desktop/common/connection/socketcan_adapter_provider.h"

#include <gtest/gtest.h>

namespace fastecu::desktop::connection
{
namespace
{

dashboard::CdbgConnectionProfile profile()
{
    return {
        .bitrate = 500000,
        .identifier_width = dashboard::CanIdentifierWidth::Standard,
        .request_id = 0x123,
        .reply_id = 0x456,
    };
}

TEST(SocketCanAdapterProviderStub, DiscoveryIsEmpty)
{
    SocketCanAdapterProvider provider;

    const auto result = provider.discover();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
    EXPECT_EQ(provider.kind(), dashboard::AdapterKind::SocketCan);
}

TEST(SocketCanAdapterProviderStub, OpenIsUnsupported)
{
    SocketCanAdapterProvider provider;

    const auto result = provider.open("socketcan:can0", profile());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
}

} // namespace
} // namespace fastecu::desktop::connection
