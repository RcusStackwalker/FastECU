#include "apps/bench/bench_commands.h"

#include <gtest/gtest.h>

#include "apps/bench/bench_args.h"
#include "apps/bench/testing/fake_bench_files.h"
#include "apps/bench/testing/fake_bench_session.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"

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

StepSpec destructiveStep(CommandId id, std::vector<std::string> args = {})
{
    return StepSpec{.id = id, .args = std::move(args), .destructive_ack = true};
}

TEST(BenchCommands, UnlockSendsTheTwelveByteReflashPayload)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x7B, 0x00}};

    const auto outcome = harness.run(destructiveStep(CommandId::Unlock));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(harness.session.requests.at(0), MitsuColtCan::buildRequestReflashUnlock());
}

TEST(BenchCommands, EraseSendsRoutineControl224AndAcceptsAZeroStatus)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x71, 0xE0, 0x00}};

    const auto outcome = harness.run(destructiveStep(CommandId::Erase));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->ok);
    EXPECT_EQ(harness.session.requests.at(0), (bytes::Bytes{0x31, 0xE0}));
}

TEST(BenchCommands, EraseTreatsANonZeroStatusAsAFailureCarryingTheAmbiguityNote)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x71, 0xE0, 0x01}};

    const auto outcome = harness.run(destructiveStep(CommandId::Erase));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::BadResponse);
    EXPECT_NE(outcome.error().detail.find("0x5a28"), std::string::npos);
}

TEST(BenchCommands, EraseRejectsAReplyWithNoStatusByte)
{
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x71, 0xE0}};

    const auto outcome = harness.run(destructiveStep(CommandId::Erase));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::BadResponse);
}

TEST(BenchCommands, DownloadSendsRequestDownloadThenTransferDataThenTheChecksum)
{
    Harness harness;
    harness.files.contents["blob.bin"] = bytes::Bytes{0xAA, 0xBB};
    harness.session.replies = {
        bytes::Bytes{0x74},             // RequestDownload for the payload
        bytes::Bytes{0x76},             // TransferData
        bytes::Bytes{0x74},             // RequestDownload for the checksum
        bytes::Bytes{0x76},             // TransferData for the checksum
        bytes::Bytes{0x71, 0xE1, 0x00}, // CRC check
    };

    const auto outcome = harness.run(destructiveStep(CommandId::Download, {"0x8000", "blob.bin"}));

    ASSERT_TRUE(outcome.has_value());
    ASSERT_EQ(harness.session.requests.size(), 5u);
    EXPECT_EQ(harness.session.requests[0], MitsuColtCan::buildRequestDownload(0x8000, 2));
    EXPECT_EQ(harness.session.requests[1], (bytes::Bytes{0x36, 0xAA, 0xBB}));
    EXPECT_EQ(harness.session.requests[2],
              MitsuColtCan::buildRequestDownload(MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize));
    // 0xAA + 0xBB = 0x0165, big-endian.
    EXPECT_EQ(harness.session.requests[3], (bytes::Bytes{0x36, 0x01, 0x65}));
}

TEST(BenchCommands, DownloadFailsWhenTheCrcCheckReportsAMismatch)
{
    Harness harness;
    harness.files.contents["blob.bin"] = bytes::Bytes{0xAA};
    harness.session.replies = {bytes::Bytes{0x74}, bytes::Bytes{0x76}, bytes::Bytes{0x74}, bytes::Bytes{0x76},
                               bytes::Bytes{0x71, 0xE1, 0x01}};

    const auto outcome = harness.run(destructiveStep(CommandId::Download, {"0x8000", "blob.bin"}));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::BadResponse);
}

TEST(BenchCommands, DownloadFailsWhenTheFileIsMissing)
{
    Harness harness;

    const auto outcome = harness.run(destructiveStep(CommandId::Download, {"0x8000", "absent.bin"}));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchCommands, DownloadChunksAPayloadLargerThanTheTransferChunkSize)
{
    Harness harness;
    const bytes::Bytes bigFile(257, 0xAB);
    harness.files.contents["big.bin"] = bigFile;
    harness.session.replies = {
        bytes::Bytes{0x74},             // RequestDownload for the payload
        bytes::Bytes{0x76},             // TransferData, 256-byte frame
        bytes::Bytes{0x76},             // TransferData, 1-byte remainder frame
        bytes::Bytes{0x74},             // RequestDownload for the checksum
        bytes::Bytes{0x76},             // TransferData for the checksum
        bytes::Bytes{0x71, 0xE1, 0x00}, // CRC check
    };

    const auto outcome = harness.run(destructiveStep(CommandId::Download, {"0x8000", "big.bin"}));

    ASSERT_TRUE(outcome.has_value());
    ASSERT_EQ(harness.session.requests.size(), 6u);
    const std::vector<bytes::Bytes> frames = MitsuColtCan::buildTransferDataFrames(bytes::ByteView(bigFile));
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(harness.session.requests[1], frames[0]);
    EXPECT_EQ(harness.session.requests[1].size(), 257u); // SID + 256 payload bytes
    EXPECT_EQ(harness.session.requests[2], frames[1]);
    EXPECT_EQ(harness.session.requests[2].size(), 2u); // SID + 1 payload byte
}

TEST(BenchCommands, UploadRoutineSendsTheBakedArrayToItsRamSlot)
{
    Harness harness;
    // upload() makes exactly 5 exchanges for a routine this size (one
    // TransferData frame each way, same as Download): RequestDownload,
    // TransferData, RequestDownload (checksum), TransferData (checksum),
    // then the CRC check. The 6th scripted reply is intentionally never
    // consumed -- see the over-scripted-test note in the task brief.
    harness.session.replies = {
        bytes::Bytes{0x74}, bytes::Bytes{0x76}, bytes::Bytes{0x74}, bytes::Bytes{0x76}, bytes::Bytes{0x71, 0xE1, 0x00},
        bytes::Bytes{0x76}};

    const auto outcome = harness.run(destructiveStep(CommandId::UploadRoutine, {"erase-redirect"}));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(harness.session.requests.at(0),
              MitsuColtCan::buildRequestDownload(MitsuColtCan::kEraseRoutineRamAddr, MitsuColtCan::kEraseRoutineSize));
}

TEST(BenchCommands, UploadRoutineSendsWriteRoutinesToTheWriteRamSlot)
{
    Harness harness;
    harness.session.replies = {
        bytes::Bytes{0x74}, bytes::Bytes{0x76}, bytes::Bytes{0x74}, bytes::Bytes{0x76}, bytes::Bytes{0x71, 0xE1, 0x00},
        bytes::Bytes{0x76}};

    const auto outcome = harness.run(destructiveStep(CommandId::UploadRoutine, {"write-redirect"}));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(harness.session.requests.at(0),
              MitsuColtCan::buildRequestDownload(MitsuColtCan::kWriteRoutineRamAddr, MitsuColtCan::kWriteRoutineSize));
}

TEST(BenchCommands, UploadRoutineFromFileSendsTheFilesBytesInsteadOfTheBakedArray)
{
    // Provenance: --from substitutes the payload bytes but not the RAM
    // address, which still comes from the routine name's slot.
    Harness harness;
    harness.files.contents["custom_erase.bin"] = bytes::Bytes{0x11, 0x22, 0x33, 0x44};
    harness.session.replies = {bytes::Bytes{0x74}, bytes::Bytes{0x76}, bytes::Bytes{0x74}, bytes::Bytes{0x76},
                               bytes::Bytes{0x71, 0xE1, 0x00}};

    const auto outcome =
        harness.run(destructiveStep(CommandId::UploadRoutine, {"erase-redirect", "--from", "custom_erase.bin"}));

    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(harness.session.requests.at(0),
              MitsuColtCan::buildRequestDownload(MitsuColtCan::kEraseRoutineRamAddr, 4));
    EXPECT_EQ(harness.session.requests.at(1), (bytes::Bytes{0x36, 0x11, 0x22, 0x33, 0x44}));
    // 0x11+0x22+0x33+0x44 = 0x00AA, big-endian.
    EXPECT_EQ(harness.session.requests.at(3), (bytes::Bytes{0x36, 0x00, 0xAA}));
}

TEST(BenchCommands, UploadRoutineFromFilePropagatesAMissingFileError)
{
    Harness harness;

    const auto outcome =
        harness.run(destructiveStep(CommandId::UploadRoutine, {"erase-redirect", "--from", "absent.bin"}));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchCommands, UploadRoutineRejectsATypoedFromFlag)
{
    Harness harness;
    harness.files.contents["custom.bin"] = bytes::Bytes{0x99};

    const auto outcome =
        harness.run(destructiveStep(CommandId::UploadRoutine, {"erase-redirect", "--form", "custom.bin"}));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchCommands, UploadRoutineRejectsAFromValueWithoutTheFlag)
{
    Harness harness;
    harness.files.contents["custom.bin"] = bytes::Bytes{0x99};

    const auto outcome = harness.run(destructiveStep(CommandId::UploadRoutine, {"erase-redirect", "custom.bin"}));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(harness.session.requests.empty());
}

TEST(BenchCommands, UploadRoutineRejectsAnUnknownRoutineName)
{
    Harness harness;

    const auto outcome = harness.run(destructiveStep(CommandId::UploadRoutine, {"not-a-routine"}));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchCommands, RunStepRefusesADestructiveStepThatLostItsAcknowledgement)
{
    // Defence in depth: bench_args gates at parse time, but a StepSpec built
    // another way must not reach the wire ungated.
    Harness harness;
    harness.session.replies = {bytes::Bytes{0x71, 0xE0, 0x00}};

    const auto outcome = harness.run(step(CommandId::Erase));

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().kind, ErrorKind::InvalidConfig);
    EXPECT_TRUE(harness.session.requests.empty());
}

} // namespace
} // namespace fastecu::bench
