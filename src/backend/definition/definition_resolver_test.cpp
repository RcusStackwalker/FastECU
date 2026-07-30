#include "src/backend/definition/definition_resolver.h"
#include "src/backend/definition/ecuflash_parser.h"
#include "src/backend/definition/romraider_parser.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{

using ::testing::ElementsAre;
using ::testing::HasSubstr;

std::vector<std::uint8_t> xml_bytes(std::string_view text)
{
    return {text.begin(), text.end()};
}

UnresolvedDefinition doc(
    std::string id,
    std::vector<std::string> parents = {},
    DefinitionFormat format = DefinitionFormat::RomRaider)
{
    return UnresolvedDefinition{
        .format = format,
        .source = id + ".xml",
        .identity = RomIdentity{.xml_id = std::move(id)},
        .parents = std::move(parents),
    };
}

UnresolvedCalibrationMap map(std::string id, std::string name = {})
{
    if (name.empty())
    {
        name = id;
    }
    return UnresolvedCalibrationMap{
        .id = id.empty() ? std::nullopt : std::optional{std::move(id)},
        .name = std::move(name),
        .type = "2D",
    };
}

UnresolvedScaling scaling(std::string name, std::string storage_type = "uint16")
{
    return UnresolvedScaling{
        .name = std::move(name),
        .from_byte = "x*0.5",
        .to_byte = "x*2",
        .format = "0.0",
        .storage_type = std::move(storage_type),
        .endian = "big",
    };
}

class DefinitionSet
{
  public:
    DefinitionSet(std::initializer_list<std::pair<const std::string, UnresolvedDefinition>> definitions)
        : definitions_(definitions)
    {
    }

    DefinitionLoader loader()
    {
        return [this](DefinitionFormat, std::string_view id) -> Result<UnresolvedDefinition>
        {
            ++loads_[std::string(id)];
            auto definition = definitions_.find(std::string(id));
            if (definition == definitions_.end())
            {
                return fail(ErrorKind::InvalidConfig, "definition ID not found: '" + std::string(id) + "'");
            }
            return definition->second;
        };
    }

    int loads(std::string_view id) const
    {
        auto count = loads_.find(std::string(id));
        return count == loads_.end() ? 0 : count->second;
    }

  private:
    std::map<std::string, UnresolvedDefinition> definitions_;
    std::map<std::string, int> loads_;
};

TEST(DefinitionResolverTest, ResolvesSingleLevelBaseBeforeChildOverrides)
{
    auto base = doc("BASE");
    base.metadata.make = "Subaru";
    base.metadata.model = "Impreza";
    base.identity.internal_id = "BASE-ID";

    auto child = doc("CHILD", {"BASE"});
    child.metadata.model = "WRX";

    DefinitionSet definitions{{"BASE", base}};
    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->identity.xml_id, "CHILD");
    EXPECT_EQ(result->identity.internal_id, "BASE-ID");
    EXPECT_EQ(result->metadata.make, "Subaru");
    EXPECT_EQ(result->metadata.model, "WRX");
    EXPECT_THAT(result->resolved_sources, ElementsAre("BASE.xml", "CHILD.xml"));
}

TEST(DefinitionResolverTest, ResolvesMultiLevelInheritance)
{
    auto grandparent = doc("GRANDPARENT");
    grandparent.metadata.make = "Subaru";
    auto parent = doc("PARENT", {"GRANDPARENT"});
    parent.metadata.model = "Legacy";
    auto child = doc("CHILD", {"PARENT"});
    child.metadata.year = "2008";

    DefinitionSet definitions{
        {"GRANDPARENT", grandparent},
        {"PARENT", parent},
    };
    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result);
    EXPECT_EQ(result->metadata.make, "Subaru");
    EXPECT_EQ(result->metadata.model, "Legacy");
    EXPECT_EQ(result->metadata.year, "2008");
    EXPECT_THAT(
        result->resolved_sources,
        ElementsAre("GRANDPARENT.xml", "PARENT.xml", "CHILD.xml"));
    EXPECT_THAT(
        result->resolved_definition_ids,
        ElementsAre("GRANDPARENT", "PARENT", "CHILD"));
}

TEST(DefinitionResolverTest, InheritsRuntimeRowsAndAllowsExplicitDefaultOverrides)
{
    auto base = doc("BASE");
    auto base_map = map("fuel", "Fuel");
    base_map.start_position = "7";
    base_map.interval = "2";
    base_map.log_parameter = "P_BASE";
    base_map.x_axis = UnresolvedAxisDefinition{
        .type = "Static X Axis",
        .name = "Load",
        .size = 1,
        .start_position = "9",
        .interval = "4",
        .log_parameter = "P_AXIS",
        .static_data = std::vector<std::string>{"1.0"},
    };
    base.maps.push_back(base_map);

    auto child = doc("CHILD", {"BASE"});
    auto child_map = map("fuel", "Fuel");
    child_map.start_position = "1";
    child_map.interval = "1";
    child.maps.push_back(child_map);

    DefinitionSet definitions{{"BASE", base}};
    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().start_position, "1");
    EXPECT_EQ(result->maps.front().interval, "1");
    EXPECT_EQ(result->maps.front().log_parameter, "P_BASE");
    EXPECT_EQ(result->maps.front().x_axis.start_position, "9");
    EXPECT_EQ(result->maps.front().x_axis.interval, "4");
    EXPECT_EQ(result->maps.front().x_axis.log_parameter, "P_AXIS");
    EXPECT_EQ(result->maps.front().x_axis.static_data, std::vector<std::string>{"1.0"});
}

TEST(DefinitionResolverTest, RejectsStaticAxisDataThatDoesNotMatchAxisSize)
{
    auto root = doc("ROOT");
    auto fuel = map("fuel", "Fuel");
    fuel.x_size = 2;
    fuel.x_axis = UnresolvedAxisDefinition{
        .type = "Static X Axis",
        .name = "Load",
        .size = 2,
        .static_data = std::vector<std::string>{"1.0"},
    };
    root.maps.push_back(fuel);
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("static data"));
    EXPECT_THAT(result.error().detail, HasSubstr("x axis"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RejectsStaticXAxisWithoutData)
{
    auto root = doc("ROOT");
    auto fuel = map("fuel", "Fuel");
    fuel.x_size = 2;
    fuel.x_axis = UnresolvedAxisDefinition{
        .type = "Static X Axis",
        .name = "Load",
        .size = 2,
    };
    root.maps.push_back(fuel);
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("static data"));
    EXPECT_THAT(result.error().detail, HasSubstr("x axis"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RejectsStaticDataOutsideXAxisPosition)
{
    auto root = doc("ROOT");
    auto fuel = map("fuel", "Fuel");
    fuel.y_size = 2;
    fuel.y_axis = UnresolvedAxisDefinition{
        .type = "Static X Axis",
        .name = "Load",
        .size = 2,
        .static_data = std::vector<std::string>{"1.0", "2.0"},
    };
    root.maps.push_back(fuel);
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("static data"));
    EXPECT_THAT(result.error().detail, HasSubstr("y axis"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, MissingParentIsContextualInvalidConfig)
{
    const auto root = doc("CHILD", {"MISSING"});
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("MISSING"));
    EXPECT_THAT(result.error().detail, HasSubstr("CHILD -> MISSING"));
    EXPECT_THAT(result.error().detail, HasSubstr("RomRaider"));
    EXPECT_THAT(result.error().detail, HasSubstr("CHILD.xml"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RecursiveDefinitionFailureIncludesInheritanceChain)
{
    const auto root = doc("ROOT", {"BASE"});
    const auto original = root;
    auto invalid_base = doc("BASE");
    invalid_base.source.clear();
    DefinitionSet definitions{{"BASE", invalid_base}};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("ROOT -> BASE"));
    EXPECT_THAT(result.error().detail, HasSubstr("BASE"));
    EXPECT_THAT(result.error().detail, HasSubstr("source"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, ReportsSelfCycle)
{
    const auto root = doc("A", {"A"});
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("A -> A"));
    EXPECT_THAT(result.error().detail, HasSubstr("RomRaider"));
    EXPECT_THAT(result.error().detail, HasSubstr("A.xml"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, ReportsCompleteCycle)
{
    const auto root = doc("A", {"B"});
    const auto original = root;
    DefinitionSet definitions{
        {"B", doc("B", {"C"})},
        {"C", doc("C", {"A"})},
    };

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("A -> B -> C -> A"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RejectsCrossFormatParent)
{
    const auto root = doc("CHILD", {"BASE"}, DefinitionFormat::RomRaider);
    const auto original = root;
    DefinitionSet definitions{
        {"BASE", doc("BASE", {}, DefinitionFormat::EcuFlash)},
    };

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("cross-format"));
    EXPECT_THAT(result.error().detail, HasSubstr("CHILD -> BASE"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, MemoizesSharedBaseInDiamondGraph)
{
    DefinitionSet definitions{
        {"LEFT", doc("LEFT", {"SHARED"})},
        {"RIGHT", doc("RIGHT", {"SHARED"})},
        {"SHARED", doc("SHARED")},
    };

    auto result = resolve_definition(doc("ROOT", {"LEFT", "RIGHT"}), definitions.loader());

    ASSERT_TRUE(result);
    EXPECT_EQ(definitions.loads("SHARED"), 1);
    EXPECT_THAT(
        result->resolved_sources,
        ElementsAre("SHARED.xml", "LEFT.xml", "RIGHT.xml", "ROOT.xml"));
}

TEST(DefinitionResolverTest, MergesMapsByStableIdAndAppendsChildMaps)
{
    auto base = doc("BASE");
    base.maps.push_back(map("boost", "Boost Limit"));
    auto fuel = map("fuel", "Fuel");
    fuel.category = "Fuel";
    fuel.address = 0x100;
    fuel.storage_type = "uint16";
    fuel.x_size = 4;
    fuel.x_axis = UnresolvedAxisDefinition{
        .type = "X Axis",
        .name = "Engine Speed",
        .units = "rpm",
        .address = 0x200,
        .size = 4,
    };
    base.maps.push_back(fuel);

    auto child = doc("CHILD", {"BASE"});
    auto fuel_override = map("fuel", "Fuel");
    fuel_override.description = "Child fuel map";
    fuel_override.address = 0x300;
    fuel_override.x_axis.units = "r/min";
    child.maps.push_back(fuel_override);
    child.maps.push_back(map("timing", "Ignition Timing"));

    DefinitionSet definitions{{"BASE", base}};
    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 3U);
    EXPECT_EQ(result->maps[0].id, "boost");
    EXPECT_EQ(result->maps[1].id, "fuel");
    EXPECT_EQ(result->maps[1].category, "Fuel");
    EXPECT_EQ(result->maps[1].description, "Child fuel map");
    EXPECT_EQ(result->maps[1].address, 0x300U);
    EXPECT_EQ(result->maps[1].storage_type, "uint16");
    EXPECT_EQ(result->maps[1].x_axis.name, "Engine Speed");
    EXPECT_EQ(result->maps[1].x_axis.units, "r/min");
    EXPECT_EQ(result->maps[1].x_axis.address, 0x200U);
    EXPECT_EQ(result->maps[2].id, "timing");
}

TEST(DefinitionResolverTest, FallsBackToMapNameWhenStableIdIsAbsent)
{
    auto base = doc("BASE");
    auto base_map = map("", "Fuel");
    base_map.category = "Fuel";
    base.maps.push_back(base_map);

    auto child = doc("CHILD", {"BASE"});
    auto child_map = map("", "Fuel");
    child_map.description = "Override";
    child.maps.push_back(child_map);

    DefinitionSet definitions{{"BASE", base}};
    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps[0].category, "Fuel");
    EXPECT_EQ(result->maps[0].description, "Override");
}

TEST(DefinitionResolverTest, FallsBackToNameWhenOnlyBaseHasStableMapId)
{
    auto base = doc("BASE");
    auto base_map = map("fuel", "Fuel");
    base_map.category = "Fuel";
    base.maps.push_back(base_map);

    auto child = doc("CHILD", {"BASE"});
    auto child_map = map("", "Fuel");
    child_map.description = "Override";
    child.maps.push_back(child_map);
    DefinitionSet definitions{{"BASE", base}};

    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps[0].id, "fuel");
    EXPECT_EQ(result->maps[0].category, "Fuel");
    EXPECT_EQ(result->maps[0].description, "Override");
}

TEST(DefinitionResolverTest, FallsBackToNameWhenOnlyChildHasStableMapId)
{
    auto base = doc("BASE");
    auto base_map = map("", "Fuel");
    base_map.category = "Fuel";
    base.maps.push_back(base_map);

    auto child = doc("CHILD", {"BASE"});
    auto child_map = map("fuel", "Fuel");
    child_map.description = "Override";
    child.maps.push_back(child_map);
    DefinitionSet definitions{{"BASE", base}};

    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps[0].id, "fuel");
    EXPECT_EQ(result->maps[0].category, "Fuel");
    EXPECT_EQ(result->maps[0].description, "Override");
}

TEST(DefinitionResolverTest, RejectsAmbiguousNameFallbackAcrossStableIds)
{
    auto base = doc("BASE");
    base.maps.push_back(map("fuel-a", "Fuel"));
    base.maps.push_back(map("fuel-b", "Fuel"));

    auto child = doc("CHILD", {"BASE"});
    child.maps.push_back(map("", "Fuel"));
    const auto original = child;
    DefinitionSet definitions{{"BASE", base}};

    auto result = resolve_definition(child, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("ambiguous"));
    EXPECT_THAT(result.error().detail, HasSubstr("Fuel"));
    EXPECT_EQ(child, original);
}

TEST(DefinitionResolverTest, RomRaiderOmittedFieldsDoNotResetInheritedValues)
{
    const auto xml = xml_bytes(R"xml(
      <roms>
        <rom>
          <romid><xmlid>BASE</xmlid></romid>
          <table id="fuel" name="Fuel" type="3D" sizex="4" sizey="2"
                 swapxy="true" flipx="true" flipy="true">
            <table type="X Axis" name="RPM" elements="4">
              <scaling name="base-rpm" expression="x*2" to_byte="x/2"/>
            </table>
          </table>
        </rom>
        <rom base="BASE">
          <romid><xmlid>CHILD</xmlid></romid>
          <table id="fuel" name="Fuel" description="Child description">
            <table type="X Axis" name="RPM">
              <scaling name="child-rpm" units="r/min"/>
            </table>
          </table>
        </rom>
      </roms>)xml");
    auto base = parse_romraider_definition(xml, "romraider.xml", "BASE");
    auto child = parse_romraider_definition(xml, "romraider.xml", "CHILD");
    ASSERT_TRUE(base);
    ASSERT_TRUE(child);
    const auto original = *child;
    DefinitionSet definitions{{"BASE", *base}};

    auto result = resolve_definition(*child, definitions.loader());

    ASSERT_TRUE(result) << result.error().detail;
    ASSERT_EQ(result->maps.size(), 1U);
    const auto& fuel = result->maps.front();
    EXPECT_EQ(fuel.description, "Child description");
    EXPECT_EQ(fuel.x_size, 4U);
    EXPECT_EQ(fuel.y_size, 2U);
    EXPECT_TRUE(fuel.swap_xy);
    EXPECT_TRUE(fuel.flip_x);
    EXPECT_TRUE(fuel.flip_y);
    EXPECT_EQ(fuel.x_axis.size, 4U);
    EXPECT_EQ(fuel.x_axis.units, "r/min");
    EXPECT_EQ(fuel.x_axis.from_byte, "x*2");
    EXPECT_EQ(fuel.x_axis.to_byte, "x/2");
    EXPECT_EQ(*child, original);
}

TEST(DefinitionResolverTest, EcuFlashExplicitDefaultsOverrideInheritedValuesAndMatchByName)
{
    auto base = parse_ecuflash_definition(xml_bytes(R"xml(
      <rom>
        <romid><xmlid>BASE</xmlid></romid>
        <table id="fuel" name="Fuel" type="3D" sizex="4" sizey="2"
               swapxy="true" flipx="true" flipy="true">
          <table type="X Axis" name="RPM" elements="4"/>
        </table>
      </rom>)xml"),
                                          "base.xml");
    auto child = parse_ecuflash_definition(xml_bytes(R"xml(
      <rom>
        <romid><xmlid>CHILD</xmlid></romid>
        <include>BASE</include>
        <table name="Fuel" description="Child description" sizex="1" sizey="1"
               swapxy="false" flipx="false" flipy="false">
          <table type="X Axis" name="RPM" elements="1">
            <scaling name="child-rpm" toexpr="x" frexpr="x"/>
          </table>
        </table>
      </rom>)xml"),
                                           "child.xml");
    ASSERT_TRUE(base);
    ASSERT_TRUE(child);
    const auto original = *child;
    DefinitionSet definitions{{"BASE", *base}};

    auto result = resolve_definition(*child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    const auto& fuel = result->maps.front();
    EXPECT_EQ(fuel.id, "fuel");
    EXPECT_EQ(fuel.description, "Child description");
    EXPECT_EQ(fuel.x_size, 1U);
    EXPECT_EQ(fuel.y_size, 1U);
    EXPECT_FALSE(fuel.swap_xy);
    EXPECT_FALSE(fuel.flip_x);
    EXPECT_FALSE(fuel.flip_y);
    EXPECT_EQ(fuel.x_axis.size, 1U);
    EXPECT_EQ(fuel.x_axis.from_byte, "x");
    EXPECT_EQ(fuel.x_axis.to_byte, "x");
    EXPECT_EQ(*child, original);
}

TEST(DefinitionResolverTest, EcuFlashDirectDataInheritsOmittedAxisOffsets)
{
    auto base = parse_ecuflash_definition(xml_bytes(R"xml(
      <rom>
        <romid><xmlid>BASE</xmlid></romid>
        <table id="load" name="Load" type="2D" sizex="2" sizey="1">
          <table type="Static X Axis" name="Breakpoints" elements="2"
                 startpos="9" interval="4">
            <data>0.5</data><data>1.0</data>
          </table>
        </table>
      </rom>)xml"),
                                          "base.xml");
    auto child = parse_ecuflash_definition(xml_bytes(R"xml(
      <rom>
        <romid><xmlid>CHILD</xmlid></romid>
        <include>BASE</include>
        <table id="load" name="Load" type="2D">
          <data>0.6</data><data>1.1</data>
        </table>
      </rom>)xml"),
                                           "child.xml");
    ASSERT_TRUE(base);
    ASSERT_TRUE(child);
    const auto original = *child;
    DefinitionSet definitions{{"BASE", *base}};

    auto result = resolve_definition(*child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().x_axis.start_position, "9");
    EXPECT_EQ(result->maps.front().x_axis.interval, "4");
    EXPECT_EQ(
        result->maps.front().x_axis.static_data,
        (std::vector<std::string>{"0.6", "1.1"}));
    EXPECT_EQ(*child, original);
}

TEST(DefinitionResolverTest, OmittedAxisSizeDoesNotFollowExplicitChildMapDimension)
{
    const auto xml = xml_bytes(R"xml(
      <roms>
        <rom>
          <romid><xmlid>BASE</xmlid></romid>
          <table id="fuel" name="Fuel" type="2D" sizex="4">
            <table type="X Axis" name="RPM" elements="4"/>
          </table>
        </rom>
        <rom base="BASE">
          <romid><xmlid>CHILD</xmlid></romid>
          <table id="fuel" name="Fuel" sizex="3">
            <table type="X Axis" name="RPM"/>
          </table>
        </rom>
      </roms>)xml");
    auto base = parse_romraider_definition(xml, "romraider.xml", "BASE");
    auto child = parse_romraider_definition(xml, "romraider.xml", "CHILD");
    ASSERT_TRUE(base);
    ASSERT_TRUE(child);
    const auto original = *child;
    DefinitionSet definitions{{"BASE", *base}};

    auto result = resolve_definition(*child, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("inconsistent dimension"));
    EXPECT_EQ(*child, original);
}

TEST(DefinitionResolverTest, ImplicitInheritedAxisSizeFollowsExplicitChildMapDimension)
{
    const auto xml = xml_bytes(R"xml(
      <roms>
        <rom>
          <romid><xmlid>BASE</xmlid></romid>
          <table id="grid" name="Grid" type="3D">
            <table type="X Axis" name="X"/>
            <table type="Y Axis" name="Y"/>
          </table>
        </rom>
        <rom base="BASE">
          <romid><xmlid>CHILD</xmlid></romid>
          <table id="grid" name="Grid" sizex="2" sizey="2">
            <table type="X Axis" name="X" storageaddress="50"/>
            <table type="Y Axis" name="Y" storageaddress="60"/>
          </table>
        </rom>
      </roms>)xml");
    auto base = parse_romraider_definition(xml, "romraider.xml", "BASE");
    auto child = parse_romraider_definition(xml, "romraider.xml", "CHILD");
    ASSERT_TRUE(base);
    ASSERT_TRUE(child);
    const auto original = *child;
    DefinitionSet definitions{{"BASE", *base}};

    auto result = resolve_definition(*child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps.front().x_size, 2U);
    EXPECT_EQ(result->maps.front().y_size, 2U);
    EXPECT_EQ(result->maps.front().x_axis.size, 2U);
    EXPECT_EQ(result->maps.front().y_axis.size, 2U);
    EXPECT_EQ(result->maps.front().x_axis.address, 0x50U);
    EXPECT_EQ(result->maps.front().y_axis.address, 0x60U);
    EXPECT_EQ(*child, original);
}

TEST(DefinitionResolverTest, ResolvesMapAndAxisScalingReferences)
{
    auto root = doc("ROOT");
    auto value_scaling = scaling("value");
    value_scaling.units = "%";
    auto axis_scaling = scaling("axis", "uint8");
    axis_scaling.units = "rpm";
    root.scalings = {value_scaling, axis_scaling};

    auto fuel = map("fuel", "Fuel");
    fuel.scaling_name = "value";
    fuel.x_size = 4;
    fuel.x_axis = UnresolvedAxisDefinition{
        .type = "X Axis",
        .name = "Engine Speed",
        .size = 4,
        .scaling_name = "axis",
    };
    root.maps.push_back(fuel);

    DefinitionSet definitions{};
    auto result = resolve_definition(root, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->maps.size(), 1U);
    EXPECT_EQ(result->maps[0].storage_type, "uint16");
    EXPECT_EQ(result->maps[0].endian, "big");
    EXPECT_EQ(result->maps[0].x_axis.units, "rpm");
    EXPECT_EQ(result->maps[0].x_axis.format, "0.0");
    EXPECT_EQ(result->maps[0].x_axis.storage_type, "uint8");
    EXPECT_EQ(result->maps[0].x_axis.endian, "big");
    EXPECT_EQ(result->maps[0].x_axis.from_byte, "x*0.5");
    EXPECT_EQ(result->maps[0].x_axis.to_byte, "x*2");
}

TEST(DefinitionResolverTest, ExplicitIdentityScalingOverridesAxisExpression)
{
    auto root = doc("ROOT");
    auto axis_scaling = scaling("axis", "uint8");
    axis_scaling.from_byte = "x";
    root.scalings.push_back(axis_scaling);

    auto fuel = map("fuel", "Fuel");
    fuel.x_axis = UnresolvedAxisDefinition{
        .type = "X Axis",
        .name = "Engine Speed",
        .from_byte = "x+1",
        .scaling_name = "axis",
    };
    root.maps.push_back(fuel);

    DefinitionSet definitions{};
    auto result = resolve_definition(root, definitions.loader());

    ASSERT_TRUE(result);
    EXPECT_EQ(result->maps[0].x_axis.from_byte, "x");
}

TEST(DefinitionResolverTest, RejectsConflictingDuplicateScalingDefinitions)
{
    auto base = doc("BASE");
    base.scalings.push_back(scaling("shared"));
    auto child = doc("CHILD", {"BASE"});
    child.scalings.push_back(scaling("shared", "uint8"));
    const auto original = child;
    DefinitionSet definitions{{"BASE", base}};

    auto result = resolve_definition(child, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("shared"));
    EXPECT_THAT(result.error().detail, HasSubstr("conflicting"));
    EXPECT_EQ(child, original);
}

TEST(DefinitionResolverTest, AcceptsOneCanonicalCopyOfIdenticalScaling)
{
    auto base = doc("BASE");
    base.scalings.push_back(scaling("shared"));
    auto child = doc("CHILD", {"BASE"});
    child.scalings.push_back(scaling("shared"));
    DefinitionSet definitions{{"BASE", base}};

    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->scalings.size(), 1U);
    EXPECT_EQ(result->scalings.front().name, "shared");
}

TEST(DefinitionResolverTest, CanonicalizesIdenticalLocalScalingDefinitions)
{
    auto root = doc("ROOT");
    root.scalings = {scaling("shared"), scaling("shared")};
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_TRUE(result);
    ASSERT_EQ(result->scalings.size(), 1U);
    EXPECT_EQ(result->scalings.front().name, "shared");
}

TEST(DefinitionResolverTest, RejectsSelectableMapWithoutSelectionScaling)
{
    auto root = doc("ROOT");
    auto modes = map("modes", "Modes");
    modes.type = "Selectable";
    modes.storage_type = "bloblist";
    root.maps.push_back(modes);
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("modes"));
    EXPECT_THAT(result.error().detail, HasSubstr("scaling"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, KeepsResolvedSourceProvenanceUnique)
{
    auto base = doc("BASE");
    base.source = "romraider.xml";
    auto child = doc("CHILD", {"BASE"});
    child.source = "romraider.xml";
    DefinitionSet definitions{{"BASE", base}};

    auto result = resolve_definition(child, definitions.loader());

    ASSERT_TRUE(result);
    EXPECT_THAT(result->resolved_sources, ElementsAre("romraider.xml"));
}

TEST(DefinitionResolverTest, RejectsUnresolvedScalingWithoutMutatingInput)
{
    auto root = doc("ROOT");
    auto fuel = map("fuel", "Fuel");
    fuel.scaling_name = "missing";
    root.maps.push_back(fuel);
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("missing"));
    EXPECT_THAT(result.error().detail, HasSubstr("fuel"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RejectsZeroRequiredDimensionWithoutMutatingInput)
{
    auto root = doc("ROOT");
    auto fuel = map("fuel", "Fuel");
    fuel.x_size = 0;
    root.maps.push_back(fuel);
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("zero"));
    EXPECT_THAT(result.error().detail, HasSubstr("fuel"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RejectsIncompleteAxisWithoutMutatingInput)
{
    auto root = doc("ROOT");
    auto fuel = map("fuel", "Fuel");
    fuel.x_axis.type = "X Axis";
    root.maps.push_back(fuel);
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("incomplete"));
    EXPECT_THAT(result.error().detail, HasSubstr("x axis"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RejectsDuplicateMapKeyWithoutMutatingInput)
{
    auto root = doc("ROOT");
    root.maps = {map("fuel", "Primary Fuel"), map("fuel", "Secondary Fuel")};
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("duplicate map key"));
    EXPECT_THAT(result.error().detail, HasSubstr("fuel"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RejectsContradictorySelectionStorageWithoutMutatingInput)
{
    auto root = doc("ROOT");
    auto modes = scaling("modes", "uint8");
    modes.selections = {{"disabled", "00"}, {"enabled", "01"}};
    root.scalings.push_back(modes);
    const auto original = root;
    DefinitionSet definitions{};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("modes"));
    EXPECT_THAT(result.error().detail, HasSubstr("bloblist"));
    EXPECT_EQ(root, original);
}

TEST(DefinitionResolverTest, RejectsLoaderDefinitionWhoseIdentityDoesNotMatchReference)
{
    const auto root = doc("ROOT", {"EXPECTED"});
    const auto original = root;
    DefinitionSet definitions{{"EXPECTED", doc("OTHER")}};

    auto result = resolve_definition(root, definitions.loader());

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("EXPECTED"));
    EXPECT_THAT(result.error().detail, HasSubstr("OTHER"));
    EXPECT_EQ(root, original);
}

} // namespace
} // namespace fastecu::definition
