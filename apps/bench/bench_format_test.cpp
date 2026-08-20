#include "apps/bench/bench_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "apps/bench/bench_types.h"

namespace fastecu::bench
{
namespace
{

CommandOutcome eraseFailure()
{
    return CommandOutcome{
        .step = "erase",
        .tx = {0x31, 0xE0},
        .rx = {0x71, 0xE0, 0x01},
        .elapsed_ms = 39,
        .vbatt = 11.676,
        .ok = false,
        .note = "status=0x01",
        .error_kind = ErrorKind::BadResponse,
        .error_detail = "erase reported a non-zero status",
    };
}

TEST(BenchFormat, TextCarriesStepTxRxAndNote)
{
    const std::string text = format_text(eraseFailure());

    EXPECT_NE(text.find("erase"), std::string::npos);
    EXPECT_NE(text.find("31 e0"), std::string::npos);
    EXPECT_NE(text.find("71 e0 01"), std::string::npos);
    EXPECT_NE(text.find("status=0x01"), std::string::npos);
    EXPECT_NE(text.find("11.676"), std::string::npos);
    EXPECT_NE(text.find("FAIL"), std::string::npos);
}

TEST(BenchFormat, TextMarksSuccessWithoutTheFailureWord)
{
    CommandOutcome ok = eraseFailure();
    ok.ok = true;
    ok.error_kind.reset();
    ok.error_detail.clear();

    const std::string text = format_text(ok);

    EXPECT_NE(text.find("ok"), std::string::npos);
    EXPECT_EQ(text.find("FAIL"), std::string::npos);
}

TEST(BenchFormat, JsonIsOneFlatObjectWithHexStrings)
{
    const std::string json = format_json(eraseFailure());

    EXPECT_TRUE(json.starts_with("{"));
    EXPECT_TRUE(json.ends_with("}"));
    EXPECT_NE(json.find("\"step\":\"erase\""), std::string::npos);
    EXPECT_NE(json.find("\"tx\":\"31e0\""), std::string::npos);
    EXPECT_NE(json.find("\"rx\":\"71e001\""), std::string::npos);
    EXPECT_NE(json.find("\"ok\":false"), std::string::npos);
    EXPECT_NE(json.find("\"error_kind\":\"BadResponse\""), std::string::npos);
    EXPECT_NE(json.find("\"vbatt\":11.676"), std::string::npos);
}

TEST(BenchFormat, JsonEscapesQuotesAndBackslashesInDetail)
{
    CommandOutcome outcome = eraseFailure();
    outcome.error_detail = R"(said "no" \ here)";

    const std::string json = format_json(outcome);

    EXPECT_NE(json.find(R"(said \"no\" \\ here)"), std::string::npos);
}

TEST(BenchFormat, JsonOmitsBatteryWhenUnavailable)
{
    CommandOutcome outcome = eraseFailure();
    outcome.vbatt.reset();

    EXPECT_EQ(format_json(outcome).find("\"vbatt\""), std::string::npos);
}

TEST(BenchFormat, EveryErrorKindGetsADistinctNonZeroExitCode)
{
    const ErrorKind kinds[] = {ErrorKind::InvalidConfig, ErrorKind::Timeout,   ErrorKind::Disconnected,
                               ErrorKind::BadResponse,   ErrorKind::Cancelled, ErrorKind::Unsupported,
                               ErrorKind::Internal};
    std::vector<int> codes;
    for (const ErrorKind kind : kinds)
    {
        const int code = exit_code_for(kind);
        EXPECT_NE(code, 0) << to_string(kind);
        codes.push_back(code);
    }
    std::ranges::sort(codes);
    EXPECT_EQ(std::ranges::unique(codes).begin(), codes.end());
}

TEST(BenchTypes, CommandTableMarksExactlyTheDestructiveCommands)
{
    for (const CommandSpec& spec : command_table())
    {
        const bool expected = spec.id == CommandId::Unlock || spec.id == CommandId::Erase ||
                              spec.id == CommandId::Download || spec.id == CommandId::UploadRoutine;
        EXPECT_EQ(spec.destructive, expected) << spec.name;
    }
}

TEST(BenchTypes, FindCommandResolvesByNameAndRejectsUnknown)
{
    const CommandSpec *read = find_command("read");
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->id, CommandId::Read);
    EXPECT_FALSE(read->destructive);
    EXPECT_EQ(find_command("definitely-not-a-command"), nullptr);
}

} // namespace
} // namespace fastecu::bench
