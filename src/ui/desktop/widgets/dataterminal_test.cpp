#include "src/platform/desktop/common/testing/widgets_application_environment.h"

#include <functional>
#include <string>
#include <vector>

#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/protocol/testing/fake_diagnostic_link.h"
#include "src/ui/desktop/widgets/dataterminal.h"

namespace
{
using fastecu::diagnostics::FakeDiagnosticLink;
using ::testing::ElementsAre;

// Types `message` into a terminal tab and presses its send button.
std::vector<std::string> send(FakeDiagnosticLink& link, const char *bus, const QString& message,
                              const std::function<void(DataTerminal&)>& configure = {})
{
    DataTerminal terminal(link);
    if (configure)
    {
        configure(terminal);
    }
    terminal.findChild<QLineEdit *>(QString(bus) + "MsgToSend")->setText(message);
    terminal.findChild<QPushButton *>(QString("send") + (QString(bus) == "kline" ? "Kline" : "Can") + "Message")
        ->click();
    return link.calls;
}

TEST(DataTerminalTest, klineSsmWrapsEachMessageInAnSsmHeader)
{
    FakeDiagnosticLink link;
    EXPECT_THAT(send(link, "kline", "A8 00"),
                ElementsAre("open kline header=None iso14230=false baud=4800 start=80 tester=F0 target=10",
                            "write 80 10 F0 02 A8 00 2A", "read 200", "reset"));
}

TEST(DataTerminalTest, klineIso14230SendsMessagesAsTyped)
{
    FakeDiagnosticLink link;
    const auto calls = send(link, "kline", "81 10 F0", [](DataTerminal& terminal)
                            { terminal.findChild<QComboBox *>("klineProtocol")->setCurrentIndex(1); });
    EXPECT_THAT(calls, ElementsAre("open kline header=None iso14230=true baud=4800 start=80 tester=F0 target=10",
                                   "write 81 10 F0", "read 200", "reset"));
}

// An out-of-range baud rate skips opening the link but still runs the script.
TEST(DataTerminalTest, klineInvalidBaudSkipsOpen)
{
    FakeDiagnosticLink link;
    const auto calls = send(link, "kline", "81", [](DataTerminal& terminal)
                            { terminal.findChild<QLineEdit *>("klineBaudRate")->setText("nan"); });
    EXPECT_THAT(calls, ElementsAre("write 80 10 F0 01 81 02", "read 200", "reset"));
}

TEST(DataTerminalTest, canPrefixesTheTesterId)
{
    FakeDiagnosticLink link;
    EXPECT_THAT(send(link, "can", "22 F1 90"),
                ElementsAre("open can iso15765=true bitrate=500000 extended=false source=7E0 destination=7E8",
                            "write 00 00 07 E0 22 F1 90", "read 200", "reset"));
}

const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment());

} // namespace
