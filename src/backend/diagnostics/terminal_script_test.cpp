#include "src/backend/diagnostics/terminal_script.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <string>
#include <vector>

#include "src/backend/ports/testing/result_matchers.h"

using namespace fastecu::diagnostics;
using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::ElementsAre;
using ::testing::HasSubstr;

namespace
{
TerminalStep Msg(bytes::Bytes payload)
{
    return TerminalMessageStep{std::move(payload)};
}

TerminalStep Pause(std::chrono::milliseconds duration)
{
    return TerminalDelayStep{duration};
}

fastecu::Result<std::vector<TerminalStep>> Parse(std::vector<std::string> lines)
{
    return ParseTerminalScript(lines);
}
} // namespace

TEST(TerminalScript, ParsesMessageLine)
{
    auto steps = Parse({"10 01 ff"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(Msg({0x10, 0x01, 0xFF})));
}

TEST(TerminalScript, AcceptsOneDigitUpperAndLowerCaseTokens)
{
    auto steps = Parse({"1 aB C"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(Msg({0x01, 0xAB, 0x0C})));
}

TEST(TerminalScript, DelayIsAStandalonePauseBetweenMessages)
{
    auto steps = Parse({"01 02", "delay(100)", "03 04"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(Msg({0x01, 0x02}), Pause(100ms), Msg({0x03, 0x04})));
}

TEST(TerminalScript, LeadingAndConsecutiveDelaysStayInOrder)
{
    auto steps = Parse({"delay(5)", "delay(7)", "01"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(Pause(5ms), Pause(7ms), Msg({0x01})));
}

TEST(TerminalScript, ZeroDelayIsAllowed)
{
    auto steps = Parse({"delay(0)"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(Pause(0ms)));
}

TEST(TerminalScript, MaximumDelayIsOneHour)
{
    ASSERT_THAT(Parse({"delay(3600000)"}), IsOk());
    EXPECT_THAT(Parse({"delay(3600001)"}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 1")));
}

TEST(TerminalScript, HugeDelayIsRejectedNotWrapped)
{
    EXPECT_THAT(Parse({"delay(99999999999999999999999)"}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 1")));
}

TEST(TerminalScript, TrimsSurroundingWhitespaceAndSkipsBlankLines)
{
    auto steps = Parse({"", "  10  01 ", "   ", "\tdelay(2)  ", ""});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(Msg({0x10, 0x01}), Pause(2ms)));
}

TEST(TerminalScript, MalformedDelaysAreRejected)
{
    for (const char *bad : {"delay()", "delay(abc)", "delay(-5)", "delay(1.5)", "delay 100", "delay(100", "delay(100)x",
                            "delay( 100 )", "Delay(100)", "delay(+5)"})
    {
        EXPECT_THAT(Parse({bad}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 1"))) << bad;
    }
}

TEST(TerminalScript, NonHexOrOversizedByteTokensAreRejected)
{
    for (const char *bad : {"zz", "10 0g", "100", "0x10", "10,11"})
    {
        EXPECT_THAT(Parse({bad}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 1"))) << bad;
    }
}

TEST(TerminalScript, ErrorNamesTheOffendingLineAndSendsNothing)
{
    auto steps = Parse({"01 02", "", "03 zz", "delay(5)"});
    EXPECT_THAT(steps, IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 3")));
}

TEST(TerminalScript, ScriptWithNoStepsIsRejected)
{
    EXPECT_THAT(Parse({}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("no steps")));
    EXPECT_THAT(Parse({"", "  "}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("no steps")));
}
