#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

TEST(ProtocolsCfgSubaruHitachiM32rKline, DeclaresBothPortableTargets)
{
    const char *path = std::getenv("PROTOCOLS_CFG_PATH");
    ASSERT_NE(path, nullptr);
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    for (const std::string protocol : {"sub_ecu_hitachi_m32r_kline",
                                       "sub_ecu_hitachi_m32r_kline_recovery"})
    {
        const auto start = text.find("<protocol name=\"" + protocol + "\">");
        ASSERT_NE(start, std::string::npos);
        const auto end = text.find("</protocol>", start);
        ASSERT_NE(end, std::string::npos);
        const std::string entry = text.substr(start, end - start);
        EXPECT_NE(entry.find("<mcu>M32R_512KB_1block</mcu>"), std::string::npos);
        EXPECT_NE(entry.find("<read>yes</read>"), std::string::npos);
        EXPECT_NE(entry.find("<test_write>no</test_write>"), std::string::npos);
        EXPECT_NE(entry.find("<write>yes</write>"), std::string::npos);
        EXPECT_NE(entry.find("<flash_transport>K-Line</flash_transport>"), std::string::npos);
    }
}
