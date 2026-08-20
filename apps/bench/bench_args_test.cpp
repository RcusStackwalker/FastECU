#include "apps/bench/bench_args.h"

#include <gtest/gtest.h>

#include <array>
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
    EXPECT_TRUE(parse({"send", "31", "e0"}).has_value());
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

} // namespace
} // namespace fastecu::bench
