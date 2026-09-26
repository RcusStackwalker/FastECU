#include "src/ui/desktop/dtc_operations.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <string>
#include <vector>

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
        // The cancelled session's own epilogue ("set_header None", "reset")
        // runs first, then the dialog's stopWorker() resets a second time.
        QVERIFY(link.calls.size() >= 3);
        const std::vector<std::string> tail(link.calls.end() - 3, link.calls.end());
        QCOMPARE(tail, (std::vector<std::string>{"set_header None", "reset", "reset"}));
        delete dialog;
    }

    void escapeDuringARunStopsTheWorkerAndResets()
    {
        FakeDiagnosticLink link;
        link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08}); // accepted -> 500 ms sleep follows
        auto *dialog = new DtcOperations(link);
        dialog->findChild<QPushButton *>("readDtcButton")->click();
        QTest::qWait(50);
        QElapsedTimer timer;
        timer.start();
        QTest::keyClick(dialog, Qt::Key_Escape); // QDialog's default handling calls reject()
        QVERIFY(timer.elapsed() < 400);
        QVERIFY(link.calls.size() >= 3);
        const std::vector<std::string> tail(link.calls.end() - 3, link.calls.end());
        QCOMPARE(tail, (std::vector<std::string>{"set_header None", "reset", "reset"}));
        delete dialog;
    }
};

QTEST_MAIN(DtcOperationsTest)
#include "dtc_operations_test.moc"
