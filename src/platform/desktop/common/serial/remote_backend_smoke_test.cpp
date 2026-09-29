#include "src/platform/desktop/common/testing/core_application_environment.h"

#include <cstdio>

#include <QCoreApplication>
#include <QWebSocket>
#include <gtest/gtest.h>
#include "remote_serial_backend.h"

// The remote path has no automated call-level tests (spec risk note: kept a
// strictly mechanical wrap + manual smoke test before release). This suite
// pins the only things that can be checked headlessly: construction against
// an unreachable peer neither blocks nor crashes, and teardown is clean.
class TestRemoteBackendSmoke : public ::testing::Test
{

  public:
};

TEST_F(TestRemoteBackendSmoke, constructAndDestroy_localPeer_noBlockNoCrash)
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
        ASSERT_TRUE(remote.qobject() != nullptr);
    }
    const qint64 elapsed = t.elapsed();
    qInfo() << "Remote backend construction/teardown:" << elapsed << "ms";
    ASSERT_TRUE(elapsed < 2000) << "construction/teardown must not block";
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);
    return RUN_ALL_TESTS();
}
