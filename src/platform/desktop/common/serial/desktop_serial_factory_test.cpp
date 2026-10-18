#include "src/platform/desktop/common/testing/core_application_environment.h"
#include <QStringList>
#include <gtest/gtest.h>

#include <memory>

#include "src/platform/desktop/common/serial/desktop_serial_factory.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include "src/platform/desktop/common/serial/direct/serial_port_actions_direct.h"

#include "src/platform/desktop/common/serial/testing/log_sink/recording_log_sink.h"

TEST(DesktopSerialFactoryTest, buildsTheDirectBackend)
{
    const auto factory = MakeSerialBackendFactory();
    ASSERT_TRUE(factory);
    const std::unique_ptr<SerialBackend> backend{factory()};
    ASSERT_TRUE(dynamic_cast<SerialPortActionsDirect *>(backend.get()) != nullptr);
}

TEST(DesktopSerialFactoryTest, everyLogLevelReachesTheSink)
{
    RecordingLogSink sink;
    const OwnedSerialPortActions serial = MakeSerialPortActions(sink);
    ASSERT_TRUE(serial != nullptr);
    sink.messages.clear();

    emit serial->logE("error", false, false);
    emit serial->logW("warning", false, false);
    emit serial->logI("info", false, false);
    emit serial->logD("debug", false, false);

    ASSERT_EQ(sink.messages, (QStringList{"error", "warning", "info", "debug"}));
}

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment({}, /*use_96_dpi=*/true));
}
