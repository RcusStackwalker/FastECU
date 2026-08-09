#include <gtest/gtest.h>
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/testing/byte_test_utils.h"
using namespace MitsuColtCan;

using test_bytes::bytesFromHex;

TEST(TestMitsuColtCanProtocol, seed_key_word_matches_known_vectors)
{
    // sk = (pk * 135 + 1542) mod 65536, hand-computed from
    // externals/livemonitor/obdengine.cpp's ObdSessionInitCommandSequence.
    ASSERT_EQ(seedKeyWord(0x0000), std::uint16_t(0x0606));
    ASSERT_EQ(seedKeyWord(0x1234), std::uint16_t(0x9F72));
    ASSERT_EQ(seedKeyWord(0xFFFF), std::uint16_t(0x057F));
}
TEST(TestMitsuColtCanProtocol, seed_key_accepts_byte_view_and_returns_bytes)
{
    const bytes::Bytes seed = bytesFromHex("12340000");
    ASSERT_EQ(seedKey(seed), bytesFromHex("9F720606"));
}
TEST(TestMitsuColtCanProtocol, checksum_sums_every_byte_with_wraparound)
{
    ASSERT_EQ(checksum(bytesFromHex("01020304")), std::uint16_t(0x000A));
    ASSERT_EQ(checksum(bytes::Bytes(200, char(0xFF))), std::uint16_t(0xC738));
}
TEST(TestMitsuColtCanProtocol, checksum_accepts_byte_view)
{
    ASSERT_EQ(checksum(bytesFromHex("01020304")), std::uint16_t(0x000A));
    const bytes::Bytes data(200, bytes::Byte{0xFF});
    ASSERT_EQ(checksum(data), std::uint16_t(0xC738));
}
TEST(TestMitsuColtCanProtocol, request_download_frame_layout)
{
    ASSERT_EQ(buildRequestDownload(0x008000, 0x000100), bytesFromHex("3400800000000100"));
}
TEST(TestMitsuColtCanProtocol, transfer_data_frames_chunk_at_256_bytes)
{
    bytes::Bytes payload(300, bytes::Byte{0x5A});
    const std::vector<bytes::Bytes> frames = buildTransferDataFrames(payload);
    ASSERT_EQ(frames.size(), std::size_t(2));
    ASSERT_EQ(frames.at(0).size(), std::size_t(257)); // SID + 256 bytes
    ASSERT_EQ(frames.at(0).at(0), kServiceTransferData);
    ASSERT_EQ(frames.at(1).size(), std::size_t(45)); // SID + 44 remaining bytes
}
TEST(TestMitsuColtCanProtocol, routine_check_crc_selects_flash_vs_memory)
{
    ASSERT_EQ(buildRoutineCheckCrc(0x008000), bytesFromHex("31E102")); // < 0x800000 -> flash (2)
    ASSERT_EQ(buildRoutineCheckCrc(0x805568), bytesFromHex("31E101")); // >= 0x800000 -> memory (1)
}
TEST(TestMitsuColtCanProtocol, routine_erase_frame_is_two_bytes_no_selector)
{
    ASSERT_EQ(buildRoutineErase(), bytesFromHex("31E0"));
}
TEST(TestMitsuColtCanProtocol, request_reflash_unlock_frame_matches_source_bytes)
{
    ASSERT_EQ(buildRequestReflashUnlock(), bytesFromHex("3B9A01015263757330300001"));
}
TEST(TestMitsuColtCanProtocol, read_memory_by_address_frame_layout)
{
    ASSERT_EQ(buildReadMemoryByAddress(0x008000, 192), bytesFromHex("23008000C0"));
}
TEST(TestMitsuColtCanProtocol, diagnostic_session_frame_layout)
{
    ASSERT_EQ(buildDiagnosticSession(kSessionBootload), bytesFromHex("1085"));
    ASSERT_EQ(buildDiagnosticSession(kSessionBasic), bytesFromHex("1081"));
}
TEST(TestMitsuColtCanProtocol, security_access_frame_layout)
{
    ASSERT_EQ(buildSecurityAccessSeedRequest(), bytesFromHex("2705"));
    const bytes::Bytes key = bytesFromHex("9F720606");
    ASSERT_EQ(buildSecurityAccessKey(key), bytesFromHex("27069F720606"));
}
TEST(TestMitsuColtCanProtocol, erase_and_write_page_routines_match_colt_flasher_xml_sizes)
{
    ASSERT_EQ(sizeof(kErasePageRoutine), size_t(160));
    ASSERT_EQ(sizeof(kWritePageRoutine), size_t(176));
    ASSERT_EQ(kErasePageRoutine[0], std::uint8_t(0x94));
    ASSERT_EQ(kWritePageRoutine[0], std::uint8_t(0x94));
}
TEST(TestMitsuColtCanProtocol, erase_and_write_redirect_routines_match_reflash_dir_checksums)
{
    // Checksums verified against mmc-patches/m32r/47110032/reflash/
    // build output -- see
    // docs/superpowers/specs/2026-07-11-z37a-top128k-can-reflash-design.md
    // in the parent claude-hobby project.
    ASSERT_EQ(sizeof(kEraseRedirectRoutine), size_t(192));
    ASSERT_EQ(sizeof(kWriteRedirectRoutine), size_t(188));
    ASSERT_EQ(checksum(kEraseRedirectRoutine), std::uint16_t(0x5079));
    ASSERT_EQ(checksum(kWriteRedirectRoutine), std::uint16_t(0x514e));
}
