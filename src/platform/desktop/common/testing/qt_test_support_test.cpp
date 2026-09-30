#include "src/platform/desktop/common/testing/core_application_environment.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include "src/platform/desktop/common/testing/signal_recorder.h"

#include <QPointer>
#include <QTimer>
#include <gtest/gtest.h>
#include <chrono>
#include <atomic>
#include <thread>

using namespace std::chrono_literals;
using fastecu::testing::process_events_for;
using fastecu::testing::SignalRecorder;
using fastecu::testing::wait_until;

namespace
{
QPointer<QCoreApplication> observed_application;
class ApplicationTeardownObserver : public ::testing::Environment
{
    void TearDown() override
    {
        EXPECT_EQ(QCoreApplication::instance(), nullptr);
        EXPECT_TRUE(observed_application.isNull());
    }
};
// Environments tear down in reverse registration order.
const auto *const teardown_observer = ::testing::AddGlobalTestEnvironment(new ApplicationTeardownObserver);
const auto *const environment = ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment(
    []
    {
        EXPECT_EQ(QCoreApplication::instance(), nullptr);
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    }));

TEST(QtTestSupport, ApplicationLivesDuringFixtures)
{
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    observed_application = QCoreApplication::instance();
    EXPECT_EQ(QCoreApplication::arguments().front(), QString("fastecu-test"));
    EXPECT_TRUE(QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts));
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

TEST(QtTestSupport, SenderCanDieBeforeRecorder)
{
    auto source = std::make_unique<QObject>();
    SignalRecorder recorder(source.get(), &QObject::objectNameChanged);
    source->setObjectName("retained");
    source.reset();
    ASSERT_EQ(recorder.count(), 1U);
    EXPECT_EQ(std::get<0>(recorder.snapshot().front()), QString("retained"));
}

TEST(QtTestSupport, RecorderCanDisconnectWhileWorkerEmits)
{
    QObject source;
    std::atomic<bool> stop{false};
    std::atomic<bool> started{false};
    auto recorder =
        std::make_unique<SignalRecorder<decltype(&QObject::objectNameChanged)>>(&source, &QObject::objectNameChanged);
    std::thread worker(
        [&]
        {
            int count = 0;
            while (!stop.load())
            {
                source.setObjectName(QString::number(count++));
                started.store(true);
            }
        });
    while (!started.load())
        std::this_thread::yield();
    recorder.reset();
    stop.store(true);
    worker.join();
}
} // namespace
