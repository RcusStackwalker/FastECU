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
    model.set_parameter_support("SSM", "rpm", fastecu::logging::EcuSupport::Unsupported);
    model.set_selection({.protocol = "SSM", .gauge_ids = {"rpm", "unresolved"}});
    EXPECT_FALSE(model.parameter_available("SSM", "rpm"));
    EXPECT_TRUE(model.parameter_available("MUT_DMA", "rpm"));
    EXPECT_TRUE(model.switch_available("SSM", "rpm"));
    EXPECT_EQ(model.definition(), definition());
    EXPECT_EQ(model.selection().gauge_ids.back(), "unresolved");
}
TEST(LoggerModelTest, CapabilitiesLeaveMissingEvidenceEligibleByDefinition)
{
    LoggerModel model;
    ASSERT_TRUE(model.install_definition(definition()));
    model.apply_capabilities("SSM", bytes::Bytes{2});
    EXPECT_TRUE(model.parameter_available("SSM", "rpm"));
    EXPECT_TRUE(model.parameter_available("SSM", "missing"));
    EXPECT_TRUE(model.switch_available("SSM", "missing"));
    model.apply_capabilities("SSM", bytes::Bytes{0});
    EXPECT_FALSE(model.parameter_available("SSM", "rpm"));
    EXPECT_FALSE(model.switch_available("SSM", "rpm"));
    EXPECT_TRUE(model.parameter_available("MUT_DMA", "rpm"));
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
    EXPECT_EQ(model.selection().gauge_ids.size(), 15U);
    EXPECT_EQ(model.selection().lower_panel_ids.size(), 12U);
    EXPECT_EQ(model.selection().switch_ids.size(), 20U);
    model.set_parameter_support("SSM", "0", fastecu::logging::EcuSupport::Unsupported);
    model.set_switch_support("SSM", "0", fastecu::logging::EcuSupport::Unsupported);
    const auto fallback = model.default_selection();
    EXPECT_EQ(fallback.protocol, "SSM");
    EXPECT_EQ(fallback.gauge_ids.front(), "1");
    EXPECT_EQ(fallback.gauge_ids.back(), "15");
    EXPECT_EQ(fallback.lower_panel_ids.back(), "12");
    EXPECT_EQ(fallback.switch_ids.back(), "20");
    EXPECT_EQ(model.selection().gauge_ids.front(), "0");
}
TEST(LoggerModelTest, InitialDefaultsExcludeDisabledDefinitions)
{
    LoggerModel model;
    ASSERT_TRUE(model.install_definition(
        {.parameters = {{.protocol = "SSM", .id = "disabled"}, {.protocol = "SSM", .id = "enabled", .enabled = true}},
         .switches = {{.protocol = "SSM", .id = "disabled"}, {.protocol = "SSM", .id = "enabled", .enabled = true}}}));
    EXPECT_EQ(model.selection().gauge_ids, std::vector<std::string>{"enabled"});
    EXPECT_EQ(model.selection().lower_panel_ids, std::vector<std::string>{"enabled"});
    EXPECT_EQ(model.selection().switch_ids, std::vector<std::string>{"enabled"});
}
TEST(LoggerModelTest, SupportEvidenceIsSeparateFromEnablementAndCanBeReset)
{
    LoggerModel model;
    ASSERT_TRUE(model.install_definition(definition()));
    EXPECT_EQ(model.parameter_support("SSM", "rpm"), EcuSupport::Unknown);
    EXPECT_EQ(model.switch_support("SSM", "rpm"), EcuSupport::Unknown);
    model.apply_capabilities("SSM", bytes::Bytes{2});
    EXPECT_EQ(model.parameter_support("SSM", "rpm"), EcuSupport::Supported);
    EXPECT_EQ(model.parameter_support("SSM", "missing"), EcuSupport::Unknown);
    model.apply_capabilities("SSM", bytes::Bytes{0});
    EXPECT_EQ(model.parameter_support("SSM", "rpm"), EcuSupport::Unsupported);
    model.apply_capabilities("SSM", {});
    EXPECT_EQ(model.parameter_support("SSM", "rpm"), EcuSupport::Unknown);
    EXPECT_EQ(model.switch_support("SSM", "rpm"), EcuSupport::Unknown);
    EXPECT_TRUE(model.parameter_available("SSM", "rpm"));
    EXPECT_EQ(model.parameter_support("MUT_DMA", "rpm"), EcuSupport::Unknown);
}
TEST(LoggerModelTest, KnownSupportOverridesDisabledDefaultOnlyInItsNamespace)
{
    LoggerModel model;
    ASSERT_TRUE(model.install_definition(
        {.parameters = {{.protocol = "SSM", .id = "same"}}, .switches = {{.protocol = "SSM", .id = "same"}}}));
    EXPECT_FALSE(model.parameter_available("SSM", "same"));
    model.set_parameter_support("SSM", "same", EcuSupport::Supported);
    EXPECT_TRUE(model.parameter_available("SSM", "same"));
    EXPECT_FALSE(model.switch_available("SSM", "same"));
    model.set_parameter_support("SSM", "same", EcuSupport::Unsupported);
    EXPECT_FALSE(model.parameter_available("SSM", "same"));
}
TEST(LoggerModelTest, EmptyModelDoesNotInventDefinitionsOrSupportedIds)
{
    LoggerModel model;
    model.set_parameter_support("SSM", "unknown", fastecu::logging::EcuSupport::Supported);
    EXPECT_FALSE(model.parameter_available("SSM", "unknown"));
    EXPECT_TRUE(model.default_selection().gauge_ids.empty());
}
} // namespace
} // namespace fastecu::logging
