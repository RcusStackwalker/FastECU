#include "src/backend/config/car_model_catalog.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

#include <cstdlib>
#include <cstring>
#include <array>
#include <fstream>
#include <map>
#include <ranges>
#include <sstream>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

using fastecu::ErrorKind;
using fastecu::InMemoryFileRepository;
using fastecu::Result;
using fastecu::Status;
using fastecu::config::CarModelCatalog;
using fastecu::config::CarModelEntry;
using fastecu::config::ConfigPaths;
using fastecu::config::find_car_model_by_protocol_name;
using fastecu::config::load_car_model_catalog;
using fastecu::config::ProtocolCatalog;
using fastecu::config::ProtocolEntry;
using fastecu::config::resolve_car_models;
using fastecu::config::ResolvedCarModel;

namespace
{
ConfigPaths test_paths()
{
    ConfigPaths p;
    p.protocols_file = "protocols.cfg";
    return p;
}

// <car_models> is a sibling of <protocols>, not nested inside it -- both
// sections present here, matching the real file's shape, so a parser that
// mistakenly looked for <car_model> inside <protocols> (or vice versa)
// would fail this fixture.
const char *kFixture = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
    <protocols>
        <protocol name="sub_ecu_denso_sh7055_densocan" alias="SH7055 DensoCAN">
            <ecu>Denso SH7055</ecu>
            <mcu>SH7055</mcu>
        </protocol>
    </protocols>
    <car_models>
        <car_model>
            <make>Subaru</make>
            <model>Impreza</model>
            <version>2.0 WRX STi</version>
            <type>EJ20</type>
            <kw>220</kw>
            <hp>300</hp>
            <fuel>Petrol</fuel>
            <year>2008</year>
            <protocol>sub_ecu_denso_sh7055_densocan</protocol>
        </car_model>
        <car_model>
            <make>Subaru</make>
            <model>Legacy</model>
            <version>2.0 A/T</version>
            <type>EJ20</type>
            <kw>195</kw>
            <hp>265</hp>
            <fuel>Petrol</fuel>
            <year>1990</year>
            <protocol>sub_ecu_unisia_jecs_92</protocol>
        </car_model>
        <car_model>
            <make>Mitsubishi</make>
            <model>Colt</model>
            <version>Ralliart</version>
            <type>4G93T</type>
            <kw>110</kw>
            <hp>150</hp>
            <fuel>Petrol</fuel>
            <year>2005</year>
            <protocol>mmc_ecu_hitachi_m32r_kline</protocol>
        </car_model>
    </car_models>
</config>
)";
} // namespace

TEST(LoadCarModelCatalog, ParsesEveryFieldOfEachEntry)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    repo.files[paths.protocols_file] = std::vector<std::uint8_t>(kFixture, kFixture + strlen(kFixture));

    auto catalog = load_car_model_catalog(paths, repo);

    ASSERT_TRUE(catalog.has_value());
    ASSERT_EQ(catalog->size(), 3u);

    const auto& first = (*catalog)[0];
    EXPECT_EQ(first.make, "Subaru");
    EXPECT_EQ(first.model, "Impreza");
    EXPECT_EQ(first.version, "2.0 WRX STi");
    EXPECT_EQ(first.type, "EJ20");
    EXPECT_EQ(first.kw, "220");
    EXPECT_EQ(first.hp, "300");
    EXPECT_EQ(first.fuel, "Petrol");
    EXPECT_EQ(first.year, "2008");
    EXPECT_EQ(first.protocol_name, "sub_ecu_denso_sh7055_densocan");

    const auto& third = (*catalog)[2];
    EXPECT_EQ(third.make, "Mitsubishi");
    EXPECT_EQ(third.model, "Colt");
    EXPECT_EQ(third.protocol_name, "mmc_ecu_hitachi_m32r_kline");
}

TEST(LoadCarModelCatalog, DoesNotResolveTheCrossReferenceJoin)
{
    // Confirms this value model stays normalized: a car_model whose
    // <protocol> text doesn't match any real protocol name still parses
    // (protocol_name is stored verbatim, unresolved) -- the join, and any
    // placeholder behavior for an unmatched name, is the adapter's job.
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    repo.files[paths.protocols_file] = std::vector<std::uint8_t>(kFixture, kFixture + strlen(kFixture));

    auto catalog = load_car_model_catalog(paths, repo);

    ASSERT_TRUE(catalog.has_value());
    EXPECT_EQ((*catalog)[1].protocol_name, "sub_ecu_unisia_jecs_92");
}

TEST(LoadCarModelCatalog, MissingFileIsInvalidConfig)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();

    auto catalog = load_car_model_catalog(paths, repo);

    ASSERT_FALSE(catalog.has_value());
    EXPECT_EQ(catalog.error().kind, ErrorKind::InvalidConfig);
}

// Reads the real, checked-in resources/shared/config/protocols.cfg via
// $(location)/env var (see BUILD.bazel's data + env on this test target),
// following protocol_catalog_test.cpp's ParsesTheRealShippedProtocolsFile...
// precedent.
TEST(LoadCarModelCatalog, ParsesTheRealShippedProtocolsFileWithoutError)
{
    const char *path = std::getenv("PROTOCOLS_CFG_PATH");
    ASSERT_NE(path, nullptr) << "PROTOCOLS_CFG_PATH not set -- see this target's data/env wiring "
                                "in src/backend/config/BUILD.bazel";
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open()) << "failed to open protocols.cfg at " << path;
    std::ostringstream contents;
    contents << file.rdbuf();

    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    const std::string text = contents.str();
    repo.files[paths.protocols_file] = std::vector<std::uint8_t>(text.begin(), text.end());

    auto catalog = load_car_model_catalog(paths, repo);

    ASSERT_TRUE(catalog.has_value());
    for (const auto& entry : *catalog)
    {
        EXPECT_FALSE(entry.protocol_name.empty());
    }

    for (const auto& [id, capacity, vendor] : std::to_array<std::tuple<std::string_view, std::string_view, bool>>({
             {"mitsu_ecu_m32r_can", "384KB", false},
             {"mitsu_ecu_m32r_can_vendor_ext", "384KB", true},
             {"mitsu_ecu_m32r_can_512kb", "512KB", false},
             {"mitsu_ecu_m32r_can_vendor_ext_512kb", "512KB", true},
         }))
    {
        const auto colt = std::ranges::find(*catalog, id, &CarModelEntry::protocol_name);
        ASSERT_NE(colt, catalog->end()) << id;
        EXPECT_EQ(colt->make, "Mitsubishi");
        EXPECT_EQ(colt->model, "Colt CZT");
        EXPECT_NE(colt->version.find(capacity), std::string::npos);
        EXPECT_EQ(colt->version.contains("vendor diagnostic extension"), vendor);
    }
}

TEST(ResolveCarModels, JoinsEachCarModelWithItsMatchingProtocol)
{
    ProtocolCatalog protocols;
    ProtocolEntry protocol;
    protocol.protocol_name = "sub_ecu_denso_can";
    protocol.mcu = "SH7058";
    protocol.checksum = "yes";
    protocols.push_back(protocol);

    CarModelCatalog car_models;
    CarModelEntry entry;
    entry.make = "Mitsubishi";
    entry.model = "Colt";
    entry.protocol_name = "sub_ecu_denso_can";
    car_models.push_back(entry);

    std::vector<ResolvedCarModel> resolved = resolve_car_models(protocols, car_models);

    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(resolved[0].make, "Mitsubishi");
    EXPECT_EQ(resolved[0].model, "Colt");
    EXPECT_EQ(resolved[0].protocol_name, "sub_ecu_denso_can");
    ASSERT_TRUE(resolved[0].protocol.has_value());
    EXPECT_EQ(resolved[0].protocol->mcu, "SH7058");
    EXPECT_EQ(resolved[0].protocol->checksum, "yes");
}

TEST(ResolveCarModels, UnmatchedProtocolNameYieldsNullopt)
{
    ProtocolCatalog protocols; // empty
    CarModelCatalog car_models;
    CarModelEntry entry;
    entry.protocol_name = "no_such_protocol";
    car_models.push_back(entry);

    std::vector<ResolvedCarModel> resolved = resolve_car_models(protocols, car_models);

    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_FALSE(resolved[0].protocol.has_value());
}

TEST(ResolveCarModels, DuplicateProtocolNamesResolveToTheFirstMatch)
{
    // Unreachable through load_protocol_catalog, which rejects duplicate
    // protocol names outright (see LoadProtocolCatalog.RejectsDuplicate-
    // ProtocolNames). Pinned only so a hand-built catalog has defined
    // behaviour rather than an accident of loop structure.
    ProtocolCatalog protocols;
    ProtocolEntry first;
    first.protocol_name = "shared_name";
    first.mcu = "FIRST";
    protocols.push_back(first);
    ProtocolEntry second;
    second.protocol_name = "shared_name";
    second.mcu = "SECOND";
    protocols.push_back(second);

    CarModelCatalog car_models;
    CarModelEntry entry;
    entry.protocol_name = "shared_name";
    car_models.push_back(entry);

    std::vector<ResolvedCarModel> resolved = resolve_car_models(protocols, car_models);

    ASSERT_EQ(resolved.size(), 1u);
    ASSERT_TRUE(resolved[0].protocol.has_value());
    EXPECT_EQ(resolved[0].protocol->mcu, "FIRST");
}

// Last-match, deliberately asymmetric with resolve_car_models above: several
// car models legitimately share one protocol, so duplicates here cannot be
// rejected at intake, and which row wins is observable in the vehicle
// open_subaru_rom_file binds.
TEST(FindCarModelByProtocolName, ReturnsTheLastMatchingIndex)
{
    std::vector<ResolvedCarModel> resolved(3);
    resolved[0].protocol_name = "a";
    resolved[1].protocol_name = "b";
    resolved[2].protocol_name = "a";

    std::optional<std::size_t> index = find_car_model_by_protocol_name(resolved, "a");

    ASSERT_TRUE(index.has_value());
    EXPECT_EQ(*index, 2u);
}

TEST(FindCarModelByProtocolName, ReturnsNulloptWhenNoRowMatches)
{
    std::vector<ResolvedCarModel> resolved(2);
    resolved[0].protocol_name = "a";
    resolved[1].protocol_name = "b";

    EXPECT_FALSE(find_car_model_by_protocol_name(resolved, "z").has_value());
}
