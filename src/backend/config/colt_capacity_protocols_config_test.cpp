#include "src/backend/config/car_model_catalog.h"
#include "src/backend/config/protocol_catalog.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using fastecu::InMemoryFileRepository;
using fastecu::config::CarModelCatalog;
using fastecu::config::ConfigPaths;
using fastecu::config::load_car_model_catalog;
using fastecu::config::load_protocol_catalog;
using fastecu::config::ProtocolCatalog;
using fastecu::config::ProtocolEntry;
using testing::HasSubstr;

namespace
{
struct ColtProtocolExpectation
{
    std::string_view id;
    std::string_view mcu;
    std::string_view version_text;
};

const std::array<ColtProtocolExpectation, 4> kColtProtocols = {{
    {"mitsu_ecu_m32r_can", "M32R_384KB_1block", "Z37A 5MT"},
    {"mitsu_ecu_m32r_can_vendor_ext", "M32R_384KB_1block", "vendor diagnostic extension"},
    {"mitsu_ecu_m32r_can_512kb", "M32R_512KB_1block", "512KB"},
    {"mitsu_ecu_m32r_can_vendor_ext_512kb", "M32R_512KB_1block",
     "vendor diagnostic extension"},
}};

std::string load_shipped_config()
{
    const char *path = std::getenv("PROTOCOLS_CFG_PATH");
    EXPECT_NE(path, nullptr) << "PROTOCOLS_CFG_PATH not set -- see target data/env wiring";
    if (path == nullptr)
    {
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open()) << "failed to open protocols.cfg at " << path;
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

ConfigPaths test_paths()
{
    ConfigPaths paths;
    paths.protocols_file = "protocols.cfg";
    return paths;
}
} // namespace

TEST(ColtCapacityProtocolsConfig, DefinesFourSelectableCapacityProtocols)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    const std::string config = load_shipped_config();
    ASSERT_FALSE(config.empty());
    repo.files[paths.protocols_file] = std::vector<std::uint8_t>(config.begin(), config.end());

    auto protocols = load_protocol_catalog(paths, repo);
    auto car_models = load_car_model_catalog(paths, repo);

    ASSERT_TRUE(protocols.has_value());
    ASSERT_TRUE(car_models.has_value());
    for (const ColtProtocolExpectation& expected : kColtProtocols)
    {
        const auto protocol = std::ranges::find(*protocols, expected.id,
                                                &ProtocolEntry::protocol_name);
        ASSERT_NE(protocol, protocols->end()) << expected.id;
        EXPECT_EQ(protocol->mcu, expected.mcu) << expected.id;
        EXPECT_EQ(protocol->read, "yes") << expected.id;
        EXPECT_EQ(protocol->write, "yes") << expected.id;
        EXPECT_EQ(protocol->test_write, "no") << expected.id;
        EXPECT_EQ(protocol->flash_transport, "iso15765") << expected.id;

        const auto car_model = std::ranges::find(*car_models, expected.id,
                                                 &fastecu::config::CarModelEntry::protocol_name);
        ASSERT_NE(car_model, car_models->end()) << expected.id;
        EXPECT_EQ(car_model->make, "Mitsubishi") << expected.id;
        EXPECT_THAT(car_model->model, HasSubstr("Colt")) << expected.id;
        EXPECT_THAT(car_model->version, HasSubstr(expected.version_text)) << expected.id;
        if (expected.id.ends_with("_512kb"))
        {
            EXPECT_THAT(car_model->version, HasSubstr("512KB")) << expected.id;
        }
    }
}
