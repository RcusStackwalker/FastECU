#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"

#include <gtest/gtest.h>

// Portable-type mirror of tests/test_mitsu_colt_can_protocol.cpp,
// tests/test_mitsu_colt_can_vendor_ext_protocol.cpp, and
// tests/test_mitsu_colt_can_cdbg_protocol.cpp -- same input/output vectors
// for all three Colt protocol variants, expressed with bytes::Bytes /
// bytes::ByteView / std::vector instead of QByteArray / QVector, exercised
// again here so this package's own test target proves the portable API
// round-trips without depending on //tests or linking Qt.

TEST(MitsuColtCanPortable, SeedKeyWordMatchesKnownVectors)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(seedKeyWord(0x0000), std::uint16_t(0x0606));
    ASSERT_EQ(seedKeyWord(0x1234), std::uint16_t(0x9F72));
    ASSERT_EQ(seedKeyWord(0xFFFF), std::uint16_t(0x057F));
}

TEST(MitsuColtCanPortable, SeedKeyCombinesBothHalves)
{
    using namespace MitsuColtCan;
    const bytes::Bytes seed = {0x12, 0x34, 0x00, 0x00};
    ASSERT_EQ(seedKey(seed), bytes::Bytes({0x9F, 0x72, 0x06, 0x06}));
}

TEST(MitsuColtCanPortable, ChecksumSumsEveryByteWithWraparound)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(checksum(bytes::Bytes({0x01, 0x02, 0x03, 0x04})), std::uint16_t(0x000A));
    const bytes::Bytes data(200, bytes::Byte{0xFF});
    ASSERT_EQ(checksum(data), std::uint16_t(0xC738));
}

TEST(MitsuColtCanPortable, RequestDownloadFrameLayout)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(buildRequestDownload(0x008000, 0x000100),
              bytes::Bytes({0x34, 0x00, 0x80, 0x00, 0x00, 0x00, 0x01, 0x00}));
}

TEST(MitsuColtCanPortable, TransferDataFramesChunkAt256Bytes)
{
    using namespace MitsuColtCan;
    bytes::Bytes payload(300, bytes::Byte{0x5A});
    const std::vector<bytes::Bytes> frames = buildTransferDataFrames(payload);
    ASSERT_EQ(frames.size(), std::size_t(2));
    ASSERT_EQ(frames.at(0).size(), std::size_t(257)); // SID + 256 bytes
    ASSERT_EQ(frames.at(0).at(0), kServiceTransferData);
    ASSERT_EQ(frames.at(1).size(), std::size_t(45)); // SID + 44 remaining bytes
}

TEST(MitsuColtCanPortable, RoutineCheckCrcSelectsFlashVsMemory)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(buildRoutineCheckCrc(0x008000), bytes::Bytes({0x31, 0xE1, 0x02})); // < 0x800000 -> flash (2)
    ASSERT_EQ(buildRoutineCheckCrc(0x805568), bytes::Bytes({0x31, 0xE1, 0x01})); // >= 0x800000 -> memory (1)
}

TEST(MitsuColtCanPortable, RoutineEraseFrameIsTwoBytesNoSelector)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(buildRoutineErase(), bytes::Bytes({0x31, 0xE0}));
}

TEST(MitsuColtCanPortable, RequestReflashUnlockFrameMatchesSourceBytes)
{
    using namespace MitsuColtCan;
    // Ported verbatim from externals/livemonitor/obdsessionwidget.cpp:180-181.
    // The original author's comment on this exact payload: "caused bootloader lockup".
    const bytes::Bytes expected = {
        0x3B, 0x9A, 0x01, 0x01, 'R', 'c', 'u', 's', '0', '0', 0x00, 0x01};
    ASSERT_EQ(buildRequestReflashUnlock(), expected);
}

TEST(MitsuColtCanPortable, ReadMemoryByAddressFrameLayout)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(buildReadMemoryByAddress(0x008000, 192), bytes::Bytes({0x23, 0x00, 0x80, 0x00, 0xC0}));
}

TEST(MitsuColtCanPortable, DiagnosticSessionFrameLayout)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(buildDiagnosticSession(kSessionBootload), bytes::Bytes({0x10, 0x85}));
    ASSERT_EQ(buildDiagnosticSession(kSessionBasic), bytes::Bytes({0x10, 0x81}));
}

TEST(MitsuColtCanPortable, SecurityAccessFrameLayout)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(buildSecurityAccessSeedRequest(), bytes::Bytes({0x27, 0x05}));
    const bytes::Bytes key = {0x9F, 0x72, 0x06, 0x06};
    ASSERT_EQ(buildSecurityAccessKey(key), bytes::Bytes({0x27, 0x06, 0x9F, 0x72, 0x06, 0x06}));
}

TEST(MitsuColtCanPortable, EraseAndWritePageRoutinesMatchColtFlasherXmlSizes)
{
    using namespace MitsuColtCan;
    ASSERT_EQ(sizeof(kErasePageRoutine), size_t(160));
    ASSERT_EQ(sizeof(kWritePageRoutine), size_t(176));
    ASSERT_EQ(kErasePageRoutine[0], bytes::Byte(0x94));
    ASSERT_EQ(kWritePageRoutine[0], bytes::Byte(0x94));
}

TEST(MitsuColtCanPortable, EraseAndWriteRedirectRoutinesMatchReflashDirChecksums)
{
    using namespace MitsuColtCan;
    // Checksums verified against mmc-patches/m32r/47110032/reflash/
    // build output -- see
    // docs/superpowers/specs/2026-07-11-z37a-top128k-can-reflash-design.md
    // in the parent claude-hobby project.
    ASSERT_EQ(sizeof(kEraseRedirectRoutine), size_t(192));
    ASSERT_EQ(sizeof(kWriteRedirectRoutine), size_t(188));
    ASSERT_EQ(checksum(bytes::ByteView(kEraseRedirectRoutine, sizeof(kEraseRedirectRoutine))), std::uint16_t(0x5079));
    ASSERT_EQ(checksum(bytes::ByteView(kWriteRedirectRoutine, sizeof(kWriteRedirectRoutine))), std::uint16_t(0x514e));
}

TEST(MitsuColtCanVendorExtPortable, ChallengeTransformMatchesKnownVectors)
{
    using namespace MitsuColtCanVendorExt;
    // Vectors confirmed by exhaustive brute-force cross-check against
    // challengeInverseTransform() across the full 32-bit domain — 0
    // mismatches out of 4294967296. See design doc "Pre-verified facts".
    ASSERT_EQ(challengeTransform(0x00000000u), std::uint32_t(0xF2E207C5u));
    ASSERT_EQ(challengeTransform(0xFFFFFFFFu), std::uint32_t(0xE4C2C64Du));
    ASSERT_EQ(challengeTransform(0x12345678u), std::uint32_t(0x669E0CB4u));
    ASSERT_EQ(challengeTransform(0x00000001u), std::uint32_t(0xE0E207D2u));
    ASSERT_EQ(challengeTransform(0xDEADBEEFu), std::uint32_t(0xA5654127u));
}

TEST(MitsuColtCanVendorExtPortable, ChallengeInverseTransformMatchesKnownVectors)
{
    using namespace MitsuColtCanVendorExt;
    // Same five vectors as the forward test, inverted.
    ASSERT_EQ(challengeInverseTransform(0xF2E207C5u), std::uint32_t(0x00000000u));
    ASSERT_EQ(challengeInverseTransform(0xE4C2C64Du), std::uint32_t(0xFFFFFFFFu));
    ASSERT_EQ(challengeInverseTransform(0x669E0CB4u), std::uint32_t(0x12345678u));
    ASSERT_EQ(challengeInverseTransform(0xE0E207D2u), std::uint32_t(0x00000001u));
    ASSERT_EQ(challengeInverseTransform(0xA5654127u), std::uint32_t(0xDEADBEEFu));
}

TEST(MitsuColtCanVendorExtPortable, ChallengeInverseRoundTripsWithForward)
{
    using namespace MitsuColtCanVendorExt;
    // Lightweight regression check standing in for the one-time
    // exhaustive 2^32 proof recorded in the design doc.
    const std::uint32_t values[] = {0x00000000u, 0xFFFFFFFFu, 0x12345678u,
                                    0x00000001u, 0xDEADBEEFu, 0x80000000u, 0x7FFFFFFFu};
    for (std::uint32_t x : values)
    {
        ASSERT_EQ(challengeInverseTransform(challengeTransform(x)), x);
    }
}

TEST(MitsuColtCanVendorExtPortable, SeedAndKeyHelpersRoundTrip)
{
    using namespace MitsuColtCanVendorExt;
    const bytes::Bytes seedBytes = {0xF2, 0xE2, 0x07, 0xC5};
    ASSERT_EQ(bytesToSeed(seedBytes), std::uint32_t(0xF2E207C5u));
    ASSERT_EQ(keyBytes(0xF2E207C5u), seedBytes);

    const std::uint32_t secret = 0x12345678u;
    const bytes::Bytes onWire = keyBytes(challengeTransform(secret));
    ASSERT_EQ(challengeInverseTransform(bytesToSeed(onWire)), secret);
}

TEST(MitsuColtCanVendorExtPortable, ChallengeFrameLayout)
{
    using namespace MitsuColtCanVendorExt;
    ASSERT_EQ(buildChallengeSeedRequest(), bytes::Bytes({0x23, 0x27, 0x41}));
    ASSERT_EQ(buildChallengeKey(0x12345678u), bytes::Bytes({0x23, 0x27, 0x42, 0x12, 0x34, 0x56, 0x78}));
}

TEST(MitsuColtCanVendorExtPortable, ChallengeKeyFrameUsesInverseKeyBytes)
{
    using namespace MitsuColtCanVendorExt;
    const bytes::Bytes seedBytes = {0x66, 0x9E, 0x0C, 0xB4};
    const std::uint32_t key = challengeInverseTransform(bytesToSeed(seedBytes));
    ASSERT_EQ(keyBytes(key), bytes::Bytes({0x12, 0x34, 0x56, 0x78}));
    ASSERT_EQ(buildChallengeKey(key), bytes::Bytes({0x23, 0x27, 0x42, 0x12, 0x34, 0x56, 0x78}));
}

TEST(MitsuColtCanCdbgPortable, InitFrameLayout)
{
    using namespace MitsuColtCanCdbg;
    const CdbgFrame expected{0x01, 0x01, 0, 0, 0, 0, 0, 0};
    ASSERT_TRUE(buildInitFrame() == expected);
}

TEST(MitsuColtCanCdbgPortable, SecuritySeedRequestFrameLayout)
{
    using namespace MitsuColtCanCdbg;
    const CdbgFrame expected{0x12, 0, 0x02, 0, 0, 0, 0, 0};
    ASSERT_TRUE(buildSecuritySeedRequestFrame() == expected);
}

TEST(MitsuColtCanCdbgPortable, SeedToKeyMatchesKnownVectorsAcrossAllParityBranches)
{
    using namespace MitsuColtCanCdbg;
    // Hand-verified by simulating cdbgengine.cpp's seed_to_key() in Python
    // (per-byte increment keyed on low 2 bits, 8-bit rotate-left-3, then a
    // parity-keyed byte permutation, then two 16-bit multiply-accumulates).
    // Each case below exercises a different parity (0-4) branch.
    ASSERT_EQ(seedToKey(0x00000000), std::uint32_t(0xAA04C31C)); // parity 0
    ASSERT_EQ(seedToKey(0x00000009), std::uint32_t(0x2104C31C)); // parity 1
    ASSERT_EQ(seedToKey(0x12345678), std::uint32_t(0x8C536B33)); // parity 2
    ASSERT_EQ(seedToKey(0x00090914), std::uint32_t(0x1E0C3241)); // parity 3
    ASSERT_EQ(seedToKey(0x09090909), std::uint32_t(0x1B632D75)); // parity 4 (default branch)
    ASSERT_EQ(seedToKey(0xD61B2EEA), std::uint32_t(0xBA80A2C1)); // live ECU log sample
}

TEST(MitsuColtCanCdbgPortable, ExtractSeedReadsBigEndianBytes4To7)
{
    using namespace MitsuColtCanCdbg;
    const CdbgFrame reply{0, 0, 0, 0, 0x12, 0x34, 0x56, 0x78};
    ASSERT_EQ(extractSeed(reply), std::uint32_t(0x12345678));
}

TEST(MitsuColtCanCdbgPortable, ExtractSeedReturnsZeroForShortReply)
{
    using namespace MitsuColtCanCdbg;
    const bytes::Bytes reply{0x00, 0x11, 0x22};
    ASSERT_EQ(extractSeed(reply), std::uint32_t(0));
}

TEST(MitsuColtCanCdbgPortable, SecurityKeyFrameLayout)
{
    using namespace MitsuColtCanCdbg;
    const CdbgFrame expected{0x13, 0, 0x8C, 0x53, 0x6B, 0x33, 0, 0};
    ASSERT_TRUE(buildSecurityKeyFrame(0x8C536B33) == expected);
}

TEST(MitsuColtCanCdbgPortable, SecurityGrantedChecksByte3)
{
    using namespace MitsuColtCanCdbg;
    const CdbgFrame granted{0, 0, 0, 1, 0, 0, 0, 0};
    const CdbgFrame liveGranted{0xFF, 0, 0, 2, 0xD6, 0x1B, 0x2E, 0xEA};
    const CdbgFrame denied{0, 0, 0, 0, 0, 0, 0, 0};
    ASSERT_TRUE(securityGranted(granted));
    ASSERT_TRUE(securityGranted(liveGranted));
    ASSERT_TRUE(!securityGranted(denied));
}

TEST(MitsuColtCanCdbgPortable, SecurityGrantedFalseForShortReply)
{
    using namespace MitsuColtCanCdbg;
    const bytes::Bytes reply{0x00, 0x11};
    ASSERT_TRUE(!securityGranted(reply));
}

TEST(MitsuColtCanCdbgPortable, LogResetFrameLayout)
{
    using namespace MitsuColtCanCdbg;
    const CdbgFrame instance0{0x14, 0, 0, 0, 0, 0, 0x06, 0x31};
    const CdbgFrame instance2{0x14, 0, 2, 0, 0, 0, 0x06, 0x31};
    ASSERT_TRUE(buildLogResetFrame(0) == instance0);
    ASSERT_TRUE(buildLogResetFrame(2) == instance2);
}

TEST(MitsuColtCanCdbgPortable, LogStartFrameUsesMillisecondsWhenItFitsIn16Bits)
{
    using namespace MitsuColtCanCdbg;
    const CdbgFrame expected{0x06, 0, 1, 0, 1, 0, 0, 0x0A};
    ASSERT_TRUE(buildLogStartFrame(0, 1, 10) == expected);
}

TEST(MitsuColtCanCdbgPortable, LogStartFrameSwitchesToTensOfMsAbove65535)
{
    using namespace MitsuColtCanCdbg;
    // 100000 ms > 65535, so unit flag = 1 and the field carries 100000/10 = 10000 = 0x2710.
    const CdbgFrame expected{0x06, 0, 1, 0, 3, 1, 0x27, 0x10};
    ASSERT_TRUE(buildLogStartFrame(0, 3, 100000) == expected);
}

TEST(MitsuColtCanCdbgPortable, BatchingPacksChannelsIntoOneFrameWhenTheyFit)
{
    using namespace MitsuColtCanCdbg;
    std::vector<CdbgChannel> channels = {{0x804FBF, 1}, {0x804DF2, 2}};
    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(batchChannelsIntoFrames(channels, frames));
    ASSERT_EQ(frames.size(), std::size_t(1));
    ASSERT_EQ(frames.at(0).size(), std::size_t(2));
}

TEST(MitsuColtCanCdbgPortable, BatchingStartsANewFrameWhenTheNextChannelWouldOverflow)
{
    using namespace MitsuColtCanCdbg;
    // byteIndex starts at 1 (byte 0 is the frame-index marker). Two
    // 4-byte channels can't share a frame (1+4=5 ok, but the second
    // 4-byte channel would need 5+4=9 > 8), so it spills into frame 1
    // along with the following 2-byte channel (5+2=7 <= 8).
    std::vector<CdbgChannel> channels = {{0x804FBF, 4}, {0x804DF2, 4}, {0x8054AC, 2}};
    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(batchChannelsIntoFrames(channels, frames));
    ASSERT_EQ(frames.size(), std::size_t(2));
    ASSERT_EQ(frames.at(0).size(), std::size_t(1));
    ASSERT_EQ(frames.at(1).size(), std::size_t(2));
}

TEST(MitsuColtCanCdbgPortable, BatchingRejectsEmptyChannelList)
{
    using namespace MitsuColtCanCdbg;
    std::vector<CdbgChannel> channels;
    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(!batchChannelsIntoFrames(channels, frames));
}

TEST(MitsuColtCanCdbgPortable, BatchingRejectsMoreThanKMaxFramesFrames)
{
    using namespace MitsuColtCanCdbg;
    // 9 single-byte-incompatible channels forcing 9 frames (one 4-byte
    // channel per frame, since 1+4=5 <= 8 but a second 4-byte channel
    // would overflow) - one more than kMaxFrames (8).
    std::vector<CdbgChannel> channels;
    for (int i = 0; i < 9; ++i)
    {
        channels.push_back(CdbgChannel{std::uint32_t(0x800000 + i), 4});
    }
    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(!batchChannelsIntoFrames(channels, frames));
}

TEST(MitsuColtCanCdbgPortable, FrameInitFramesLayoutForTwoItems)
{
    using namespace MitsuColtCanCdbg;
    std::vector<CdbgChannel> frameItems = {{0x804FBF, 1}, {0x804DF2, 2}};
    const std::vector<CdbgFrame> cmds = buildFrameInitFrames(0, 0, frameItems);
    ASSERT_EQ(cmds.size(), std::size_t(4));
    const CdbgFrame select0{0x15, 0, 0, 0, 0, 0, 0, 0};
    const CdbgFrame pointer0{0x16, 0, 1, 0, 0, 0x80, 0x4F, 0xBF};
    const CdbgFrame select1{0x15, 0, 0, 0, 1, 0, 0, 0};
    const CdbgFrame pointer1{0x16, 0, 2, 0, 0, 0x80, 0x4D, 0xF2};
    ASSERT_TRUE(cmds.at(0) == select0);  // select item 0
    ASSERT_TRUE(cmds.at(1) == pointer0); // pointer/size item 0
    ASSERT_TRUE(cmds.at(2) == select1);  // select item 1
    ASSERT_TRUE(cmds.at(3) == pointer1); // pointer/size item 1
}

TEST(MitsuColtCanCdbgPortable, DecodeFrameReadsBigEndianValuesAtTheRightOffsets)
{
    using namespace MitsuColtCanCdbg;
    std::vector<CdbgChannel> frameItems = {{0x804FBF, 1}, {0x804DF2, 2}};
    const CdbgFrame frame{0, 0x2A, 0x12, 0x34, 0, 0, 0, 0};
    std::vector<std::uint32_t> values = decodeFrame(0, frameItems, frame);
    ASSERT_EQ(values.size(), std::size_t(2));
    ASSERT_EQ(values.at(0), std::uint32_t(0x2A));
    ASSERT_EQ(values.at(1), std::uint32_t(0x1234));
}

TEST(MitsuColtCanCdbgPortable, DecodeFrameRejectsMismatchedFrameIndex)
{
    using namespace MitsuColtCanCdbg;
    std::vector<CdbgChannel> frameItems = {{0x804FBF, 1}};
    const CdbgFrame frame{1, 0x2A, 0, 0, 0, 0, 0, 0}; // index byte is 1, not 0
    ASSERT_TRUE(decodeFrame(0, frameItems, frame).empty());
}

TEST(MitsuColtCanCdbgPortable, DecodeFrameRejectsTooShortFrame)
{
    using namespace MitsuColtCanCdbg;
    std::vector<CdbgChannel> frameItems = {{0x804FBF, 4}};
    const bytes::Bytes frame{0x00, 0x11}; // needs 1+4=5 bytes, only has 2
    ASSERT_TRUE(decodeFrame(0, frameItems, frame).empty());
}
