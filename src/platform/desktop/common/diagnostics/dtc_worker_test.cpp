#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include "src/platform/desktop/common/diagnostics/dtc_worker.h"

#include <QCoreApplication>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <gtest/gtest.h>

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

class DtcWorkerTest : public ::testing::Test
{

  public:
};

TEST_F(DtcWorkerTest, reportsTheSessionOutcomeAndForwardsLogLines)
{
    FakeDiagnosticLink link;
    link.queue_five_baud(bytes::Bytes{0x55, 0x00, 0x00}); // rejected
    DtcWorker worker(DtcRequest{ObdProtocol::Iso9141, DtcOperation::Read}, link, std::make_unique<FakeClock>());
    fastecu::testing::SignalRecorder logs(&worker, &DtcWorker::logEvent);
    fastecu::testing::SignalRecorder done(&worker, &DtcWorker::completed);
    worker.start();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return done.count() != 0; }, std::chrono::milliseconds(5000)));
    ASSERT_EQ(done.count(), 1);
    const auto result = std::get<0>(done.snapshot().at(0));
    ASSERT_TRUE(!result.success);
    ASSERT_EQ(result.error_kind, ErrorKind::BadResponse);
    ASSERT_TRUE(logs.count() >= 2); // "Testing ..." and "iso9141 five baud init failed."
}

TEST_F(DtcWorkerTest, stopBeforeStartCancelsTheRun)
{
    FakeDiagnosticLink link;
    link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08});
    DtcWorker worker(DtcRequest{ObdProtocol::Iso9141, DtcOperation::Read}, link, std::make_unique<FakeClock>());
    fastecu::testing::SignalRecorder done(&worker, &DtcWorker::completed);
    worker.requestStop();
    worker.start();
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return done.count() != 0; }, std::chrono::milliseconds(5000)));
    ASSERT_EQ(std::get<0>(done.snapshot().at(0)).error_kind, ErrorKind::Cancelled);
    ASSERT_EQ(link.calls.back(), std::string("reset"));
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
