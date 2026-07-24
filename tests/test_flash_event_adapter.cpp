// Standalone coverage for QtEventSinkAdapter (step 5c, Task 13): proves the
// IEventSink -> Qt signal translation in isolation, without constructing a
// FlashWorker (or a QThread) at all -- that independence from FlashWorker is
// the entire reason this class was extracted out of flash_worker.cpp's
// private QtEventSinkForwarder into its own target.
#include "src/platform/desktop/common/flash/flash_event_adapter.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

using fastecu::LogLevel;
using fastecu::flash::QtEventSinkAdapter;

class TestFlashEventAdapter : public QObject
{
    Q_OBJECT

  private slots:

    void logForwardsLevelAndMessageAsASignal()
    {
        QtEventSinkAdapter adapter;
        QSignalSpy spy(&adapter, &QtEventSinkAdapter::logEvent);

        adapter.log(LogLevel::Warning, "disk almost full");

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), static_cast<int>(LogLevel::Warning));
        QCOMPARE(spy.at(0).at(1).toString(), QString("disk almost full"));
    }

    void progressForwardsDoneAndTotal()
    {
        QtEventSinkAdapter adapter;
        QSignalSpy spy(&adapter, &QtEventSinkAdapter::progressChanged);

        adapter.progress(3, 10);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 3);
        QCOMPARE(spy.at(0).at(1).toInt(), 10);
    }

    void noticeForwardsAsAnInfoLevelLogEvent()
    {
        QtEventSinkAdapter adapter;
        QSignalSpy spy(&adapter, &QtEventSinkAdapter::logEvent);

        adapter.notice("connected");

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), static_cast<int>(LogLevel::Info));
    }
};

int run_test_flash_event_adapter(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TestFlashEventAdapter t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_flash_event_adapter.moc"
