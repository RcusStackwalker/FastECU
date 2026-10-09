#include "src/platform/desktop/common/testing/core_application_environment.h"

#include <cstdio>

#include <QCoreApplication>
#include <QWebSocket>
#include <gtest/gtest.h>
#include "src/platform/desktop/common/serial/remote/remote_serial_backend.h"

// The remote path has no automated call-level tests (spec risk note: kept a
// strictly mechanical wrap + manual smoke test before release). This suite
// pins the only things that can be checked headlessly: construction against
// an unreachable peer neither blocks nor crashes, and teardown is clean.
TEST(TestRemoteBackendSmoke, constructAndDestroy_localPeer_noBlockNoCrash)
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

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment(
        []
        {
            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
        }));
} // namespace
