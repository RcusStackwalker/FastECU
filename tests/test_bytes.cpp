#include <QtTest>

#include <array>
#include <cstdint>

#include "protocol/bytes.h"
#include "test_bytes.h"

class TestBytes : public QObject
{
    Q_OBJECT
  private slots:
    void readU16Le_readsLowByteFirst()
    {
        const std::array<bytes::Byte, 2> data{0x34, 0x12};
        QCOMPARE(bytes::readU16Le(bytes::ByteView(data)), std::uint16_t(0x1234));
    }

    void readU24Le_readsLowByteFirst()
    {
        const std::array<bytes::Byte, 3> data{0x56, 0x34, 0x12};
        QCOMPARE(bytes::readU24Le(bytes::ByteView(data)), std::uint32_t(0x123456));
    }

    void readU32Le_readsLowByteFirst()
    {
        const std::array<bytes::Byte, 4> data{0x78, 0x56, 0x34, 0x12};
        QCOMPARE(bytes::readU32Le(bytes::ByteView(data)), std::uint32_t(0x12345678));
    }

    void readU16Le_respectsOffset()
    {
        const std::array<bytes::Byte, 4> data{0xFF, 0xFF, 0x34, 0x12};
        QCOMPARE(bytes::readU16Le(bytes::ByteView(data), 2), std::uint16_t(0x1234));
    }

    void readLe_outOfBoundsReturnsZero()
    {
        const std::array<bytes::Byte, 1> data{0x12};
        QCOMPARE(bytes::readU16Le(bytes::ByteView(data)), std::uint16_t(0));
        QCOMPARE(bytes::readU24Le(bytes::ByteView(data)), std::uint32_t(0));
        QCOMPARE(bytes::readU32Le(bytes::ByteView(data)), std::uint32_t(0));
    }

    void appendU16Le_appendsLowByteFirst()
    {
        bytes::Bytes out;
        bytes::appendU16Le(out, 0x1234);
        QCOMPARE(out, (bytes::Bytes{0x34, 0x12}));
    }

    void appendU24Le_appendsLowByteFirst()
    {
        bytes::Bytes out;
        bytes::appendU24Le(out, 0x123456);
        QCOMPARE(out, (bytes::Bytes{0x56, 0x34, 0x12}));
    }

    void appendU32Le_appendsLowByteFirst()
    {
        bytes::Bytes out;
        bytes::appendU32Le(out, 0x12345678);
        QCOMPARE(out, (bytes::Bytes{0x78, 0x56, 0x34, 0x12}));
    }

    void writeU16Be_writesAtOffsetWithoutDisturbingRest()
    {
        std::array<bytes::Byte, 4> data{0xAA, 0xAA, 0xAA, 0xAA};
        bytes::writeU16Be(bytes::MutableByteView(data), 1, 0x1234);
        QCOMPARE(data, (std::array<bytes::Byte, 4>{0xAA, 0x12, 0x34, 0xAA}));
    }

    void writeU24Be_writesBigEndianBytes()
    {
        std::array<bytes::Byte, 3> data{};
        bytes::writeU24Be(bytes::MutableByteView(data), 0, 0x123456);
        QCOMPARE(data, (std::array<bytes::Byte, 3>{0x12, 0x34, 0x56}));
    }

    void writeU32Be_writesBigEndianBytes()
    {
        std::array<bytes::Byte, 4> data{};
        bytes::writeU32Be(bytes::MutableByteView(data), 0, 0x12345678);
        QCOMPARE(data, (std::array<bytes::Byte, 4>{0x12, 0x34, 0x56, 0x78}));
    }

    void writeU16Le_writesLittleEndianBytes()
    {
        std::array<bytes::Byte, 2> data{};
        bytes::writeU16Le(bytes::MutableByteView(data), 0, 0x1234);
        QCOMPARE(data, (std::array<bytes::Byte, 2>{0x34, 0x12}));
    }

    void writeU24Le_writesLittleEndianBytes()
    {
        std::array<bytes::Byte, 3> data{};
        bytes::writeU24Le(bytes::MutableByteView(data), 0, 0x123456);
        QCOMPARE(data, (std::array<bytes::Byte, 3>{0x56, 0x34, 0x12}));
    }

    void writeU32Le_writesLittleEndianBytes()
    {
        std::array<bytes::Byte, 4> data{};
        bytes::writeU32Le(bytes::MutableByteView(data), 0, 0x12345678);
        QCOMPARE(data, (std::array<bytes::Byte, 4>{0x78, 0x56, 0x34, 0x12}));
    }

    void writeBe_outOfBoundsIsNoOp()
    {
        std::array<bytes::Byte, 1> data{0xAA};
        bytes::writeU16Be(bytes::MutableByteView(data), 0, 0x1234);
        QCOMPARE(data, (std::array<bytes::Byte, 1>{0xAA}));
    }
};

int run_test_bytes(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestBytes t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_bytes.moc"
