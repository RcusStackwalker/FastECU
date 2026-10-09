#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <QStringList>
#include <gtest/gtest.h>

#include <memory>

#include "src/platform/desktop/common/serial/desktop_serial_factory.h"
#include "src/platform/desktop/common/serial/remote/remote_serial_backend.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

#include "src/platform/desktop/common/serial/testing/log_sink/recording_log_sink.h"

TEST(DesktopSerialFactoryTest, directConnectionBuildsTheDirectBackend)
{
    const auto factory = MakeSerialBackendFactory(DirectSerial{});
    ASSERT_TRUE(factory);
    const std::unique_ptr<SerialBackend> backend{factory()};
    ASSERT_TRUE(dynamic_cast<SerialPortActionsDirect *>(backend.get()) != nullptr);
}

// An unreachable peer: RemoteSerialBackend's constructor does not block
// on it (see remote_backend_smoke_test.cpp).
TEST(DesktopSerialFactoryTest, remoteConnectionBuildsTheRemoteBackend)
{
    const auto factory = MakeSerialBackendFactory(RemoteSerial{"local:fastecu-test-nonexistent", "pw"});
    ASSERT_TRUE(factory);
    const std::unique_ptr<SerialBackend> backend{factory()};
    ASSERT_TRUE(dynamic_cast<RemoteSerialBackend *>(backend.get()) != nullptr);
}

TEST(DesktopSerialFactoryTest, everyLogLevelReachesTheSink)
{
    RecordingLogSink sink;
    const OwnedSerialPortActions serial = MakeSerialPortActions(DirectSerial{}, sink);
    ASSERT_TRUE(serial != nullptr);
    sink.messages.clear();

    emit serial->LOG_E("error", false, false);
    emit serial->LOG_W("warning", false, false);
    emit serial->LOG_I("info", false, false);
    emit serial->LOG_D("debug", false, false);

    ASSERT_EQ(sink.messages, (QStringList{"error", "warning", "info", "debug"}));
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
