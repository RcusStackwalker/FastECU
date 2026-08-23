#include "apps/bench/bench_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
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
        .exchange_count = 1,
        .tx = {0x31, 0xE0},
        .rx = {0x71, 0xE0, 0x01},
        .last_tx = {0x31, 0xE0},
        .last_rx = {0x71, 0xE0, 0x01},
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
    EXPECT_NE(text.find("1 exchange"), std::string::npos);
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
    EXPECT_NE(json.find("\"exchanges\":1"), std::string::npos);
    EXPECT_NE(json.find("\"last_tx\":\"31e0\""), std::string::npos);
    EXPECT_NE(json.find("\"last_rx\":\"71e001\""), std::string::npos);
    EXPECT_NE(json.find("\"ok\":false"), std::string::npos);
    EXPECT_NE(json.find("\"error_kind\":\"BadResponse\""), std::string::npos);
    EXPECT_NE(json.find("\"vbatt\":11.676"), std::string::npos);
}

TEST(BenchFormat, MultiExchangeTextAndJsonCarryFirstAndLastPdus)
{
    CommandOutcome outcome = eraseFailure();
    outcome.exchange_count = 5;
    outcome.last_tx = {0x31, 0xE1, 0x01};
    outcome.last_rx = {0x71, 0xE1, 0x00};
    outcome.data = {0xAA, 0xBB};

    const std::string text = format_text(outcome);
    const std::string json = format_json(outcome);

    EXPECT_NE(text.find("5 exchanges"), std::string::npos);
    EXPECT_NE(text.find("TX first 31 e0"), std::string::npos);
    EXPECT_NE(text.find("RX last 71 e1 00"), std::string::npos);
    EXPECT_NE(text.find("DATA aa bb"), std::string::npos);
    EXPECT_NE(json.find("\"exchanges\":5"), std::string::npos);
    EXPECT_NE(json.find("\"last_tx\":\"31e101\""), std::string::npos);
    EXPECT_NE(json.find("\"last_rx\":\"71e100\""), std::string::npos);
    EXPECT_NE(json.find("\"data\":\"aabb\""), std::string::npos);
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

TEST(BenchFormat, JsonOmitsBatteryWhenNotFinite)
{
    CommandOutcome outcome = eraseFailure();
    outcome.vbatt = std::numeric_limits<double>::quiet_NaN();

    EXPECT_EQ(format_json(outcome).find("\"vbatt\""), std::string::npos);
}

TEST(BenchFormat, JsonEscapesControlCharactersInDetail)
{
    CommandOutcome outcome = eraseFailure();
    outcome.error_detail = "one\x01two\bthree\ffour";

    const std::string json = format_json(outcome);

    EXPECT_NE(json.find("\\u0001"), std::string::npos);
    EXPECT_NE(json.find("\\b"), std::string::npos);
    EXPECT_NE(json.find("\\f"), std::string::npos);
    EXPECT_EQ(json.find('\x01'), std::string::npos);
    EXPECT_EQ(json.find('\b'), std::string::npos);
    EXPECT_EQ(json.find('\f'), std::string::npos);
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

TEST(BenchTypes, CommandTableMarksExactlyTheCommandsRequiringDestructiveAcknowledgement)
{
    for (const CommandSpec& spec : command_table())
    {
        const bool expected = spec.id == CommandId::Send || spec.id == CommandId::SendRaw ||
                              spec.id == CommandId::Unlock || spec.id == CommandId::Erase ||
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

TEST(BenchFormat, StatsAreOmittedUnlessRequested)
{
    const CommandOutcome outcome{
        .step = "read 0x200 4", .exchange_count = 2, .data = bytes::Bytes{1, 2, 3, 4}, .elapsed_ms = 100, .ok = true};

    EXPECT_EQ(format_text(outcome).find("bytes/s"), std::string::npos);
    EXPECT_EQ(format_json(outcome).find("bytes_per_s"), std::string::npos);
}

TEST(BenchFormatStats, JsonCarriesBothDerivedFigures)
{
    const CommandOutcome outcome{
        .step = "read 0x200 4", .exchange_count = 2, .data = bytes::Bytes{1, 2, 3, 4}, .elapsed_ms = 100, .ok = true};

    const std::string json = format_json(outcome, true);

    // 4 bytes / 0.100 s = 40 bytes/s; 100 ms / 2 exchanges = 50 ms.
    EXPECT_NE(json.find(R"("bytes_per_s":40.0)"), std::string::npos);
    EXPECT_NE(json.find(R"("ms_per_exchange":50.0)"), std::string::npos);
}

TEST(BenchFormatStats, BytesPerSecondIsOmittedWithoutData)
{
    const CommandOutcome outcome{.step = "erase", .exchange_count = 1, .elapsed_ms = 100, .ok = true};

    const std::string json = format_json(outcome, true);

    EXPECT_EQ(json.find("bytes_per_s"), std::string::npos);
    EXPECT_NE(json.find(R"("ms_per_exchange":100.0)"), std::string::npos);
}

TEST(BenchFormatStats, BytesPerSecondIsOmittedWhenNoTimeElapsed)
{
    const CommandOutcome outcome{
        .step = "read 0x200 4", .exchange_count = 1, .data = bytes::Bytes{1, 2, 3, 4}, .elapsed_ms = 0, .ok = true};

    const std::string json = format_json(outcome, true);

    EXPECT_EQ(json.find("bytes_per_s"), std::string::npos);
}

TEST(BenchFormatStats, MsPerExchangeIsOmittedWithoutExchanges)
{
    const CommandOutcome outcome{.step = "ports", .exchange_count = 0, .elapsed_ms = 5, .ok = true};

    const std::string json = format_json(outcome, true);

    EXPECT_EQ(json.find("ms_per_exchange"), std::string::npos);
}

TEST(BenchFormatStats, TextModePrintsAnIndentedStatsLine)
{
    const CommandOutcome outcome{
        .step = "read 0x200 4", .exchange_count = 2, .data = bytes::Bytes{1, 2, 3, 4}, .elapsed_ms = 100, .ok = true};

    EXPECT_NE(format_text(outcome, true).find("  40.0 bytes/s, 50.0 ms/exchange\n"), std::string::npos);
}

} // namespace
} // namespace fastecu::bench
