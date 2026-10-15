#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h"

#include "src/algorithms/protocol/bytes.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

// Literal vectors were calculated independently; they are regression evidence, not hardware qualification.

TEST(SubaruTcuCvtMitsuCanCommonTest, SeedKeyMatchesFixedVectors)
{
    EXPECT_THAT(TcuCvtMitsuSeedKey(bytes::Bytes{0x00, 0x00, 0x00, 0x00}),
                testing::ElementsAreArray(bytes::Bytes{0xc2, 0x61, 0x58, 0x3a}));
    EXPECT_THAT(TcuCvtMitsuSeedKey(bytes::Bytes{0x11, 0x22, 0x33, 0x44}),
                testing::ElementsAreArray(bytes::Bytes{0x12, 0xb6, 0xce, 0xba}));
    EXPECT_THAT(TcuCvtMitsuSeedKey(bytes::Bytes{0xff, 0xff, 0xff, 0xff}),
                testing::ElementsAreArray(bytes::Bytes{0xe3, 0xa0, 0x6c, 0x00}));
}

TEST(SubaruTcuCvtMitsuCanCommonTest, SeedKeyPreservesShortAndTrailingInputBehavior)
{
    EXPECT_THAT(TcuCvtMitsuSeedKey(bytes::Bytes{}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuSeedKey(bytes::Bytes{0x11}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuSeedKey(bytes::Bytes{0x11, 0x22}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuSeedKey(bytes::Bytes{0x11, 0x22, 0x33}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuSeedKey(bytes::Bytes{0x11, 0x22, 0x33, 0x44, 0xaa, 0xbb}),
                testing::ElementsAreArray(bytes::Bytes{0x12, 0xb6, 0xce, 0xba}));
}

TEST(SubaruTcuCvtMitsuCanCommonTest, EncryptRomMatchesFixedVectors)
{
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{0x00, 0x00, 0x00, 0x00}),
                testing::ElementsAreArray(bytes::Bytes{0xf6, 0x29, 0xa3, 0x06}));
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}),
                testing::ElementsAreArray(bytes::Bytes{0xd7, 0x8e, 0x41, 0xf9, 0x7a, 0xb5, 0x9e, 0x6d}));
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                                   0x0b, 0x0c, 0x0d, 0x0e, 0x0f}),
                testing::ElementsAreArray(bytes::Bytes{0x76, 0xf9, 0xec, 0x8b, 0x7d, 0x53, 0x6f, 0xbf, 0x50, 0xe5, 0x3f,
                                                       0x61, 0x16, 0xc7, 0xd5, 0x5c}));
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{0xff, 0xff, 0xff, 0xff}),
                testing::ElementsAreArray(bytes::Bytes{0x68, 0xfc, 0x4e, 0x1c}));
}

TEST(SubaruTcuCvtMitsuCanCommonTest, DecryptPageMatchesFixedVectors)
{
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{0xf6, 0x29, 0xa3, 0x06}),
                testing::ElementsAreArray(bytes::Bytes{0x00, 0x00, 0x00, 0x00}));
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{0xd7, 0x8e, 0x41, 0xf9, 0x7a, 0xb5, 0x9e, 0x6d}),
                testing::ElementsAreArray(bytes::Bytes{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}));
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{0x76, 0xf9, 0xec, 0x8b, 0x7d, 0x53, 0x6f, 0xbf, 0x50, 0xe5, 0x3f,
                                                    0x61, 0x16, 0xc7, 0xd5, 0x5c}),
                testing::ElementsAreArray(bytes::Bytes{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                                                       0x0b, 0x0c, 0x0d, 0x0e, 0x0f}));
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{0x68, 0xfc, 0x4e, 0x1c}),
                testing::ElementsAreArray(bytes::Bytes{0xff, 0xff, 0xff, 0xff}));
}

TEST(SubaruTcuCvtMitsuCanCommonTest, PayloadOperationsPreservePartialWordBehavior)
{
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{0x00}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{0x00, 0x11}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{0x00, 0x11, 0x22}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{0x00}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{0x00, 0x11}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{0x00, 0x11, 0x22}), testing::ElementsAreArray(bytes::Bytes{}));
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::Bytes{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}),
                testing::ElementsAreArray(bytes::Bytes{0xd7, 0x8e, 0x41, 0xf9}));
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::Bytes{0xd7, 0x8e, 0x41, 0xf9, 0x7a, 0xb5, 0x9e}),
                testing::ElementsAreArray(bytes::Bytes{0x00, 0x11, 0x22, 0x33}));
}

TEST(SubaruTcuCvtMitsuCanCommonTest, TransformsOnlyTheInputViewWithoutMutation)
{
    const auto plain = bytes::Bytes{0xaa, 0xbb, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0xcc, 0xdd};
    const auto plain_copy = plain;
    EXPECT_THAT(TcuCvtMitsuEncryptRom(bytes::ByteView(plain).subspan(2, 8)),
                testing::ElementsAreArray(bytes::Bytes{0xd7, 0x8e, 0x41, 0xf9, 0x7a, 0xb5, 0x9e, 0x6d}));
    EXPECT_EQ(plain, plain_copy);
    const auto cipher = bytes::Bytes{0xaa, 0xbb, 0xd7, 0x8e, 0x41, 0xf9, 0x7a, 0xb5, 0x9e, 0x6d, 0xcc, 0xdd};
    const auto cipher_copy = cipher;
    EXPECT_THAT(TcuCvtMitsuDecryptPage(bytes::ByteView(cipher).subspan(2, 8)),
                testing::ElementsAreArray(bytes::Bytes{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}));
    EXPECT_EQ(cipher, cipher_copy);
}

} // namespace
} // namespace fastecu::flash
