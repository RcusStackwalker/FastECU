
#include <cstdio>

#include <QCoreApplication>
#include <QWebSocket>
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
    // QWebSocket initializes Qt's default SSL configuration on first use.
    // Measure that platform setup separately from waiting for a remote peer.
    {
        QWebSocket initialize_websocket;
    }
    qInfo() << "Qt WebSocket initialization:" << t.elapsed() << "ms";
    t.restart();
    {
        RemoteSerialBackend remote("local:fastecu-test-nonexistent", "pw");
        QVERIFY(remote.qobject() != nullptr);
    }
    const qint64 elapsed = t.elapsed();
    qInfo() << "Remote backend construction/teardown:" << elapsed << "ms";
    QVERIFY2(elapsed < 2000, "construction/teardown must not block");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);
    TestRemoteBackendSmoke test;
    return QTest::qExec(&test, argc, argv);
}

#include "remote_backend_smoke_test.moc"
