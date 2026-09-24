#include "src/backend/flash/ecu/denso_sh705x_kline_common.h"

#include <array>
#include <cstdint>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{
namespace
{

// flash_ecu_subaru_denso_sh705x_kline_operation.cpp generate_seed_key() /
// generate_ecutek_seed_key(): the same 16-entry key table.
constexpr auto kLegacyKeyTable =
    std::to_array<std::uint16_t>({0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608, 0xAFB8, 0x503D,
                                  0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0});
// encrypt_payload(): the 4-entry key table.
constexpr auto kLegacyEncryptTable = std::to_array<std::uint16_t>({0x7856, 0xCE22, 0xF513, 0x6E86});

const bytes::Bytes kSeed{0x12, 0x34, 0x56, 0x78};

TEST(DensoSh705xKlineCommon, TablesMatchTheLegacyLiterals)
{
    EXPECT_EQ(kDensoSh705xKlineSeedKeyTable, kLegacyKeyTable);
    EXPECT_EQ(kDensoSh705xKlineEncryptTable, kLegacyEncryptTable);
}

TEST(DensoSh705xKlineCommon, StockSeedKeyUsesTheStockTransformation)
{
    EXPECT_EQ(denso_sh705x_kline_stock_seed_key(kSeed),
              SsmProtocol::calculateSeedKey(kSeed, kLegacyKeyTable, SsmProtocol::kIndexTransformationStock));
}

TEST(DensoSh705xKlineCommon, EcutekSeedKeyUsesTheEcutekTransformation)
{
    const bytes::Bytes stock = denso_sh705x_kline_stock_seed_key(kSeed);
    const bytes::Bytes ecutek = denso_sh705x_kline_ecutek_seed_key(kSeed);

    EXPECT_EQ(ecutek, SsmProtocol::calculateSeedKey(kSeed, kLegacyKeyTable, SsmProtocol::kIndexTransformationEcutek));
    EXPECT_NE(ecutek, stock);
}

TEST(DensoSh705xKlineCommon, EncryptPayloadUsesTheKlineTableAndStockTransformation)
{
    const bytes::Bytes payload{0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x00, 0x8D, 0xC8};
    EXPECT_EQ(denso_sh705x_kline_encrypt_payload(payload, 8),
              SsmProtocol::calculatePayload(payload, 8, kLegacyEncryptTable, SsmProtocol::kIndexTransformationStock));
}

} // namespace
} // namespace fastecu::flash
