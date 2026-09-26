#include "src/ui/desktop/dtc_operations.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>

#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using fastecu::diagnostics::FakeDiagnosticLink;

class DtcOperationsTest : public QObject
{
    Q_OBJECT

  private slots:

    void aFailedRunLogsOnceAndReenablesTheButtons()
    {
        FakeDiagnosticLink link; // five-baud answers nothing -> fails before any sleep
        DtcOperations dialog(link);
        QSignalSpy errors(&dialog, &DtcOperations::LOG_E);
        auto *read = dialog.findChild<QPushButton *>("readDtcButton");
        QVERIFY(read != nullptr);
        read->click();
        QVERIFY(!read->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(read->isEnabled(), 5000);
        const bool logged = std::any_of(errors.begin(), errors.end(), [](const QList<QVariant>& args)
                                        { return args.at(0).toString().startsWith("DTC operation failed: "); });
        QVERIFY(logged);
    }

    void closeDuringARunStopsTheWorkerAndResets()
    {
        FakeDiagnosticLink link;
        link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08}); // accepted -> 500 ms sleep follows
        auto *dialog = new DtcOperations(link);
        dialog->findChild<QPushButton *>("readDtcButton")->click();
        QTest::qWait(50);
        QElapsedTimer timer;
        timer.start();
        dialog->close();
        QVERIFY(timer.elapsed() < 400);
        QCOMPARE(link.calls.back(), std::string("reset"));
        QVERIFY(std::find(link.calls.begin(), link.calls.end(), "set_header None") != link.calls.end());
        delete dialog;
    }
};

QTEST_MAIN(DtcOperationsTest)
#include "dtc_operations_test.moc"
