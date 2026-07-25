#include "src/backend/config/car_model_catalog.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <gtest/gtest.h>

using fastecu::ErrorKind;
using fastecu::IFileRepository;
using fastecu::Result;
using fastecu::Status;
using fastecu::config::CarModelCatalog;
using fastecu::config::ConfigPaths;
using fastecu::config::load_car_model_catalog;

namespace
{
class InMemoryFileRepository : public IFileRepository
{
  public:
    Result<std::vector<std::uint8_t>> read(std::string_view handle) override
    {
        auto it = files.find(std::string(handle));
        if (it == files.end())
            return fastecu::fail(ErrorKind::InvalidConfig, "no such handle");
        return it->second;
    }
    Status write(std::string_view, std::span<const std::uint8_t>) override
    {
        return {};
    }
    std::map<std::string, std::vector<std::uint8_t>> files;
};

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
    // The real, checked-in file has 63 <car_model> elements as of this
    // writing (verified directly against
    // resources/shared/config/protocols.cfg with
    // `grep -c '<car_model>' resources/shared/config/protocols.cfg`; the
    // originating task description's "64" was an estimate, not a count).
    EXPECT_EQ(catalog->size(), 63u);
    for (const auto& entry : *catalog)
        EXPECT_FALSE(entry.protocol_name.empty());
}
