#include "src/ui/desktop/config_fields.h"

#include <gtest/gtest.h>

#include "src/backend/config/testing/config_session_fixture.h"

using fastecu::config::ProtocolSpec;
using fastecu::config::testing::ConfigSessionFixture;

TEST(ConfigFields, FieldsAndCapabilities)
{
    ConfigSessionFixture f;
    ASSERT_TRUE(f.Initialize().has_value());
    const auto& impreza = f.session.Vehicles()[0];
    EXPECT_EQ(fastecu::ui::protocolField(impreza, &ProtocolSpec::mcu), QString("SH7058"));
    EXPECT_TRUE(fastecu::ui::protocolCapability(impreza, &ProtocolSpec::read));
    EXPECT_FALSE(fastecu::ui::protocolCapability(impreza, &ProtocolSpec::test_write));
    EXPECT_EQ(fastecu::ui::protocolFlag(impreza, &ProtocolSpec::write), QString("yes"));
    EXPECT_EQ(fastecu::ui::protocolFlag(impreza, &ProtocolSpec::test_write), QString("no"));
    EXPECT_EQ(fastecu::ui::checksumField(impreza), QString("yes"));
    EXPECT_EQ(fastecu::ui::checksumField(f.session.Vehicles()[1]), QString("n/a"));
    EXPECT_EQ(fastecu::ui::kernelAddressField(impreza), QString("0xFFFF3000"));
}

TEST(ConfigFields, ListConversionsRoundTrip)
{
    const std::vector<std::string> items{"a", "b"};
    EXPECT_EQ(fastecu::ui::stringVector(fastecu::ui::qstringList(items)), items);
}
