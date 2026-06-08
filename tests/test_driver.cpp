#include <QtTest>
#include "protocol/mut_dma_driver.h"
#include "protocol/mut_dma_freeform.h"
#include "protocol/mut_dma_codec.h"
#include "protocol/mut_dma_memory.h"
#include "scripted_kline_transport.h"
#include "test_driver.h"
using namespace mutdma;
class TestDriver : public QObject { Q_OBJECT
private slots:
    void free_form_handshake_reaches_streaming() {
        QVector<Channel> ch = { {0x8000,2} };
        ScriptedKlineTransport t;
        AlreadyInMode init(125000);
        // host setup (0xA0,N=1) -> ECU ACK-1 -> host id-list -> ECU ACK-2
        t.expectWrite(buildSetupFrame(0xA0, 1));
        QByteArray ack1 = buildCommandFrame(0xA5, QByteArray(), TRAILER_STD);
        t.queueRead(ack1);
        t.expectWrite(buildIdListFrame(0xA1, ch));
        QByteArray ack2 = buildCommandFrame(0x05, QByteArray(), TRAILER_STD);
        t.queueRead(ack2);
        MutDmaDriver d(t, init);
        QVERIFY(d.startFreeFormLog(ch, 0xA0, 0xA1));
        QVERIFY(d.isStreaming());
        QVERIFY(t.scriptConsumed());
    }
    void write_memory_sends_and_acks() {
        ScriptedKlineTransport t; AlreadyInMode init(125000);
        QByteArray bytes = QByteArray::fromHex("DEAD");
        QVector<QByteArray> frames = buildWriteFrames(0x8010, bytes);
        t.expectWrite(frames.at(0));
        t.queueRead(buildCommandFrame(0x87, QByteArray::fromHex("8000"), TRAILER_STD)); // echo ack
        MutDmaDriver d(t, init);
        QVERIFY(d.writeMemory(0x8010, bytes));
        QVERIFY(t.scriptConsumed());
    }
    void poll_decodes_stream_frame() {
        QVector<Channel> ch = { {0x8000,2} };
        ScriptedKlineTransport t; AlreadyInMode init(125000);
        // one streamed frame: [0x51][12 34][csum][0x0D]
        QByteArray fr; fr.append(char(0x51)); fr.append(QByteArray::fromHex("1234"));
        fr.append(char(sum8(fr,0,fr.size()))); fr.append(char(TRAILER_STD));
        t.queueRead(fr);
        MutDmaDriver d(t, init);
        d.setChannelsForTest(ch);
        QVector<quint32> v = d.pollOnce(50);
        QCOMPARE(v.size(), 1); QCOMPARE(v.at(0), quint32(0x1234));
    }
};
int run_test_driver(int argc, char** argv) {
    TestDriver t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_driver.moc"
