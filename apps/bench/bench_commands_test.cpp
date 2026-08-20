#include "apps/bench/bench_commands.h"

#include <gtest/gtest.h>

#include "apps/bench/bench_args.h"
#include "apps/bench/testing/fake_bench_files.h"
#include "apps/bench/testing/fake_bench_session.h"

namespace fastecu::bench
{
namespace
{

using testing::FakeBenchFiles;
using testing::FakeBenchSession;

struct Harness
{
    FakeBenchSession session;
    FakeBenchFiles files;
    GlobalOptions options;

    Result<CommandOutcome> run(const StepSpec& step)
    {
        BenchContext context{session, files, options};
        return run_step(context, step);
    }
};

StepSpec step(CommandId id, std::vector<std::string> args = {})
{
    return StepSpec{.id = id, .args = std::move(args), .destructive_ack = false};
}

TEST(BenchCommands, ReadBuildsReadMemoryByAddressAndReturnsThePayload)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x63, 0xAB}};

    const auto outcome = harness.run(step(CommandId::Read, {"0x200", "1"}));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->ok);
    EXPECT_EQ(harness.session.requests.at(0), (bytes::Bytes{0x23, 0x00, 0x02, 0x00, 0x01}));
    EXPECT_EQ(outcome->rx, (bytes::Bytes{0xAB}));
}

TEST(BenchCommands, ReadChunksLongRangesAtTheFlashReadBlockSize)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes(193, 0x11), bytes::Bytes(9, 0x22)};

    const auto outcome = harness.run(step(CommandId::Read, {"0x8056a8", "200"}));

    ASSERT_TRUE(outcome.has_value());
    ASSERT_EQ(harness.session.requests.size(), 2u);
    // 192-byte first chunk, 8-byte remainder.
    EXPECT_EQ(harness.session.requests[0].back(), 192);
    EXPECT_EQ(harness.session.requests[1].back(), 8);
    EXPECT_EQ(outcome->rx.size(), 200u);
    EXPECT_NE(outcome->note.find("2 chunks"), std::string::npos);
}

TEST(BenchCommands, ReadRejectsAShortReplyRatherThanPaddingIt)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x63}};

    const auto outcome = harness.run(step(CommandId::Read, {"0x200", "4"}));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::BadResponse);
}

TEST(BenchCommands, DumpWritesTheReadBytesToTheNamedFile)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x63, 0x01, 0x02}};

    const auto outcome = harness.run(step(CommandId::Dump, {"0x200", "2", "out.bin"}));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(harness.files.saved.at("out.bin"), (bytes::Bytes{0x01, 0x02}));
}

TEST(BenchCommands, SendForwardsRawHexAsAPdu)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x7B, 0x00}};

    const auto outcome = harness.run(step(CommandId::Send, {"3b", "9a", "01"}));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(harness.session.requests.at(0), (bytes::Bytes{0x3B, 0x9A, 0x01}));
    EXPECT_EQ(outcome->tx, (bytes::Bytes{0x3B, 0x9A, 0x01}));
}

TEST(BenchCommands, CrcCheckReportsAZeroStatusAsSuccess)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x71, 0xE1, 0x00}};

    const auto outcome = harness.run(step(CommandId::CrcCheck, {"0x8000"}));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->ok);
}

TEST(BenchCommands, CrcCheckReportsANonZeroStatusAsFailure)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x71, 0xE1, 0x01}};

    const auto outcome = harness.run(step(CommandId::CrcCheck, {"0x8000"}));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::BadResponse);
}

TEST(BenchCommands, EveryStepRecordsTheBatteryVoltage)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x63, 0xAB}};
    harness.session.battery = 11.676;

    const auto outcome = harness.run(step(CommandId::Read, {"0x200", "1"}));

    ASSERT_TRUE(outcome.has_value());
    ASSERT_TRUE(outcome->vbatt.has_value());
    EXPECT_NEAR(*outcome->vbatt, 11.676, 0.0005);
}

TEST(BenchDecode, EraseStatusZeroReadsAsSuccess)
{
    const std::string note = decode_erase_reply(bytes::Bytes{0xE0, 0x00});

    EXPECT_NE(note.find("status=0x00"), std::string::npos);
    EXPECT_EQ(note.find("0x5a28"), std::string::npos);
}

TEST(BenchDecode, EraseStatusOneNamesBothReachablePathsAsAmbiguous)
{
    // colt_commented.S 0x5a28 is reached from the pre-erase gate (0x59b0) and
    // from the post-erase ret==3 branch (0x5a14). The reply cannot tell them
    // apart, and saying so is the point of the note.
    const std::string note = decode_erase_reply(bytes::Bytes{0xE0, 0x01});

    EXPECT_NE(note.find("status=0x01"), std::string::npos);
    EXPECT_NE(note.find("0x5a28"), std::string::npos);
    EXPECT_NE(note.find("0x59b0"), std::string::npos);
    EXPECT_NE(note.find("0x5a14"), std::string::npos);
    EXPECT_NE(note.find("ambiguous"), std::string::npos);
}

TEST(BenchDecode, EraseReplyWithoutAStatusByteSaysSo)
{
    EXPECT_NE(decode_erase_reply(bytes::Bytes{0xE0}).find("no status byte"), std::string::npos);
}

} // namespace
} // namespace fastecu::bench
