#include <QtTest>
#include "protocol/imut_dma_init.h"
#include "scripted_kline_transport.h"
#include "test_init.h"
using namespace mutdma;
class TestInit : public QObject { Q_OBJECT
private slots:
    void already_in_mode_just_sets_baud() {
        ScriptedKlineTransport t;
        AlreadyInMode init(125000);
        QVERIFY(init.wake(t));
        QVERIFY(t.scriptConsumed());   // no writes expected
    }
};
int run_test_init(int argc, char** argv) {
    TestInit t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_init.moc"
