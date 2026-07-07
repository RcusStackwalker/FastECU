#include <QtTest>
#include "protocol/mut_dma_memory.h"
#include "protocol/mut_dma_codec.h"
#include "protocol/mut_dma_freeform.h"
#include "test_memory.h"
using namespace mutdma;
class TestMemory : public QObject
{
    Q_OBJECT
  private slots:
    void write_frame_single()
    {
        const bytes::Bytes data = {0xDE, 0xAD};
        const std::vector<MutDmaFrame> frames = buildWriteFrames(0x8010, data);
        QCOMPARE(frames.size(), std::size_t(1));
        const MutDmaFrame& f = frames.at(0);
        QCOMPARE(static_cast<int>(f.size()), FRAME_LEN);
        QCOMPARE(f[0], bytes::Byte(0x87)); // cmd
        QCOMPARE(f[1], bytes::Byte(0x00)); // sub-selector hi (0x0003)
        QCOMPARE(f[2], bytes::Byte(0x03)); // sub-selector lo = write arbitrary
        QCOMPARE(f[3], bytes::Byte(0x80)); // addr hi
        QCOMPARE(f[4], bytes::Byte(0x10)); // addr lo
        QCOMPARE(f[5], bytes::Byte(0x02)); // size
        QCOMPARE(f[6], bytes::Byte(0xDE)); // data
        QCOMPARE(f[7], bytes::Byte(0xAD));
        QVERIFY(verifyFrame(f));
    }
    void write_chunks_large_payload()
    {
        bytes::Bytes data(100, 0x5A); // > one frame's data capacity
        const std::vector<MutDmaFrame> frames = buildWriteFrames(0x8000, data);
        QVERIFY(frames.size() >= 3);
        int total = 0;
        for (const MutDmaFrame& f : frames)
            total += f[5];
        QCOMPARE(total, static_cast<int>(data.size())); // all bytes accounted for
    }
    void read_plan_one_byte_channels()
    {
        QVector<Channel> ch = planReadChannels(0x8000, 3);
        QCOMPARE(ch.size(), 3);
        QCOMPARE(ch.at(0).id, std::uint16_t(0x8000));
        QCOMPARE(ch.at(0).len, bytes::Byte(1));
        QCOMPARE(ch.at(2).id, std::uint16_t(0x8002));
    }
    void read_reassembles_values()
    {
        QVector<std::uint32_t> vals = {0xDE, 0xAD, 0xBE};
        const bytes::Bytes out = reassembleRead(vals);
        const bytes::Bytes expected = {0xDE, 0xAD, 0xBE};
        QVERIFY(out == expected);
    }
};
int run_test_memory(int argc, char **argv)
{
    TestMemory t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_memory.moc"
