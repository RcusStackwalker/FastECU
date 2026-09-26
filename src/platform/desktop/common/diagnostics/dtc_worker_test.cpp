#include "src/platform/desktop/common/diagnostics/dtc_worker.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::diagnostics::DtcOperation;
using fastecu::diagnostics::DtcRequest;
using fastecu::diagnostics::DtcWorker;
using fastecu::diagnostics::DtcWorkerResult;
using fastecu::diagnostics::FakeDiagnosticLink;
using fastecu::diagnostics::ObdProtocol;

class DtcWorkerTest : public QObject
{
    Q_OBJECT

  private slots:

    void reportsTheSessionOutcomeAndForwardsLogLines()
    {
        FakeDiagnosticLink link;
        link.queue_five_baud(bytes::Bytes{0x55, 0x00, 0x00}); // rejected
        DtcWorker worker(DtcRequest{ObdProtocol::Iso9141, DtcOperation::Read}, link, std::make_unique<FakeClock>());
        QSignalSpy logs(&worker, &DtcWorker::logEvent);
        QSignalSpy done(&worker, &DtcWorker::completed);
        worker.start();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.count(), 1);
        const auto result = done.at(0).at(0).value<DtcWorkerResult>();
        QVERIFY(!result.success);
        QCOMPARE(result.error_kind, ErrorKind::BadResponse);
        QVERIFY(logs.count() >= 2); // "Testing ..." and "iso9141 five baud init failed."
    }

    void stopBeforeStartCancelsTheRun()
    {
        FakeDiagnosticLink link;
        link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08});
        DtcWorker worker(DtcRequest{ObdProtocol::Iso9141, DtcOperation::Read}, link, std::make_unique<FakeClock>());
        QSignalSpy done(&worker, &DtcWorker::completed);
        worker.requestStop();
        worker.start();
        QVERIFY(done.wait(5000));
        QCOMPARE(done.at(0).at(0).value<DtcWorkerResult>().error_kind, ErrorKind::Cancelled);
        QCOMPARE(link.calls.back(), std::string("reset"));
    }
};

QTEST_MAIN(DtcWorkerTest)
#include "dtc_worker_test.moc"
