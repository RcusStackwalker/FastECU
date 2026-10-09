#include "src/backend/logging/logger_model.h"

#include <gtest/gtest.h>

namespace fastecu::logging
{
namespace
{
LoggerDefinition Definition()
{
    return {
        .parameters =
            {{.protocol = "SSM", .id = "rpm", .name = "Same", .ecu_byte_index = "0", .ecu_bit = "1", .enabled = true},
             {.protocol = "MUT_DMA", .id = "rpm", .name = "Same", .enabled = true},
             {.protocol = "SSM", .id = "missing", .ecu_byte_index = "9", .enabled = true}},
        .switches = {{.protocol = "SSM", .id = "rpm", .ecu_byte_index = "0", .ecu_bit = "1", .enabled = true},
                     {.protocol = "SSM", .id = "missing", .ecu_byte_index = "9", .enabled = true}}};
}
TEST(LoggerModelTest, DefinitionInstalledOnceAndSelectionNeverRewritesSupport)
{
    LoggerModel model;
    ASSERT_TRUE(model.InstallDefinition(Definition()));
    EXPECT_FALSE(model.InstallDefinition({}));
    ASSERT_NE(model.Parameter("SSM", "rpm"), nullptr);
    ASSERT_NE(model.Parameter("MUT_DMA", "rpm"), nullptr);
    ASSERT_NE(model.SwitchDefinition("SSM", "rpm"), nullptr);
    model.SetParameterSupported("SSM", "rpm", false);
    model.SetSelection({.protocol = "SSM", .gauge_ids = {"rpm", "unresolved"}});
    EXPECT_FALSE(model.ParameterSupported("SSM", "rpm"));
    EXPECT_TRUE(model.ParameterSupported("MUT_DMA", "rpm"));
    EXPECT_TRUE(model.SwitchSupported("SSM", "rpm"));
    EXPECT_EQ(model.Definition(), Definition());
    EXPECT_EQ(model.Selection().gauge_ids.back(), "unresolved");
}
TEST(LoggerModelTest, CapabilitiesDisableMissingParametersButRetainMissingSwitches)
{
    LoggerModel model;
    ASSERT_TRUE(model.InstallDefinition(Definition()));
    model.ApplyCapabilities("SSM", bytes::Bytes{2});
    EXPECT_TRUE(model.ParameterSupported("SSM", "rpm"));
    EXPECT_FALSE(model.ParameterSupported("SSM", "missing"));
    EXPECT_TRUE(model.SwitchSupported("SSM", "missing"));
    model.ApplyCapabilities("SSM", bytes::Bytes{0});
    EXPECT_FALSE(model.ParameterSupported("SSM", "rpm"));
    EXPECT_FALSE(model.SwitchSupported("SSM", "rpm"));
    EXPECT_TRUE(model.ParameterSupported("MUT_DMA", "rpm"));
}
TEST(LoggerModelTest, DefaultsUseCurrentSupportInDefinitionOrderAndKeepLimits)
{
    LoggerDefinition def;
    for (int i = 0; i < 30; ++i)
    {
        def.parameters.push_back({.protocol = "SSM", .id = std::to_string(i), .enabled = true});
        def.switches.push_back({.protocol = "SSM", .id = std::to_string(i), .enabled = true});
    }
    LoggerModel model;
    ASSERT_TRUE(model.InstallDefinition(std::move(def)));
    EXPECT_EQ(model.Selection().gauge_ids.size(), 15U);
    EXPECT_EQ(model.Selection().lower_panel_ids.size(), 12U);
    EXPECT_EQ(model.Selection().switch_ids.size(), 20U);
    model.SetParameterSupported("SSM", "0", false);
    model.SetSwitchSupported("SSM", "0", false);
    const auto fallback = model.DefaultSelection();
    EXPECT_EQ(fallback.protocol, "SSM");
    EXPECT_EQ(fallback.gauge_ids.front(), "1");
    EXPECT_EQ(fallback.gauge_ids.back(), "15");
    EXPECT_EQ(fallback.lower_panel_ids.back(), "12");
    EXPECT_EQ(fallback.switch_ids.back(), "20");
    EXPECT_EQ(model.Selection().gauge_ids.front(), "0");
}
TEST(LoggerModelTest, EmptyModelDoesNotInventDefinitionsOrSupportedIds)
{
    LoggerModel model;
    model.SetParameterSupported("SSM", "unknown", true);
    EXPECT_FALSE(model.ParameterSupported("SSM", "unknown"));
    EXPECT_TRUE(model.DefaultSelection().gauge_ids.empty());
}
} // namespace
} // namespace fastecu::logging
