#include "src/backend/config/protocol_catalog.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include <gtest/gtest.h>

using fastecu::ErrorKind;
using fastecu::InMemoryFileRepository;
using fastecu::Result;
using fastecu::Status;
using fastecu::config::ConfigPaths;
using fastecu::config::load_protocol_catalog;
using fastecu::config::ProtocolCatalog;

namespace
{
ConfigPaths test_paths()
{
    ConfigPaths p;
    p.protocols_file = "protocols.cfg";
    return p;
}

const char *kFixture = R"(<?xml version="1.0" encoding="UTF-8"?>
<config name="FastECU" version="0.0-dev0">
    <protocols>
        <protocol name="sub_ecu_denso_sh7055_densocan" alias="SH7055 DensoCAN">
            <ecu>Denso SH7055</ecu>
            <mcu>SH7055</mcu>
            <mode>OBD2</mode>
            <checksum>yes</checksum>
            <read>yes</read>
            <test_write>yes</test_write>
            <write>yes</write>
            <flash_transport>CAN</flash_transport>
            <log_transport>K-Line</log_transport>
            <log_protocol>SSM</log_protocol>
            <ecu_id_ascii>no</ecu_id_ascii>
            <ecu_id_addr>0x0</ecu_id_addr>
            <ecu_id_length>0</ecu_id_length>
            <cal_id_ascii>yes</cal_id_ascii>
            <cal_id_addr>0x2000</cal_id_addr>
            <cal_id_length>8</cal_id_length>
            <kernel>ssmk_can_sh7055.bin</kernel>
            <kernel_addr>0xFFFF6004</kernel_addr>
            <description>Subaru Forester/Impreza/Legacy SH7055 DensoCAN bootloader</description>
        </protocol>
        <protocol name="sub_ecu_denso_sh7058_densocan">
            <ecu>Denso SH7058</ecu>
            <mcu>SH7058</mcu>
            <mode>OBD2</mode>
            <checksum>yes</checksum>
            <read>yes</read>
            <test_write>yes</test_write>
            <write>yes</write>
            <flash_transport>CAN</flash_transport>
            <log_transport>K-Line</log_transport>
            <log_protocol>SSM</log_protocol>
            <cal_id_ascii>yes</cal_id_ascii>
            <cal_id_addr>0x2000</cal_id_addr>
            <cal_id_length>8</cal_id_length>
            <kernel>ssmk_can_sh7058.bin</kernel>
            <kernel_addr>0xFFFF3000</kernel_addr>
            <description>Subaru Forester/Impreza/Legacy SH7058 DensoCAN bootloader</description>
        </protocol>
    </protocols>
</config>
)";
} // namespace

TEST(LoadProtocolCatalog, ParsesEveryFieldOfFirstProtocol)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    repo.files[paths.protocols_file] = std::vector<std::uint8_t>(kFixture, kFixture + strlen(kFixture));

    auto catalog = load_protocol_catalog(paths, repo);

    ASSERT_TRUE(catalog.has_value());
    ASSERT_EQ(catalog->size(), 2u);
    const auto& first = (*catalog)[0];
    EXPECT_EQ(first.protocol_name, "sub_ecu_denso_sh7055_densocan");
    EXPECT_EQ(first.alias, "SH7055 DensoCAN");
    EXPECT_EQ(first.ecu, "Denso SH7055");
    EXPECT_EQ(first.mcu, "SH7055");
    EXPECT_EQ(first.mode, "OBD2");
    EXPECT_EQ(first.checksum, "yes");
    EXPECT_EQ(first.read, "yes");
    EXPECT_EQ(first.test_write, "yes");
    EXPECT_EQ(first.write, "yes");
    EXPECT_EQ(first.flash_transport, "CAN");
    EXPECT_EQ(first.log_transport, "K-Line");
    EXPECT_EQ(first.log_protocol, "SSM");
    EXPECT_EQ(first.ecu_id_ascii, "no");
    EXPECT_EQ(first.ecu_id_addr, "0x0");
    EXPECT_EQ(first.ecu_id_length, "0");
    EXPECT_EQ(first.cal_id_ascii, "yes");
    EXPECT_EQ(first.cal_id_addr, "0x2000");
    EXPECT_EQ(first.cal_id_length, "8");
    EXPECT_EQ(first.kernel, "ssmk_can_sh7055.bin");
    EXPECT_EQ(first.kernel_addr, "0xFFFF6004");
    EXPECT_EQ(first.description, "Subaru Forester/Impreza/Legacy SH7055 DensoCAN bootloader");
}

TEST(LoadProtocolCatalog, MissingOptionalFieldsDefaultToLegacyDefaults)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    repo.files[paths.protocols_file] = std::vector<std::uint8_t>(kFixture, kFixture + strlen(kFixture));

    auto catalog = load_protocol_catalog(paths, repo);

    ASSERT_TRUE(catalog.has_value());
    const auto& second = (*catalog)[1];
    // Matches legacy read_protocols_file's Qt
    // QDomElement::attribute(name, default) defaulting (file_actions.cpp:
    // 1146/1148): absent name/alias attributes read as the literal strings
    // "No name"/"No alias", not empty string.
    EXPECT_EQ(second.alias, "No alias"); // no alias attribute on this <protocol>
    EXPECT_EQ(second.ecu_id_ascii, "");  // no <ecu_id_ascii> element at all
    EXPECT_EQ(second.ecu_id_addr, "");
}

TEST(LoadProtocolCatalog, ChecksumPreservesRawTextIncludingNonBooleanValues)
{
    // Real shipped protocols.cfg entries (e.g. sub_ecu_mitsu_m32r_kline) use
    // "n/a" for <checksum>/<read>, not just "yes"/"no". A bool-typed field
    // would collapse "n/a" into false/"no", which is observable downstream:
    // legacy branches on "n/a" specifically for both RomInfo text
    // (file_actions.cpp:1795) and whether the checksum-module-missing
    // warning dialog fires at flash time (file_actions.cpp:2347). This test
    // pins that the raw text -- not a lossy bool -- is what's stored.
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();
    std::string xml =
        R"(<?xml version="1.0"?><config name="FastECU" version="x"><protocols>)"
        R"(<protocol name="p_na"><checksum>n/a</checksum><read>n/a</read></protocol>)"
        R"(</protocols></config>)";
    repo.files[paths.protocols_file] = std::vector<std::uint8_t>(xml.begin(), xml.end());

    auto catalog = load_protocol_catalog(paths, repo);

    ASSERT_TRUE(catalog.has_value());
    ASSERT_EQ(catalog->size(), 1u);
    EXPECT_EQ((*catalog)[0].checksum, "n/a");
    EXPECT_EQ((*catalog)[0].read, "n/a");
}

TEST(LoadProtocolCatalog, MissingFileIsInvalidConfig)
{
    InMemoryFileRepository repo;
    ConfigPaths paths = test_paths();

    auto catalog = load_protocol_catalog(paths, repo);

    ASSERT_FALSE(catalog.has_value());
    EXPECT_EQ(catalog.error().kind, ErrorKind::InvalidConfig);
}

// Reads the real, checked-in resources/shared/config/protocols.cfg via
// $(location)/env var (see BUILD.bazel's data + env on this test target),
// following the same convention already established by
// tests/test_protocols_cfg_eeprom_capabilities.cpp -- not a hardcoded
// relative path, which would only happen to work if run from the runfiles
// root and would silently pass-by-accident (or fail confusingly) otherwise.
TEST(LoadProtocolCatalog, ParsesTheRealShippedProtocolsFileWithoutError)
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

    auto catalog = load_protocol_catalog(paths, repo);

    ASSERT_TRUE(catalog.has_value());
    // The real, checked-in file has 61 <protocol> elements as of this
    // writing (verified directly against resources/shared/config/protocols.cfg;
    // an earlier estimate of "~130" conflated the <protocol> count with the
    // combined <protocol> + <car_model> element count in the same file).
    EXPECT_GT(catalog->size(), 50u);
    for (const auto& entry : *catalog)
        EXPECT_FALSE(entry.protocol_name.empty());
}
