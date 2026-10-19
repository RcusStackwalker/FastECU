#include "src/backend/definition/definition_resolver.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace fastecu::definition
{
namespace
{
using fuzztest::Arbitrary;
using fuzztest::ElementOf;
using fuzztest::InRange;
using fuzztest::Just;
using fuzztest::OneOf;
using fuzztest::OptionalOf;
using fuzztest::StructOf;
using fuzztest::VectorOf;

// Small vocabularies make ids, names and scaling references collide, so
// duplicates, overlays, diamonds and inheritance cycles are common.
auto DefinitionIds()
{
    return ElementOf<std::string>({"A", "B", "C", "D"});
}

// Mostly valid values with an occasional empty one, so the resolver's
// rejections stay reachable without drowning out the success path.
auto Names()
{
    return ElementOf<std::string>({"", "a", "a", "a", "b", "b", "b", "c", "c", "c"});
}

auto Sources()
{
    return ElementOf<std::string>({"", "s1", "s1", "s1", "s2", "s2", "s2"});
}

auto OptionalNames()
{
    return OptionalOf(Names());
}

auto Addresses()
{
    return OptionalOf(fuzztest::Map([](std::uint32_t value) { return memory::DefinitionAddress{value}; },
                                    ElementOf<std::uint32_t>({0, 1, 0x100, 0xFFFFFFF0, 0xFFFFFFFF})));
}

auto Counts()
{
    return OptionalOf(OneOf(InRange<std::uint32_t>(1, 3), InRange<std::uint32_t>(1, 3), InRange<std::uint32_t>(1, 3),
                            InRange<std::uint32_t>(0, 4)));
}

auto StorageTypes()
{
    return OptionalOf(ElementOf({StorageType::kUint8, StorageType::kInt8, StorageType::kUint16, StorageType::kInt16,
                                 StorageType::kUint24, StorageType::kInt24, StorageType::kUint32, StorageType::kInt32,
                                 StorageType::kFloat, StorageType::kBloblist}));
}

auto Endians()
{
    return ElementOf<std::string>({"", "big", "big", "little", "little"});
}

auto AxisTypes()
{
    return ElementOf<std::string>({"", "X Axis", "Y Axis", "Static X Axis", "Static Y Axis"});
}

// An axis is absent when it equals the default; most generated axes are, and
// a present axis usually takes its map's dimension, so maps get past validation.
auto Axes()
{
    const auto present = StructOf<UnresolvedAxisDefinition>(
        AxisTypes(), Names(), Names(), Names(), StorageTypes(), Endians(), Addresses(),
        OneOf(Just(std::optional<std::uint32_t>{}), Just(std::optional<std::uint32_t>{}), Counts()), OptionalNames(),
        OptionalNames(), Names(), Counts(), Counts(), OptionalNames(), OptionalOf(VectorOf(Names()).WithMaxSize(4)));
    return OneOf(Just(UnresolvedAxisDefinition{}), Just(UnresolvedAxisDefinition{}), present);
}

auto Maps()
{
    return StructOf<UnresolvedCalibrationMap>(
        OptionalNames(), Names(), ElementOf<std::string>({"", "3D", "2D", "1D", "Selectable"}), Names(), Names(),
        Names(), Addresses(), Counts(), Counts(), OptionalOf(Arbitrary<bool>()), OptionalOf(Arbitrary<bool>()),
        OptionalOf(Arbitrary<bool>()), Names(), Names(), Names(), StorageTypes(), Endians(), Counts(), Counts(),
        OptionalNames(), Axes(), Axes());
}

auto Scalings()
{
    return StructOf<UnresolvedScaling>(
        Names(), Names(), OptionalNames(), OptionalNames(), OptionalNames(), Names(), Names(), Names(), Names(),
        StorageTypes(), Endians(),
        VectorOf(fuzztest::PairOf(Names(), ElementOf<std::string>({"", "00", "0102", "AA", "zz"}))).WithMaxSize(3));
}

auto Definitions()
{
    return StructOf<UnresolvedDefinition>(
        ElementOf({DefinitionFormat::kRomRaider, DefinitionFormat::kEcuFlash}), Sources(),
        StructOf<RomIdentity>(DefinitionIds(), Names(), Names(), Addresses()), Arbitrary<RomMetadata>(),
        VectorOf(DefinitionIds()).WithMaxSize(3), VectorOf(Maps()).WithMaxSize(3), VectorOf(Scalings()).WithMaxSize(3));
}

void ExpectResolvedMapsAreConsistent(const RomDefinition& resolved)
{
    // Maps match on id when both have one, else on name.
    const auto same_map = [](const CalibrationMap& left, const CalibrationMap& right)
    { return !left.id.empty() && !right.id.empty() ? left.id == right.id : left.name == right.name; };
    for (std::size_t index = 0; index < resolved.maps.size(); ++index)
    {
        const auto& map = resolved.maps[index];
        EXPECT_FALSE(map.name.empty());
        EXPECT_TRUE(std::ranges::none_of(resolved.maps | std::views::take(index),
                                         [&](const CalibrationMap& earlier) { return same_map(earlier, map); }))
            << "duplicate map " << map.name;
        EXPECT_GE(map.x_size, 1U);
        EXPECT_GE(map.y_size, 1U);
        EXPECT_GE(map.start_position, 1U);
        EXPECT_EQ(map.storage_type == StorageType::kBloblist, map.type == "Selectable");
        for (const auto& [axis, required_size] :
             {std::pair{&map.x_axis, map.x_size}, std::pair{&map.y_axis, map.y_size}})
        {
            if (*axis == AxisDefinition{})
            {
                continue;
            }
            EXPECT_EQ(axis->size, required_size);
            EXPECT_GE(axis->start_position, 1U);
            EXPECT_TRUE(axis->static_data.empty() || axis->static_data.size() == axis->size);
        }
    }
}

// Any inheritance graph of generated definitions resolves or is rejected with
// a reported error. A resolved definition holds the resolver's own contract:
// distinct named maps, positive dimensions and start positions, axes that
// match their map's dimensions, selectable exactly when bloblist, and a
// provenance list that names the root once. Resolving is deterministic.
void ResolvesAnyInheritanceGraphSafely(const UnresolvedDefinition& root,
                                       const std::vector<UnresolvedDefinition>& others)
{
    const DefinitionLoader loader = [&others](DefinitionFormat, std::string_view id) -> Result<UnresolvedDefinition>
    {
        const auto found =
            std::ranges::find(others, id, [](const UnresolvedDefinition& d) { return d.identity.xml_id; });
        if (found == others.end())
        {
            return std::unexpected(Error{.kind = ErrorKind::kInvalidConfig, .detail = "unknown definition"});
        }
        return *found;
    };

    const auto resolved = ResolveDefinition(root, loader);
    if (!resolved.has_value())
    {
        EXPECT_FALSE(resolved.error().detail.empty());
        return;
    }

    EXPECT_EQ(resolved->identity.xml_id, root.identity.xml_id);
    EXPECT_EQ(std::ranges::count(resolved->resolved_definition_ids, root.identity.xml_id), 1);
    ExpectResolvedMapsAreConsistent(*resolved);

    const auto again = ResolveDefinition(root, loader);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*resolved, *again);
}
FUZZ_TEST(DefinitionResolverFuzz, ResolvesAnyInheritanceGraphSafely)
    .WithDomains(Definitions(), VectorOf(Definitions()).WithMaxSize(4));
} // namespace
} // namespace fastecu::definition
