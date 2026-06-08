#include <QtTest>
#include "protocol/mut_dma_memory.h"
#include "protocol/mut_dma_codec.h"
#include "test_memory.h"
using namespace mutdma;
class TestMemory : public QObject { Q_OBJECT
private slots:
    void write_frame_single() {
        QByteArray bytes = QByteArray::fromHex("DEAD");
        QVector<QByteArray> frames = buildWriteFrames(0x8010, bytes);
        QCOMPARE(frames.size(), 1);
        const QByteArray& f = frames.at(0);
        QCOMPARE(f.size(), FRAME_LEN);
        QCOMPARE(quint8(f.at(0)), quint8(0x87));        // cmd
        QCOMPARE(quint8(f.at(1)), quint8(0x00));        // sub-selector hi (0x0003)
        QCOMPARE(quint8(f.at(2)), quint8(0x03));        // sub-selector lo = write arbitrary
        QCOMPARE(quint8(f.at(3)), quint8(0x80));        // addr hi
        QCOMPARE(quint8(f.at(4)), quint8(0x10));        // addr lo
        QCOMPARE(quint8(f.at(5)), quint8(0x02));        // size
        QCOMPARE(quint8(f.at(6)), quint8(0xDE));        // data
        QCOMPARE(quint8(f.at(7)), quint8(0xAD));
        QVERIFY(verifyFrame(f));
    }
    void write_chunks_large_payload() {
        QByteArray bytes(100, char(0x5A));              // > one frame's data capacity
        QVector<QByteArray> frames = buildWriteFrames(0x8000, bytes);
        QVERIFY(frames.size() >= 3);
        int total = 0; for (const QByteArray& f : frames) total += quint8(f.at(5));
        QCOMPARE(total, bytes.size());                  // all bytes accounted for
    }
};
int run_test_memory(int argc, char** argv) {
    TestMemory t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_memory.moc"
