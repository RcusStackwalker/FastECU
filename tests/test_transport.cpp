#include <QtTest>
#include "protocol/qt_bytes.h"
#include "scripted_kline_transport.h"
#include "test_transport.h"
using namespace mutdma;
class TestTransport : public QObject { Q_OBJECT
private slots:
    void scripted_write_then_read() {
        ScriptedKlineTransport t;
        t.expectWrite(QByteArray::fromHex("A0"));
        t.queueRead(QByteArray::fromHex("A5"));
        QCOMPARE(t.setBaud(125000), true);
        QCOMPARE(t.write(bytes::view(QByteArray::fromHex("A0"))), 1);
        QCOMPARE(bytes::toQByteArray(t.read(50, -1)), QByteArray::fromHex("A5"));
        QVERIFY(t.scriptConsumed());
    }
    void scripted_unexpected_write_flags() {
        ScriptedKlineTransport t;
        t.expectWrite(QByteArray::fromHex("A0"));
        t.write(bytes::view(QByteArray::fromHex("BB")));
        QVERIFY(!t.ok());                                 // mismatch recorded
    }
};
int run_test_transport(int argc, char** argv) {
    TestTransport t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_transport.moc"
