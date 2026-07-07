#include <QtTest>
#include "protocol/mut_dma_freeform.h"
#include "protocol/mut_dma_codec.h"
#include "test_freeform.h"
using namespace mutdma;
class TestFreeform : public QObject
{
    Q_OBJECT
  private slots:
    void size_descriptor_mapping()
    {
        QCOMPARE(sizeToDescriptor(1), bytes::Byte(0));
        QCOMPARE(sizeToDescriptor(2), bytes::Byte(1));
        QCOMPARE(sizeToDescriptor(4), bytes::Byte(2));
    }
    void setup_frame()
    {
        const MutDmaFrame f = buildSetupFrame(0xA0, 3);
        QCOMPARE(static_cast<int>(f.size()), FRAME_LEN);
        QCOMPARE(f[0], bytes::Byte(0xA0));
        QCOMPARE(f[1], bytes::Byte(3)); // channel count
        QCOMPARE(f[TRAILER_OFFSET], TRAILER_FREEFORM);
        QVERIFY(verifyFrame(f));
    }
    void reqlen_formula()
    {
        QCOMPARE(reqLen(1), ((1 + 3) >> 2) + 1 * 2 + 0x1c); // 1 + 2 + 28 = 31
        QCOMPARE(reqLen(4), ((4 + 3) >> 2) + 4 * 2 + 0x1c); // 1 + 8 + 28 = 37
    }
    void id_list_frame()
    {
        QVector<Channel> ch = {{0x8000, 2}, {0x8004, 1}}; // N=2
        const bytes::Bytes f = buildIdListFrame(0xA1, ch);
        QCOMPARE(static_cast<int>(f.size()), reqLen(2)); // ((2+3)>>2)+4+28 = 1+4+28 = 33
        QCOMPARE(f[0], bytes::Byte(0xA1));               // rate selector
        QCOMPARE(f[1], bytes::Byte(2));                  // count
        // descriptors: ch0=2B->1 at bits[7:6], ch1=1B->0 at bits[5:4] => 0b01000000 = 0x40
        QCOMPARE(f[2], bytes::Byte(0x40));
        // ids start at offset 2 + ceil(N/4) = 2 + 1 = 3, big-endian u16
        QCOMPARE(f[3], bytes::Byte(0x80));
        QCOMPARE(f[4], bytes::Byte(0x00));
        QCOMPARE(f[5], bytes::Byte(0x80));
        QCOMPARE(f[6], bytes::Byte(0x04));
        // checksum at reqLen-2 over bytes [0..reqLen-3]; trailer 0x0D at reqLen-1
        QCOMPARE(f[f.size() - 2], sum8(f, 0, f.size() - 2));
        QCOMPARE(f[f.size() - 1], TRAILER_STD);
    }
    void decode_stream_values()
    {
        QVector<Channel> ch = {{0x8000, 2}, {0x8004, 1}, {0x8008, 4}};
        QCOMPARE(responseDataLength(ch), 2 + 1 + 4);                          // 7
        const bytes::Bytes data = {0x12, 0x34, 0x56, 0x89, 0xAB, 0xCD, 0xEF}; // BE per channel
        QVector<std::uint32_t> v = decodeStreamValues(ch, data);
        QCOMPARE(v.size(), 3);
        QCOMPARE(v.at(0), std::uint32_t(0x1234));
        QCOMPARE(v.at(1), std::uint32_t(0x56));
        QCOMPARE(v.at(2), std::uint32_t(0x89ABCDEF));
    }
};
int run_test_freeform(int argc, char **argv)
{
    TestFreeform t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_freeform.moc"
