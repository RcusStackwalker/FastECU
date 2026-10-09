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
TerminalStep msg(bytes::Bytes payload)
{
    return TerminalMessageStep{std::move(payload)};
}

TerminalStep pause(std::chrono::milliseconds duration)
{
    return TerminalDelayStep{duration};
}

fastecu::Result<std::vector<TerminalStep>> parse(std::vector<std::string> lines)
{
    return parse_terminal_script(lines);
}
} // namespace

TEST(TerminalScript, ParsesMessageLine)
{
    auto steps = parse({"10 01 ff"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(msg({0x10, 0x01, 0xFF})));
}

TEST(TerminalScript, AcceptsOneDigitUpperAndLowerCaseTokens)
{
    auto steps = parse({"1 aB C"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(msg({0x01, 0xAB, 0x0C})));
}

TEST(TerminalScript, DelayIsAStandalonePauseBetweenMessages)
{
    auto steps = parse({"01 02", "delay(100)", "03 04"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(msg({0x01, 0x02}), pause(100ms), msg({0x03, 0x04})));
}

TEST(TerminalScript, LeadingAndConsecutiveDelaysStayInOrder)
{
    auto steps = parse({"delay(5)", "delay(7)", "01"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(pause(5ms), pause(7ms), msg({0x01})));
}

TEST(TerminalScript, ZeroDelayIsAllowed)
{
    auto steps = parse({"delay(0)"});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(pause(0ms)));
}

TEST(TerminalScript, MaximumDelayIsOneHour)
{
    ASSERT_THAT(parse({"delay(3600000)"}), IsOk());
    EXPECT_THAT(parse({"delay(3600001)"}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 1")));
}

TEST(TerminalScript, HugeDelayIsRejectedNotWrapped)
{
    EXPECT_THAT(parse({"delay(99999999999999999999999)"}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 1")));
}

TEST(TerminalScript, TrimsSurroundingWhitespaceAndSkipsBlankLines)
{
    auto steps = parse({"", "  10  01 ", "   ", "\tdelay(2)  ", ""});
    ASSERT_THAT(steps, IsOk());
    EXPECT_THAT(*steps, ElementsAre(msg({0x10, 0x01}), pause(2ms)));
}

TEST(TerminalScript, MalformedDelaysAreRejected)
{
    for (const char *bad : {"delay()", "delay(abc)", "delay(-5)", "delay(1.5)", "delay 100", "delay(100", "delay(100)x",
                            "delay( 100 )", "Delay(100)", "delay(+5)"})
    {
        EXPECT_THAT(parse({bad}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 1"))) << bad;
    }
}

TEST(TerminalScript, NonHexOrOversizedByteTokensAreRejected)
{
    for (const char *bad : {"zz", "10 0g", "100", "0x10", "10,11"})
    {
        EXPECT_THAT(parse({bad}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 1"))) << bad;
    }
}

TEST(TerminalScript, ErrorNamesTheOffendingLineAndSendsNothing)
{
    auto steps = parse({"01 02", "", "03 zz", "delay(5)"});
    EXPECT_THAT(steps, IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("line 3")));
}

TEST(TerminalScript, ScriptWithNoStepsIsRejected)
{
    EXPECT_THAT(parse({}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("no steps")));
    EXPECT_THAT(parse({"", "  "}), IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("no steps")));
}
