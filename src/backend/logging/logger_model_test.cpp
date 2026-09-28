#include "src/backend/logging/logger_model.h"

#include <gtest/gtest.h>

namespace fastecu::logging
{
namespace
{
LoggerDefinition definition()
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
    ASSERT_TRUE(model.install_definition(definition()));
    EXPECT_FALSE(model.install_definition({}));
    ASSERT_NE(model.parameter("SSM", "rpm"), nullptr);
    ASSERT_NE(model.parameter("MUT_DMA", "rpm"), nullptr);
    ASSERT_NE(model.switch_definition("SSM", "rpm"), nullptr);
    model.set_parameter_supported("SSM", "rpm", false);
    model.set_selection({.protocol = "SSM", .gauge_ids = {"rpm", "unresolved"}});
    EXPECT_FALSE(model.parameter_supported("SSM", "rpm"));
    EXPECT_TRUE(model.parameter_supported("MUT_DMA", "rpm"));
    EXPECT_TRUE(model.switch_supported("SSM", "rpm"));
    EXPECT_EQ(model.definition(), definition());
    EXPECT_EQ(model.selection().gauge_ids.back(), "unresolved");
}
TEST(LoggerModelTest, CapabilitiesDisableMissingParametersButRetainMissingSwitches)
{
    LoggerModel model;
    ASSERT_TRUE(model.install_definition(definition()));
    model.apply_capabilities("SSM", bytes::Bytes{2});
    EXPECT_TRUE(model.parameter_supported("SSM", "rpm"));
    EXPECT_FALSE(model.parameter_supported("SSM", "missing"));
    EXPECT_TRUE(model.switch_supported("SSM", "missing"));
    model.apply_capabilities("SSM", bytes::Bytes{0});
    EXPECT_FALSE(model.parameter_supported("SSM", "rpm"));
    EXPECT_FALSE(model.switch_supported("SSM", "rpm"));
    EXPECT_TRUE(model.parameter_supported("MUT_DMA", "rpm"));
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
    ASSERT_TRUE(model.install_definition(std::move(def)));
    EXPECT_EQ(model.selection().gauge_ids.size(), 15);
    EXPECT_EQ(model.selection().lower_panel_ids.size(), 12);
    EXPECT_EQ(model.selection().switch_ids.size(), 20);
    model.set_parameter_supported("SSM", "0", false);
    model.set_switch_supported("SSM", "0", false);
    const auto fallback = model.default_selection();
    EXPECT_EQ(fallback.protocol, "SSM");
    EXPECT_EQ(fallback.gauge_ids.front(), "1");
    EXPECT_EQ(fallback.gauge_ids.back(), "15");
    EXPECT_EQ(fallback.lower_panel_ids.back(), "12");
    EXPECT_EQ(fallback.switch_ids.back(), "20");
    EXPECT_EQ(model.selection().gauge_ids.front(), "0");
}
TEST(LoggerModelTest, EmptyModelDoesNotInventDefinitionsOrSupportedIds)
{
    LoggerModel model;
    model.set_parameter_supported("SSM", "unknown", true);
    EXPECT_FALSE(model.parameter_supported("SSM", "unknown"));
    EXPECT_TRUE(model.default_selection().gauge_ids.empty());
}
} // namespace
} // namespace fastecu::logging
