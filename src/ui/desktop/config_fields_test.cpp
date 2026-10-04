#include "src/ui/desktop/config_fields.h"

#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"

using fastecu::config::ProtocolSpec;
using fastecu::config::testing::ConfigSessionFixture;

TEST(ConfigFields, FieldsAndCapabilities)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.initialize().has_value());
    const auto& impreza = f.session.vehicles()[0];
    EXPECT_EQ(fastecu::ui::protocol_field(impreza, &ProtocolSpec::mcu), QString("SH7058"));
    EXPECT_TRUE(fastecu::ui::protocol_capability(impreza, &ProtocolSpec::read));
    EXPECT_FALSE(fastecu::ui::protocol_capability(impreza, &ProtocolSpec::test_write));
    EXPECT_EQ(fastecu::ui::protocol_flag(impreza, &ProtocolSpec::write), QString("yes"));
    EXPECT_EQ(fastecu::ui::protocol_flag(impreza, &ProtocolSpec::test_write), QString("no"));
    EXPECT_EQ(fastecu::ui::checksum_field(impreza), QString("yes"));
    EXPECT_EQ(fastecu::ui::checksum_field(f.session.vehicles()[1]), QString("n/a"));
    EXPECT_EQ(fastecu::ui::kernel_address_field(impreza), QString("0xFFFF3000"));
}

TEST(ConfigFields, ListConversionsRoundTrip)
{
    const std::vector<std::string> items{"a", "b"};
    EXPECT_EQ(fastecu::ui::string_vector(fastecu::ui::qstring_list(items)), items);
}
