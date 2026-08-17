// Relocated verbatim (step 5c, Task 8) from
// protocolsCfgDeclaresEepromWriteAndTestWriteUnsupported() in the now-deleted
// tests/test_eeprom_ecu_subaru_denso_sh705x_kline_operation_characterization.cpp
// (Task 6). That test never exercised the deleted K-Line operation class --
// it only read the real, checked-in resources/shared/config/protocols.cfg --
// so it survives independent of which operation/executor implementation
// backs each protocol name. Portable: plain file/string parsing, no Qt.
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

using namespace std::literals::string_view_literals;

namespace
{

std::string readProtocolsCfg()
{
    const char *path = std::getenv("PROTOCOLS_CFG_PATH");
    if (path == nullptr || *path == '\0')
    {
        ADD_FAILURE() << "PROTOCOLS_CFG_PATH not set -- see tests/BUILD.bazel's "
                         "test_protocols_cfg_eeprom_capabilities data/env wiring";
        return {};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        ADD_FAILURE() << "failed to open protocols.cfg at " << path;
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

TEST(ProtocolsCfgEepromCapabilitiesTest, DeclaresEepromWriteAndTestWriteUnsupported)
{
    const std::string contents = readProtocolsCfg();
    ASSERT_FALSE(contents.empty());

    for (const auto& name : {"sub_ecu_eeprom_denso_sh7055_kline"sv, "sub_ecu_eeprom_denso_sh7058_kline"sv,
                             "sub_ecu_eeprom_denso_sh7055_densocan"sv, "sub_ecu_eeprom_denso_sh7058_densocan"sv,
                             "sub_ecu_eeprom_denso_sh7058_can"sv, "sub_ecu_eeprom_denso_sh7058_can_diesel"sv})
    {
        SCOPED_TRACE(name);
        const std::string needle = std::format("name=\"{}\"", name);
        const std::size_t nameIndex = contents.find(needle);
        ASSERT_NE(nameIndex, std::string::npos);
        const std::size_t entryEnd = contents.find("</protocol>", nameIndex);
        ASSERT_NE(entryEnd, std::string::npos);
        const std::string entry = contents.substr(nameIndex, entryEnd - nameIndex);
        EXPECT_THAT(entry, testing::HasSubstr("<test_write>no</test_write>"));
        EXPECT_THAT(entry, testing::HasSubstr("<write>no</write>"));
    }
}
