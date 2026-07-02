#include "test_remote_backend_smoke.h"

#include <QtTest>
#include "remote_serial_backend.h"

// The remote path has no automated call-level tests (spec risk note: kept a
// strictly mechanical wrap + manual smoke test before release). This suite
// pins the only things that can be checked headlessly: construction against
// an unreachable peer neither blocks nor crashes, and teardown is clean.
class TestRemoteBackendSmoke : public QObject
{
    Q_OBJECT
private slots:
    void constructAndDestroy_localPeer_noBlockNoCrash();
};

void TestRemoteBackendSmoke::constructAndDestroy_localPeer_noBlockNoCrash()
{
    QElapsedTimer t;
    t.start();
    {
        RemoteSerialBackend remote("local:fastecu-test-nonexistent", "pw");
        QVERIFY(remote.qobject() != nullptr);
    }
    QVERIFY2(t.elapsed() < 2000, "construction/teardown must not block");
}

int run_test_remote_backend_smoke(int argc, char **argv)
{
    TestRemoteBackendSmoke t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_remote_backend_smoke.moc"
