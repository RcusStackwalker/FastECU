#include <gtest/gtest.h>

#include "modules/ssm_protocol.h"
#include "byte_test_utils.h"

#include <thread>

static const uint16_t kCommonSeedTable[16] = {
    0x90A1, 0x2F92, 0xDE3C, 0xCDC0,
    0x1A99, 0x437C, 0xF91B, 0xDB57,
    0x96BA, 0xDE10, 0xFCAF, 0x3F31,
    0xF47F, 0x0BB6, 0x16E9, 0x4645};

static const uint16_t kAlternateSeedTable[16] = {
    0x8765, 0x2345, 0xA5A5, 0x1357,
    0x2468, 0xACE0, 0x0ACE, 0x55AA,
    0xAA55, 0x1020, 0x3040, 0x5060,
    0x7080, 0x90A0, 0xB0C0, 0xD0E0};

static const uint8_t kCommonTransformTable[32] = {
    0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
    0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
    0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};

static const uint8_t kAlternateTransformTable[32] = {
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
    0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF,
    0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8,
    0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0x0};

static const uint16_t kPayloadTable[4] = {
    0xC85B, 0x32C0, 0xE282, 0x92A0};

static bytes::Bytes fromHex(const char *hex)
{
    return test_bytes::bytesFromHex(hex);
}

TEST(TestSsmProtocol, seed_key_matches_common_denso_vector)
{
    ASSERT_EQ(SsmProtocol::calculateSeedKey(QByteArray::fromHex("12345678"),
                                            kCommonSeedTable, kCommonTransformTable),
              QByteArray::fromHex("2daa46dc"));
}

TEST(TestSsmProtocol, seed_key_matches_common_denso_vector_with_byte_view)
{
    ASSERT_TRUE(SsmProtocol::calculateSeedKey(fromHex("12345678"),
                                              kCommonSeedTable, kCommonTransformTable) == fromHex("2daa46dc"));
}

TEST(TestSsmProtocol, seed_key_matches_alternate_table_vector)
{
    ASSERT_EQ(SsmProtocol::calculateSeedKey(QByteArray::fromHex("89abcdef"),
                                            kAlternateSeedTable, kAlternateTransformTable),
              QByteArray::fromHex("408d111d"));
}

TEST(TestSsmProtocol, payload_matches_common_denso_vector)
{
    ASSERT_EQ(SsmProtocol::calculatePayload(QByteArray::fromHex("0011223344556677"),
                                            8, kPayloadTable, kCommonTransformTable),
              QByteArray::fromHex("ed9fd931afacd594"));
}

TEST(TestSsmProtocol, payload_matches_common_denso_vector_with_byte_view)
{
    ASSERT_TRUE(SsmProtocol::calculatePayload(fromHex("0011223344556677"),
                                              8, kPayloadTable, kCommonTransformTable) == fromHex("ed9fd931afacd594"));
}

TEST(TestSsmProtocol, payload_truncates_to_four_byte_boundary)
{
    ASSERT_EQ(SsmProtocol::calculatePayload(QByteArray::fromHex("0011223344"),
                                            5, kPayloadTable, kCommonTransformTable),
              QByteArray::fromHex("ed9fd931"));
}

TEST(TestSsmProtocol, checksum_and_header_match_existing_layout)
{
    const QByteArray payload = QByteArray::fromHex("A800112233");
    ASSERT_EQ(SsmProtocol::checksum(payload, false), uint8_t(0x0E));
    ASSERT_EQ(SsmProtocol::checksum(payload, true), uint8_t(0xF2));
    ASSERT_EQ(SsmProtocol::addHeader(payload, 0xF1, 0x10, false),
              QByteArray::fromHex("8010F105A80011223394"));
}

TEST(TestSsmProtocol, checksum_and_header_match_existing_layout_with_byte_view)
{
    const bytes::Bytes payload = fromHex("A800112233");
    ASSERT_EQ(SsmProtocol::checksum(payload, false), bytes::Byte(0x0E));
    ASSERT_EQ(SsmProtocol::checksum(payload, true), bytes::Byte(0xF2));
    ASSERT_TRUE(SsmProtocol::addHeader(payload, 0xF1, 0x10, false) == fromHex("8010F105A80011223394"));
}

TEST(TestSsmProtocol, frame_validation_accepts_matching_header_length_and_checksum)
{
    const QByteArray response = SsmProtocol::addHeader(QByteArray::fromHex("EF52"), 0xF0, 0x10);

    ASSERT_TRUE(SsmProtocol::hasValidFrame(response, 0x10, 0xF0));
}

TEST(TestSsmProtocol, frame_validation_rejects_malformed_frames)
{
    const QByteArray response = SsmProtocol::addHeader(QByteArray::fromHex("EF52"), 0xF0, 0x10);
    QByteArray badLength = response;
    badLength[3] = char(0x03);
    QByteArray badChecksum = response;
    badChecksum[badChecksum.size() - 1] = char(0x00);

    ASSERT_TRUE(!SsmProtocol::hasValidFrame(QByteArray::fromHex("8010F0"), 0x10, 0xF0));
    ASSERT_TRUE(!SsmProtocol::hasValidFrame(response, 0x11, 0xF0));
    ASSERT_TRUE(!SsmProtocol::hasValidFrame(response, 0x10, 0xF1));
    ASSERT_TRUE(!SsmProtocol::hasValidFrame(badLength, 0x10, 0xF0));
    ASSERT_TRUE(!SsmProtocol::hasValidFrame(badChecksum, 0x10, 0xF0));
}

TEST(TestSsmProtocol, payload_prefix_checks_validated_payload_start)
{
    const QByteArray response = SsmProtocol::addHeader(QByteArray::fromHex("EF5201"), 0xF0, 0x10);
    QByteArray badChecksum = response;
    badChecksum[badChecksum.size() - 1] = char(0x00);

    ASSERT_TRUE(SsmProtocol::hasPayloadPrefix(response, QByteArray::fromHex("EF52"), 0x10, 0xF0));
    ASSERT_TRUE(!SsmProtocol::hasPayloadPrefix(response, QByteArray::fromHex("EF53"), 0x10, 0xF0));
    ASSERT_TRUE(!SsmProtocol::hasPayloadPrefix(response, QByteArray::fromHex("EF520100"), 0x10, 0xF0));
    ASSERT_TRUE(!SsmProtocol::hasPayloadPrefix(badChecksum, QByteArray::fromHex("EF52"), 0x10, 0xF0));
}

TEST(TestSsmProtocol, to_hex_preserves_existing_trailing_space_format)
{
    ASSERT_EQ(SsmProtocol::toHex(QByteArray::fromHex("800102ff")), QString("80 01 02 ff "));
    ASSERT_EQ(SsmProtocol::toHex(QByteArray()), QString());
}

TEST(TestSsmProtocol, to_hex_preserves_existing_trailing_space_format_with_byte_view)
{
    ASSERT_EQ(SsmProtocol::toHex(fromHex("800102ff")), std::string("80 01 02 ff "));
    ASSERT_EQ(SsmProtocol::toHex(bytes::ByteView()), std::string());
}

TEST(TestSsmProtocol, crc32_matches_existing_polynomial_vector)
{
    const QByteArray payload = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    ASSERT_EQ(SsmProtocol::crc32(reinterpret_cast<const unsigned char *>(payload.constData()),
                                 uint32_t(payload.size())),
              uint32_t(0x8FA3DEB3));
}

TEST(TestSsmProtocol, crc32_matches_existing_polynomial_vector_with_byte_view)
{
    ASSERT_EQ(SsmProtocol::crc32(fromHex("00112233445566778899aabbccddeeff")),
              uint32_t(0x8FA3DEB3));
}

TEST(TestSsmProtocol, crc32_null_pointer_returns_zero)
{
    ASSERT_EQ(SsmProtocol::crc32(nullptr, 8), uint32_t(0));
}

TEST(TestSsmProtocol, crc32_first_touch_is_thread_safe)
{
    const QByteArray payload = QByteArray::fromHex("0102030405060708");
    auto fn = [&payload]()
    {
        return SsmProtocol::crc32(reinterpret_cast<const unsigned char *>(payload.constData()),
                                  uint32_t(payload.size()));
    };

    uint32_t a = 0;
    uint32_t b = 0;
    std::thread ta([&]()
                   { a = fn(); });
    std::thread tb([&]()
                   { b = fn(); });
    ta.join();
    tb.join();
    ASSERT_EQ(a, uint32_t(0x8AD85CF9));
    ASSERT_EQ(b, uint32_t(0x8AD85CF9));
}

TEST(TestSsmProtocol, byte_utilities_append_and_read_big_endian_values)
{
    bytes::Bytes data;
    bytes::appendU16Be(data, 0x1234);
    bytes::appendU24Be(data, 0xABCDEF);
    bytes::appendU32Be(data, 0x10203040);

    ASSERT_TRUE(data == fromHex("1234abcdef10203040"));
    ASSERT_EQ(bytes::readU16Be(data, 0), uint16_t(0x1234));
    ASSERT_EQ(bytes::readU24Be(data, 2), uint32_t(0xABCDEF));
    ASSERT_EQ(bytes::readU32Be(data, 5), uint32_t(0x10203040));
}

TEST(TestSsmProtocol, byte_utilities_return_zero_for_short_reads)
{
    const bytes::Bytes data = fromHex("1234");
    ASSERT_EQ(bytes::readU16Be(data, 1), uint16_t(0));
    ASSERT_EQ(bytes::readU24Be(data, 0), uint32_t(0));
    ASSERT_EQ(bytes::readU32Be(data, 0), uint32_t(0));
}

TEST(TestSsmProtocol, byte_utilities_sum8_wraps)
{
    ASSERT_EQ(bytes::sum8(fromHex("01020304")), bytes::Byte(0x0A));
    ASSERT_EQ(bytes::sum8(bytes::Bytes{0xFF, 0xFF, 0xFF}), bytes::Byte(0xFD));
}
