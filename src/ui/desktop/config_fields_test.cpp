#include "src/ui/desktop/config_fields.h"

#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"

using fastecu::config::ProtocolEntry;
using fastecu::config::testing::ConfigSessionFixture;

TEST(ConfigFields, ResolvedFieldsAndCapabilities)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    EXPECT_EQ(fastecu::ui::protocol_field(f.session.vehicles()[0], &ProtocolEntry::mcu), QString("SH7058"));
    EXPECT_TRUE(fastecu::ui::protocol_capability(f.session.vehicles()[0], &ProtocolEntry::read));
    EXPECT_FALSE(fastecu::ui::protocol_capability(f.session.vehicles()[0], &ProtocolEntry::test_write));
}

TEST(ConfigFields, UnresolvedRowShowsThePlaceholderAndNoCapability)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    const auto& unresolved = f.session.vehicles()[3];
    EXPECT_EQ(fastecu::ui::protocol_field(unresolved, &ProtocolEntry::description), QString(" "));
    EXPECT_FALSE(fastecu::ui::protocol_capability(unresolved, &ProtocolEntry::read));
    EXPECT_FALSE(fastecu::ui::protocol_capability(unresolved, &ProtocolEntry::write));
}

TEST(ConfigFields, ListConversionsRoundTrip)
{
    const std::vector<std::string> items{"a", "b"};
    EXPECT_EQ(fastecu::ui::string_vector(fastecu::ui::qstring_list(items)), items);
}
