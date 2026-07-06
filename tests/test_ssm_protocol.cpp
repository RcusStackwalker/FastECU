#include <QtTest>

#include "modules/ssm_protocol.h"
#include "test_ssm_protocol.h"

#include <thread>

class TestSsmProtocol : public QObject
{
    Q_OBJECT

private slots:
    void seed_key_matches_common_denso_vector();
    void seed_key_matches_alternate_table_vector();
    void payload_matches_common_denso_vector();
    void payload_truncates_to_four_byte_boundary();
    void checksum_and_header_match_existing_layout();
    void to_hex_preserves_existing_trailing_space_format();
    void crc32_matches_existing_polynomial_vector();
    void crc32_null_pointer_returns_zero();
    void crc32_first_touch_is_thread_safe();
};

static const uint16_t kCommonSeedTable[16] = {
    0x90A1, 0x2F92, 0xDE3C, 0xCDC0,
    0x1A99, 0x437C, 0xF91B, 0xDB57,
    0x96BA, 0xDE10, 0xFCAF, 0x3F31,
    0xF47F, 0x0BB6, 0x16E9, 0x4645
};

static const uint16_t kAlternateSeedTable[16] = {
    0x8765, 0x2345, 0xA5A5, 0x1357,
    0x2468, 0xACE0, 0x0ACE, 0x55AA,
    0xAA55, 0x1020, 0x3040, 0x5060,
    0x7080, 0x90A0, 0xB0C0, 0xD0E0
};

static const uint8_t kCommonTransformTable[32] = {
    0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8,
    0xA, 0xD, 0x2, 0xB, 0xF, 0x4, 0x0, 0x3,
    0xB, 0x4, 0x6, 0x0, 0xF, 0x2, 0xD, 0x9,
    0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8
};

static const uint8_t kAlternateTransformTable[32] = {
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
    0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF,
    0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8,
    0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0x0
};

static const uint16_t kPayloadTable[4] = {
    0xC85B, 0x32C0, 0xE282, 0x92A0
};

void TestSsmProtocol::seed_key_matches_common_denso_vector()
{
    QCOMPARE(SsmProtocol::calculateSeedKey(QByteArray::fromHex("12345678"),
                                           kCommonSeedTable, kCommonTransformTable),
             QByteArray::fromHex("2daa46dc"));
}

void TestSsmProtocol::seed_key_matches_alternate_table_vector()
{
    QCOMPARE(SsmProtocol::calculateSeedKey(QByteArray::fromHex("89abcdef"),
                                           kAlternateSeedTable, kAlternateTransformTable),
             QByteArray::fromHex("408d111d"));
}

void TestSsmProtocol::payload_matches_common_denso_vector()
{
    QCOMPARE(SsmProtocol::calculatePayload(QByteArray::fromHex("0011223344556677"),
                                           8, kPayloadTable, kCommonTransformTable),
             QByteArray::fromHex("ed9fd931afacd594"));
}

void TestSsmProtocol::payload_truncates_to_four_byte_boundary()
{
    QCOMPARE(SsmProtocol::calculatePayload(QByteArray::fromHex("0011223344"),
                                           5, kPayloadTable, kCommonTransformTable),
             QByteArray::fromHex("ed9fd931"));
}

void TestSsmProtocol::checksum_and_header_match_existing_layout()
{
    const QByteArray payload = QByteArray::fromHex("A800112233");
    QCOMPARE(SsmProtocol::checksum(payload, false), uint8_t(0x0E));
    QCOMPARE(SsmProtocol::checksum(payload, true), uint8_t(0xF2));
    QCOMPARE(SsmProtocol::addHeader(payload, 0xF1, 0x10, false),
             QByteArray::fromHex("8010F105A80011223394"));
}

void TestSsmProtocol::to_hex_preserves_existing_trailing_space_format()
{
    QCOMPARE(SsmProtocol::toHex(QByteArray::fromHex("800102ff")), QString("80 01 02 ff "));
    QCOMPARE(SsmProtocol::toHex(QByteArray()), QString());
}

void TestSsmProtocol::crc32_matches_existing_polynomial_vector()
{
    const QByteArray payload = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    QCOMPARE(SsmProtocol::crc32(reinterpret_cast<const unsigned char *>(payload.constData()),
                                uint32_t(payload.size())),
             uint32_t(0x8FA3DEB3));
}

void TestSsmProtocol::crc32_null_pointer_returns_zero()
{
    QCOMPARE(SsmProtocol::crc32(nullptr, 8), uint32_t(0));
}

void TestSsmProtocol::crc32_first_touch_is_thread_safe()
{
    const QByteArray payload = QByteArray::fromHex("0102030405060708");
    auto fn = [&payload]() {
        return SsmProtocol::crc32(reinterpret_cast<const unsigned char *>(payload.constData()),
                                  uint32_t(payload.size()));
    };

    uint32_t a = 0;
    uint32_t b = 0;
    std::thread ta([&]() { a = fn(); });
    std::thread tb([&]() { b = fn(); });
    ta.join();
    tb.join();
    QCOMPARE(a, uint32_t(0x8AD85CF9));
    QCOMPARE(b, uint32_t(0x8AD85CF9));
}

int run_test_ssm_protocol(int argc, char **argv)
{
    TestSsmProtocol t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_ssm_protocol.moc"
