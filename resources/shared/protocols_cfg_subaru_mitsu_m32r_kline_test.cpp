#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

TEST(ProtocolsCfgSubaruMitsuM32rKline, DeclaresPortableOperationAndMcuContract)
{
    const char *path = std::getenv("PROTOCOLS_CFG_PATH");
    ASSERT_NE(path, nullptr);
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    const auto start = text.find("<protocol name=\"sub_ecu_mitsu_m32r_kline\">");
    ASSERT_NE(start, std::string::npos);
    const auto end = text.find("</protocol>", start);
    ASSERT_NE(end, std::string::npos);
    const std::string entry = text.substr(start, end - start);
    EXPECT_NE(entry.find("<mcu>M32R_512KB_4blocks</mcu>"), std::string::npos);
    EXPECT_NE(entry.find("<read>yes</read>"), std::string::npos);
    EXPECT_NE(entry.find("<test_write>no</test_write>"), std::string::npos);
    EXPECT_NE(entry.find("<write>yes</write>"), std::string::npos);
}
