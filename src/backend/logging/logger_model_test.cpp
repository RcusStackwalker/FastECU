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
    model.SetParameterSupport("SSM", "rpm", fastecu::logging::EcuSupport::kUnsupported);
    model.SetSelection({.protocol = "SSM", .gauge_ids = {"rpm", "unresolved"}});
    EXPECT_FALSE(model.ParameterAvailable("SSM", "rpm"));
    EXPECT_TRUE(model.ParameterAvailable("MUT_DMA", "rpm"));
    EXPECT_TRUE(model.SwitchAvailable("SSM", "rpm"));
    EXPECT_EQ(model.Definition(), Definition());
    EXPECT_EQ(model.Selection().gauge_ids.back(), "unresolved");
}
TEST(LoggerModelTest, CapabilitiesLeaveMissingEvidenceEligibleByDefinition)
{
    LoggerModel model;
    ASSERT_TRUE(model.InstallDefinition(Definition()));
    model.ApplyCapabilities("SSM", bytes::Bytes{2});
    EXPECT_TRUE(model.ParameterAvailable("SSM", "rpm"));
    EXPECT_TRUE(model.ParameterAvailable("SSM", "missing"));
    EXPECT_TRUE(model.SwitchAvailable("SSM", "missing"));
    model.ApplyCapabilities("SSM", bytes::Bytes{0});
    EXPECT_FALSE(model.ParameterAvailable("SSM", "rpm"));
    EXPECT_FALSE(model.SwitchAvailable("SSM", "rpm"));
    EXPECT_TRUE(model.ParameterAvailable("MUT_DMA", "rpm"));
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
    model.SetParameterSupport("SSM", "0", fastecu::logging::EcuSupport::kUnsupported);
    model.SetSwitchSupport("SSM", "0", fastecu::logging::EcuSupport::kUnsupported);
    const auto fallback = model.DefaultSelection();
    EXPECT_EQ(fallback.protocol, "SSM");
    EXPECT_EQ(fallback.gauge_ids.front(), "1");
    EXPECT_EQ(fallback.gauge_ids.back(), "15");
    EXPECT_EQ(fallback.lower_panel_ids.back(), "12");
    EXPECT_EQ(fallback.switch_ids.back(), "20");
    EXPECT_EQ(model.Selection().gauge_ids.front(), "0");
}
TEST(LoggerModelTest, InitialDefaultsExcludeDisabledDefinitions)
{
    LoggerModel model;
    ASSERT_TRUE(model.InstallDefinition(
        {.parameters = {{.protocol = "SSM", .id = "disabled"}, {.protocol = "SSM", .id = "enabled", .enabled = true}},
         .switches = {{.protocol = "SSM", .id = "disabled"}, {.protocol = "SSM", .id = "enabled", .enabled = true}}}));
    EXPECT_EQ(model.Selection().gauge_ids, std::vector<std::string>{"enabled"});
    EXPECT_EQ(model.Selection().lower_panel_ids, std::vector<std::string>{"enabled"});
    EXPECT_EQ(model.Selection().switch_ids, std::vector<std::string>{"enabled"});
}
TEST(LoggerModelTest, SupportEvidenceIsSeparateFromEnablementAndCanBeReset)
{
    LoggerModel model;
    ASSERT_TRUE(model.InstallDefinition(Definition()));
    EXPECT_EQ(model.ParameterSupport("SSM", "rpm"), EcuSupport::kUnknown);
    EXPECT_EQ(model.SwitchSupport("SSM", "rpm"), EcuSupport::kUnknown);
    model.ApplyCapabilities("SSM", bytes::Bytes{2});
    EXPECT_EQ(model.ParameterSupport("SSM", "rpm"), EcuSupport::kSupported);
    EXPECT_EQ(model.ParameterSupport("SSM", "missing"), EcuSupport::kUnknown);
    model.ApplyCapabilities("SSM", bytes::Bytes{0});
    EXPECT_EQ(model.ParameterSupport("SSM", "rpm"), EcuSupport::kUnsupported);
    model.ApplyCapabilities("SSM", {});
    EXPECT_EQ(model.ParameterSupport("SSM", "rpm"), EcuSupport::kUnknown);
    EXPECT_EQ(model.SwitchSupport("SSM", "rpm"), EcuSupport::kUnknown);
    EXPECT_TRUE(model.ParameterAvailable("SSM", "rpm"));
    EXPECT_EQ(model.ParameterSupport("MUT_DMA", "rpm"), EcuSupport::kUnknown);
}
TEST(LoggerModelTest, KnownSupportOverridesDisabledDefaultOnlyInItsNamespace)
{
    LoggerModel model;
    ASSERT_TRUE(model.InstallDefinition(
        {.parameters = {{.protocol = "SSM", .id = "same"}}, .switches = {{.protocol = "SSM", .id = "same"}}}));
    EXPECT_FALSE(model.ParameterAvailable("SSM", "same"));
    model.SetParameterSupport("SSM", "same", EcuSupport::kSupported);
    EXPECT_TRUE(model.ParameterAvailable("SSM", "same"));
    EXPECT_FALSE(model.SwitchAvailable("SSM", "same"));
    model.SetParameterSupport("SSM", "same", EcuSupport::kUnsupported);
    EXPECT_FALSE(model.ParameterAvailable("SSM", "same"));
}
TEST(LoggerModelTest, DefaultsNeverMixProtocolNamespaces)
{
    LoggerModel model;
    ASSERT_TRUE(model.InstallDefinition({.parameters = {{.protocol = "SSM", .id = "ssm", .enabled = true},
                                                        {.protocol = "MUT_DMA", .id = "mut", .enabled = true}},
                                         .switches = {{.protocol = "SSM", .id = "flag", .enabled = true},
                                                      {.protocol = "MUT_DMA", .id = "mut-flag", .enabled = true}}}));
    EXPECT_EQ(model.Selection().lower_panel_ids, (std::vector<std::string>{"ssm"}));
    EXPECT_EQ(model.Selection().switch_ids, (std::vector<std::string>{"flag"}));
}
TEST(LoggerModelTest, EmptyModelDoesNotInventDefinitionsOrSupportedIds)
{
    LoggerModel model;
    model.SetParameterSupport("SSM", "unknown", fastecu::logging::EcuSupport::kSupported);
    EXPECT_FALSE(model.ParameterAvailable("SSM", "unknown"));
    EXPECT_TRUE(model.DefaultSelection().gauge_ids.empty());
}
} // namespace
} // namespace fastecu::logging
