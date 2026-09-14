#include "src/backend/ports/testing/result_matchers.h"
#include "apps/bench/bench_args.h"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace fastecu::bench
{
namespace
{

Result<ParsedCommandLine> parse(std::vector<std::string_view> args)
{
    return parse_command_line(args);
}

TEST(BenchArgs, ParsesASingleStepWithItsArguments)
{
    const auto parsed = parse({"read", "0x200", "1"});

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    ASSERT_EQ(parsed->steps.size(), 1U);
    EXPECT_EQ(parsed->steps[0].id, CommandId::Read);
    EXPECT_EQ(parsed->steps[0].args, (std::vector<std::string>{"0x200", "1"}));
}

TEST(BenchArgs, SplitsChainedStepsOnTheColonSeparator)
{
    const auto parsed = parse({"read", "0x200", "1", ":", "crc-check", "0x8000"});

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    ASSERT_EQ(parsed->steps.size(), 2U);
    EXPECT_EQ(parsed->steps[0].id, CommandId::Read);
    EXPECT_EQ(parsed->steps[1].id, CommandId::CrcCheck);
    EXPECT_EQ(parsed->steps[1].args, (std::vector<std::string>{"0x8000"}));
}

TEST(BenchArgs, RejectsADestructiveStepWithoutItsFlag)
{
    const auto parsed = parse({"erase"});

    ASSERT_THAT(parsed, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_NE(parsed.error().detail.find("--destructive"), std::string::npos);
}

TEST(BenchArgs, AcceptsADestructiveStepCarryingItsFlag)
{
    const auto parsed = parse({"erase", "--destructive"});

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    ASSERT_EQ(parsed->steps.size(), 1U);
    EXPECT_TRUE(parsed->steps[0].destructive_ack);
}

TEST(BenchArgs, RejectsTheWholeChainWhenALaterStepIsUngated)
{
    // The gate must fire before the port opens, so an ungated third step
    // fails the whole parse rather than being discovered mid-session.
    ASSERT_THAT(parse({"read", "0x200", "1", ":", "unlock", "--destructive", ":", "erase"}),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(BenchArgs, RejectsPortsChainedWithAnotherStep)
{
    // `ports` never opens a device: main.cpp relies on it being the only
    // step so it can be handled before any transport is constructed.
    const auto parsed = parse({"ports", ":", "erase", "--destructive"});

    ASSERT_THAT(parsed, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_NE(parsed.error().detail.find("ports"), std::string::npos);
}

TEST(BenchArgs, RejectsDestructiveFlagOnANonDestructiveStep)
{
    ASSERT_THAT(parse({"read", "0x200", "1", "--destructive"}), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(BenchArgs, RejectsArbitraryDiagnosticPdusWithoutDestructiveAcknowledgement)
{
    const std::vector<std::vector<std::string_view>> command_lines = {
        {"send", "22", "f1", "90"},
        {"send-raw", "31", "e1", "02"},
    };

    for (const auto& command_line : command_lines)
    {
        const auto parsed = parse(command_line);
        ASSERT_THAT(parsed, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
        EXPECT_NE(parsed.error().detail.find("--destructive"), std::string::npos);
    }
}

TEST(BenchArgs, AcceptsArbitraryDiagnosticPdusWithDestructiveAcknowledgement)
{
    const std::vector<std::vector<std::string_view>> command_lines = {
        {"send", "22", "f1", "90", "--destructive"},
        {"send-raw", "31", "e1", "02", "--destructive"},
    };

    for (const auto& command_line : command_lines)
    {
        const auto parsed = parse(command_line);
        ASSERT_THAT(parsed, fastecu::testing::IsOk());
        ASSERT_EQ(parsed->steps.size(), 1U);
        EXPECT_TRUE(parsed->steps[0].destructive_ack);
    }
}

TEST(BenchArgs, RejectsKnownDestructivePdusThroughDiagnosticCommands)
{
    const std::vector<std::vector<std::string_view>> command_lines = {
        {"send", "3b", "9a", "01", "--destructive"},
        {"send-raw", "31", "e0", "--destructive"},
        {"send", "34", "00", "80", "00", "00", "00", "00", "01", "--destructive"},
        {"send-raw", "36", "aa", "--destructive"},
    };

    for (const auto& command_line : command_lines)
    {
        const auto parsed = parse(command_line);
        ASSERT_THAT(parsed, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
        EXPECT_NE(parsed.error().detail.find("named destructive command"), std::string::npos);
    }
}

TEST(BenchArgs, PassesUploadRoutineFromThroughAsOrdinaryArguments)
{
    const auto parsed = parse({"upload-routine", "erase-redirect", "--from", "custom.bin", "--destructive"});

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    ASSERT_EQ(parsed->steps.size(), 1U);
    EXPECT_EQ(parsed->steps[0].id, CommandId::UploadRoutine);
    EXPECT_TRUE(parsed->steps[0].destructive_ack);
    EXPECT_EQ(parsed->steps[0].args, (std::vector<std::string>{"erase-redirect", "--from", "custom.bin"}));
}

TEST(BenchArgs, RejectsUnknownCommands)
{
    ASSERT_THAT(parse({"frobnicate"}), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(BenchArgs, RejectsWrongArgumentCounts)
{
    EXPECT_THAT(parse({"read", "0x200"}), ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(parse({"read", "0x200", "1", "extra"}), ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(parse({"send", "31", "e1", "--destructive"}), fastecu::testing::IsOk());
}

TEST(BenchArgs, ParsesGlobalOptionsAnywhereInTheCommandLine)
{
    const auto parsed = parse({"--port", "op2-1", "read", "0x200", "1", "--json", "--timeout", "1500"});

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_EQ(parsed->options.port_name, "op2-1");
    EXPECT_TRUE(parsed->options.json);
    EXPECT_EQ(parsed->options.timeout_ms, 1500);
    EXPECT_EQ(parsed->steps[0].args, (std::vector<std::string>{"0x200", "1"}));
}

TEST(BenchArgs, RejectsAnEmptyCommandLine)
{
    EXPECT_THAT(parse({}), ::testing::Not(fastecu::testing::IsOk()));
}

TEST(BenchArgs, RejectsAnEmptyStepBetweenSeparators)
{
    EXPECT_THAT(parse({"erase", "--destructive", ":", ":", "connect"}), ::testing::Not(fastecu::testing::IsOk()));
}

TEST(BenchArgs, RejectsGlobalOptionsMissingTheirValue)
{
    ASSERT_THAT(parse({"--port"}), fastecu::testing::IsErr(ErrorKind::InvalidConfig));

    ASSERT_THAT(parse({"--timeout"}), fastecu::testing::IsErr(ErrorKind::InvalidConfig));

    ASSERT_THAT(parse({"--script"}), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(BenchArgs, RejectsANonNumericTimeoutValue)
{
    ASSERT_THAT(parse({"--timeout", "abc"}), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(BenchArgs, RejectsTimeoutThatCannotFitDownstreamStorage)
{
    ASSERT_THAT(parse({"--timeout", "65536", "send-raw", "22"}), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(BenchArgs, AcceptsLargestTimeoutThatFitsDownstreamStorage)
{
    const auto parsed = parse({"--timeout", "65535", "send-raw", "22", "--destructive"});

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_EQ(parsed->options.timeout_ms, 65535);
}

TEST(BenchArgs, RejectsAScriptValueOtherThanStdin)
{
    ASSERT_THAT(parse({"--script", "notstdin"}), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(BenchArgs, ParsesU32InHexAndDecimal)
{
    EXPECT_EQ(parse_u32("0x8056a8").value(), 0x8056a8U);
    EXPECT_EQ(parse_u32("0X10").value(), 0x10U);
    EXPECT_EQ(parse_u32("192").value(), 192U);
    EXPECT_THAT(parse_u32(""), ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(parse_u32("0xzz"), ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(parse_u32("12nonsense"), ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(parse_u32("0x1ffffffff"), ::testing::Not(fastecu::testing::IsOk()));
}

TEST(BenchArgs, ParsesHexByteTokens)
{
    const std::vector<std::string> tokens{"31", "e0", "FF"};
    ASSERT_THAT(parse_hex_bytes(tokens), fastecu::testing::IsOkAnd((bytes::Bytes{0x31, 0xE0, 0xFF})));
}

TEST(BenchArgs, RejectsMalformedHexByteTokens)
{
    const std::vector<std::string> tooWide{"1ff"};
    const std::vector<std::string> notHex{"zz"};

    EXPECT_THAT(parse_hex_bytes(tooWide), ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(parse_hex_bytes(notHex), ::testing::Not(fastecu::testing::IsOk()));
}

TEST(BenchArgs, VendorExtDefaultsToOff)
{
    const std::vector<std::string_view> args{"read", "0x200", "1"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_FALSE(parsed->options.vendor_ext);
}

TEST(BenchArgs, VendorExtFlagIsRecognisedAnywhereOnTheCommandLine)
{
    const std::vector<std::string_view> args{"read", "0x200", "1", "--vendor-ext"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_TRUE(parsed->options.vendor_ext);
    // The flag is global, not a step argument: it must not reach the step.
    ASSERT_EQ(parsed->steps.size(), 1U);
    EXPECT_EQ(parsed->steps.front().args.size(), 2U);
}

TEST(BenchArgs, StatsFlagIsRecognised)
{
    const std::vector<std::string_view> args{"--stats", "read", "0x200", "1"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_TRUE(parsed->options.stats);
}

} // namespace
} // namespace fastecu::bench
