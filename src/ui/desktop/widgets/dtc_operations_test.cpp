#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include <QKeyEvent>
#include "src/ui/desktop/widgets/dtc_operations.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include "src/platform/desktop/common/testing/signal_recorder.h"
#include "src/platform/desktop/common/testing/event_helpers.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "src/backend/protocol/testing/fake_diagnostic_link.h"

using fastecu::diagnostics::FakeDiagnosticLink;

TEST(DtcOperationsTest, aFailedRunLogsOnceAndReenablesTheButtons)
{
    FakeDiagnosticLink link; // five-baud answers nothing -> fails before any sleep
    DtcOperations dialog(link);
    fastecu::testing::SignalRecorder errors(&dialog, &DtcOperations::LOG_E);
    auto *read = dialog.findChild<QPushButton *>("readDtcButton");
    ASSERT_TRUE(read != nullptr);
    read->click();
    ASSERT_TRUE(!read->isEnabled());
    ASSERT_TRUE(fastecu::testing::wait_until([&] { return read->isEnabled(); }, std::chrono::milliseconds(5000)));
    const auto records = errors.snapshot();
    const bool logged = std::any_of(records.begin(), records.end(), [](const auto& args)
                                    { return std::get<0>(args).startsWith("DTC operation failed: "); });
    ASSERT_TRUE(logged);
}

TEST(DtcOperationsTest, closeDuringARunStopsTheWorkerAndResets)
{
    FakeDiagnosticLink link;
    link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08}); // accepted -> 500 ms sleep follows
    auto *dialog = new DtcOperations(link);
    dialog->findChild<QPushButton *>("readDtcButton")->click();
    fastecu::testing::process_events_for(std::chrono::milliseconds(50));
    QElapsedTimer timer;
    timer.start();
    dialog->close();
    ASSERT_TRUE(timer.elapsed() < 400);
    // The cancelled session's own epilogue ("set_header None", "reset")
    // runs first, then the dialog's stopWorker() resets a second time.
    ASSERT_TRUE(link.calls.size() >= 3);
    const std::vector<std::string> tail(link.calls.end() - 3, link.calls.end());
    ASSERT_EQ(tail, (std::vector<std::string>{"set_header None", "reset", "reset"}));
    delete dialog;
}

TEST(DtcOperationsTest, escapeDuringARunStopsTheWorkerAndResets)
{
    FakeDiagnosticLink link;
    link.queue_five_baud(bytes::Bytes{0x55, 0x08, 0x08}); // accepted -> 500 ms sleep follows
    auto *dialog = new DtcOperations(link);
    dialog->findChild<QPushButton *>("readDtcButton")->click();
    fastecu::testing::process_events_for(std::chrono::milliseconds(50));
    QElapsedTimer timer;
    timer.start();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(dialog, &press);
    QCoreApplication::sendEvent(dialog, &release); // QDialog's default handling calls reject()
    ASSERT_TRUE(timer.elapsed() < 400);
    ASSERT_TRUE(link.calls.size() >= 3);
    const std::vector<std::string> tail(link.calls.end() - 3, link.calls.end());
    ASSERT_EQ(tail, (std::vector<std::string>{"set_header None", "reset", "reset"}));
    delete dialog;
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
