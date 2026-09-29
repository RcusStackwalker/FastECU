#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include "src/platform/desktop/common/testing/signal_recorder.h"

#include <QPointer>
#include <QTimer>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using fastecu::testing::process_events_for;
using fastecu::testing::SignalRecorder;
using fastecu::testing::wait_until;

namespace
{
const auto *const environment = ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment);

TEST(QtTestSupport, ApplicationLivesDuringFixtures)
{
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    EXPECT_FALSE(QCoreApplication::arguments().empty());
}

TEST(QtTestSupport, CapturesImmediateAndWorkerSignalsWithoutEventProcessing)
{
    QObject source;
    SignalRecorder recorder(&source, &QObject::objectNameChanged);
    ASSERT_TRUE(recorder.is_valid());
    source.setObjectName("immediate");
    std::thread worker([&] { source.setObjectName("worker"); });
    worker.join();
    ASSERT_EQ(recorder.count(), 2U);
    const auto records = recorder.snapshot();
    EXPECT_EQ(std::get<0>(records[0]), QString("immediate"));
    EXPECT_EQ(std::get<0>(records[1]), QString("worker"));
}

TEST(QtTestSupport, CapturesQueuedSignals)
{
    QObject source;
    SignalRecorder recorder(&source, &QObject::objectNameChanged);
    QTimer::singleShot(0, &source, [&] { source.setObjectName("queued"); });
    ASSERT_TRUE(wait_until([&] { return recorder.count() == 1; }, 100ms));
    EXPECT_EQ(std::get<0>(recorder.snapshot().front()), QString("queued"));
}

TEST(QtTestSupport, TimesOutAndProcessesDeferredDeletion)
{
    EXPECT_FALSE(wait_until([] { return false; }, 2ms));
    QPointer<QObject> object = new QObject;
    object->deleteLater();
    ASSERT_TRUE(wait_until([&] { return object.isNull(); }, 100ms));
}

TEST(QtTestSupport, RecorderCanDieBeforeSender)
{
    QObject source;
    {
        SignalRecorder recorder(&source, &QObject::objectNameChanged);
        source.setObjectName("first");
        ASSERT_EQ(recorder.count(), 1U);
    }
    source.setObjectName("after destruction");
    process_events_for(2ms);
}
} // namespace
