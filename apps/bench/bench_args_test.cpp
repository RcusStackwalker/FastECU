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

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->steps.size(), 1u);
    EXPECT_EQ(parsed->steps[0].id, CommandId::Read);
    EXPECT_EQ(parsed->steps[0].args, (std::vector<std::string>{"0x200", "1"}));
}

TEST(BenchArgs, SplitsChainedStepsOnTheColonSeparator)
{
    const auto parsed = parse({"read", "0x200", "1", ":", "crc-check", "0x8000"});

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->steps.size(), 2u);
    EXPECT_EQ(parsed->steps[0].id, CommandId::Read);
    EXPECT_EQ(parsed->steps[1].id, CommandId::CrcCheck);
    EXPECT_EQ(parsed->steps[1].args, (std::vector<std::string>{"0x8000"}));
}

TEST(BenchArgs, RejectsADestructiveStepWithoutItsFlag)
{
    const auto parsed = parse({"erase"});

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(parsed.error().detail.find("--destructive"), std::string::npos);
}

TEST(BenchArgs, AcceptsADestructiveStepCarryingItsFlag)
{
    const auto parsed = parse({"erase", "--destructive"});

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->steps.size(), 1u);
    EXPECT_TRUE(parsed->steps[0].destructive_ack);
}

TEST(BenchArgs, RejectsTheWholeChainWhenALaterStepIsUngated)
{
    // The gate must fire before the port opens, so an ungated third step
    // fails the whole parse rather than being discovered mid-session.
    const auto parsed = parse({"read", "0x200", "1", ":", "unlock", "--destructive", ":", "erase"});

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchArgs, RejectsPortsChainedWithAnotherStep)
{
    // `ports` never opens a device: main.cpp relies on it being the only
    // step so it can be handled before any transport is constructed.
    const auto parsed = parse({"ports", ":", "erase", "--destructive"});

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
    EXPECT_NE(parsed.error().detail.find("ports"), std::string::npos);
}

TEST(BenchArgs, RejectsDestructiveFlagOnANonDestructiveStep)
{
    const auto parsed = parse({"read", "0x200", "1", "--destructive"});

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
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
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
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
        ASSERT_TRUE(parsed.has_value());
        ASSERT_EQ(parsed->steps.size(), 1u);
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
        ASSERT_FALSE(parsed.has_value());
        EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
        EXPECT_NE(parsed.error().detail.find("named destructive command"), std::string::npos);
    }
}

TEST(BenchArgs, PassesUploadRoutineFromThroughAsOrdinaryArguments)
{
    const auto parsed = parse({"upload-routine", "erase-redirect", "--from", "custom.bin", "--destructive"});

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->steps.size(), 1u);
    EXPECT_EQ(parsed->steps[0].id, CommandId::UploadRoutine);
    EXPECT_TRUE(parsed->steps[0].destructive_ack);
    EXPECT_EQ(parsed->steps[0].args, (std::vector<std::string>{"erase-redirect", "--from", "custom.bin"}));
}

TEST(BenchArgs, RejectsUnknownCommands)
{
    const auto parsed = parse({"frobnicate"});

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchArgs, RejectsWrongArgumentCounts)
{
    EXPECT_FALSE(parse({"read", "0x200"}).has_value());
    EXPECT_FALSE(parse({"read", "0x200", "1", "extra"}).has_value());
    EXPECT_TRUE(parse({"send", "31", "e1", "--destructive"}).has_value());
}

TEST(BenchArgs, ParsesGlobalOptionsAnywhereInTheCommandLine)
{
    const auto parsed = parse({"--port", "op2-1", "read", "0x200", "1", "--json", "--timeout", "1500"});

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->options.port_name, "op2-1");
    EXPECT_TRUE(parsed->options.json);
    EXPECT_EQ(parsed->options.timeout_ms, 1500);
    EXPECT_EQ(parsed->steps[0].args, (std::vector<std::string>{"0x200", "1"}));
}

TEST(BenchArgs, RejectsAnEmptyCommandLine)
{
    EXPECT_FALSE(parse({}).has_value());
}

TEST(BenchArgs, RejectsAnEmptyStepBetweenSeparators)
{
    EXPECT_FALSE(parse({"erase", "--destructive", ":", ":", "connect"}).has_value());
}

TEST(BenchArgs, RejectsGlobalOptionsMissingTheirValue)
{
    const auto missingPort = parse({"--port"});
    ASSERT_FALSE(missingPort.has_value());
    EXPECT_EQ(missingPort.error().kind, ErrorKind::InvalidConfig);

    const auto missingTimeout = parse({"--timeout"});
    ASSERT_FALSE(missingTimeout.has_value());
    EXPECT_EQ(missingTimeout.error().kind, ErrorKind::InvalidConfig);

    const auto missingScript = parse({"--script"});
    ASSERT_FALSE(missingScript.has_value());
    EXPECT_EQ(missingScript.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchArgs, RejectsANonNumericTimeoutValue)
{
    const auto parsed = parse({"--timeout", "abc"});

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchArgs, RejectsTimeoutThatCannotFitDownstreamStorage)
{
    const auto parsed = parse({"--timeout", "65536", "send-raw", "22"});

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchArgs, AcceptsLargestTimeoutThatFitsDownstreamStorage)
{
    const auto parsed = parse({"--timeout", "65535", "send-raw", "22", "--destructive"});

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->options.timeout_ms, 65535);
}

TEST(BenchArgs, RejectsAScriptValueOtherThanStdin)
{
    const auto parsed = parse({"--script", "notstdin"});

    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchArgs, ParsesU32InHexAndDecimal)
{
    EXPECT_EQ(parse_u32("0x8056a8").value(), 0x8056a8u);
    EXPECT_EQ(parse_u32("0X10").value(), 0x10u);
    EXPECT_EQ(parse_u32("192").value(), 192u);
    EXPECT_FALSE(parse_u32("").has_value());
    EXPECT_FALSE(parse_u32("0xzz").has_value());
    EXPECT_FALSE(parse_u32("12nonsense").has_value());
    EXPECT_FALSE(parse_u32("0x1ffffffff").has_value());
}

TEST(BenchArgs, ParsesHexByteTokens)
{
    const std::vector<std::string> tokens{"31", "e0", "FF"};
    const auto parsed = parse_hex_bytes(tokens);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, (bytes::Bytes{0x31, 0xE0, 0xFF}));
}

TEST(BenchArgs, RejectsMalformedHexByteTokens)
{
    const std::vector<std::string> tooWide{"1ff"};
    const std::vector<std::string> notHex{"zz"};

    EXPECT_FALSE(parse_hex_bytes(tooWide).has_value());
    EXPECT_FALSE(parse_hex_bytes(notHex).has_value());
}

TEST(BenchArgs, VendorExtDefaultsToOff)
{
    const std::vector<std::string_view> args{"read", "0x200", "1"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->options.vendor_ext);
}

TEST(BenchArgs, VendorExtFlagIsRecognisedAnywhereOnTheCommandLine)
{
    const std::vector<std::string_view> args{"read", "0x200", "1", "--vendor-ext"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->options.vendor_ext);
    // The flag is global, not a step argument: it must not reach the step.
    ASSERT_EQ(parsed->steps.size(), 1u);
    EXPECT_EQ(parsed->steps.front().args.size(), 2u);
}

TEST(BenchArgs, StatsFlagIsRecognised)
{
    const std::vector<std::string_view> args{"--stats", "read", "0x200", "1"};
    const Result<ParsedCommandLine> parsed = parse_command_line(args);

    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->options.stats);
}

} // namespace
} // namespace fastecu::bench
