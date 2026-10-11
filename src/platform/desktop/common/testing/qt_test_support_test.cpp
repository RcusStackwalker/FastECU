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
using fastecu::testing::ProcessEventsFor;
using fastecu::testing::SignalRecorder;
using fastecu::testing::WaitUntil;

namespace
{
QPointer<QCoreApplication> g_observed_application;
class ApplicationTeardownObserver : public ::testing::Environment
{
    void TearDown() override
    {
        EXPECT_EQ(QCoreApplication::instance(), nullptr);
        EXPECT_TRUE(g_observed_application.isNull());
    }
};
// Environments tear down in reverse registration order.
const auto *const kTeardownObserver = ::testing::AddGlobalTestEnvironment(new ApplicationTeardownObserver);
const auto *const kEnvironment = ::testing::AddGlobalTestEnvironment(new fastecu::testing::CoreApplicationEnvironment(
    []
    {
        EXPECT_EQ(QCoreApplication::instance(), nullptr);
        QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    },
    /*use_96_dpi=*/true));

TEST(QtTestSupport, ApplicationLivesDuringFixtures)
{
    ASSERT_NE(QCoreApplication::instance(), nullptr);
    g_observed_application = QCoreApplication::instance();
    EXPECT_EQ(QCoreApplication::arguments().front(), QString("fastecu-test"));
    EXPECT_TRUE(QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts));
    EXPECT_TRUE(QCoreApplication::testAttribute(Qt::AA_Use96Dpi));
}

TEST(QtTestSupport, CapturesImmediateAndWorkerSignalsWithoutEventProcessing)
{
    QObject source;
    SignalRecorder recorder(&source, &QObject::objectNameChanged);
    ASSERT_TRUE(recorder.IsValid());
    source.setObjectName("immediate");
    std::thread worker([&] { source.setObjectName("worker"); });
    worker.join();
    ASSERT_EQ(recorder.Count(), 2U);
    const auto records = recorder.Snapshot();
    EXPECT_EQ(std::get<0>(records[0]), QString("immediate"));
    EXPECT_EQ(std::get<0>(records[1]), QString("worker"));
}

TEST(QtTestSupport, CapturesQueuedSignals)
{
    QObject source;
    SignalRecorder recorder(&source, &QObject::objectNameChanged);
    QTimer::singleShot(0, &source, [&] { source.setObjectName("queued"); });
    ASSERT_TRUE(WaitUntil([&] { return recorder.Count() == 1; }, 100ms));
    EXPECT_EQ(std::get<0>(recorder.Snapshot().front()), QString("queued"));
}

TEST(QtTestSupport, TimesOutAndProcessesDeferredDeletion)
{
    EXPECT_FALSE(WaitUntil([] { return false; }, 2ms));
    QPointer<QObject> object = new QObject;
    object->deleteLater();
    ASSERT_TRUE(WaitUntil([&] { return object.isNull(); }, 100ms));
}

TEST(QtTestSupport, RecorderCanDieBeforeSender)
{
    QObject source;
    {
        SignalRecorder recorder(&source, &QObject::objectNameChanged);
        source.setObjectName("first");
        ASSERT_EQ(recorder.Count(), 1U);
    }
    source.setObjectName("after destruction");
    ProcessEventsFor(2ms);
}

TEST(QtTestSupport, SenderCanDieBeforeRecorder)
{
    auto source = std::make_unique<QObject>();
    SignalRecorder recorder(source.get(), &QObject::objectNameChanged);
    source->setObjectName("retained");
    source.reset();
    ASSERT_EQ(recorder.Count(), 1U);
    EXPECT_EQ(std::get<0>(recorder.Snapshot().front()), QString("retained"));
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
    {
        std::this_thread::yield();
    }
    recorder.reset();
    stop.store(true);
    worker.join();
}
} // namespace
