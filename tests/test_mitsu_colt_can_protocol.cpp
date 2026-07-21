#include <gtest/gtest.h>
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/qt_colt.h"
#include "byte_test_utils.h"
using namespace MitsuColtCan;

TEST(TestMitsuColtCanProtocol, seed_key_word_matches_known_vectors)
{
    // sk = (pk * 135 + 1542) mod 65536, hand-computed from
    // externals/livemonitor/obdengine.cpp's ObdSessionInitCommandSequence.
    ASSERT_EQ(seedKeyWord(0x0000), quint16(0x0606));
    ASSERT_EQ(seedKeyWord(0x1234), quint16(0x9F72));
    ASSERT_EQ(seedKeyWord(0xFFFF), quint16(0x057F));
}
TEST(TestMitsuColtCanProtocol, seed_key_combines_both_halves)
{
    QByteArray seed = QByteArray::fromHex("12340000");
    QByteArray key = seedKey(seed);
    ASSERT_EQ(key.size(), 4);
    ASSERT_EQ(quint8(key.at(0)), quint8(0x9F));
    ASSERT_EQ(quint8(key.at(1)), quint8(0x72));
    ASSERT_EQ(quint8(key.at(2)), quint8(0x06));
    ASSERT_EQ(quint8(key.at(3)), quint8(0x06));
}
TEST(TestMitsuColtCanProtocol, seed_key_accepts_byte_view_and_returns_bytes)
{
    const bytes::Bytes seed = test_bytes::bytesFromHex("12340000");
    ASSERT_EQ(seedKey(seed), test_bytes::bytesFromHex("9F720606"));
}
TEST(TestMitsuColtCanProtocol, checksum_sums_every_byte_with_wraparound)
{
    ASSERT_EQ(checksum(QByteArray::fromHex("01020304")), quint16(0x000A));
    ASSERT_EQ(checksum(QByteArray(200, char(0xFF))), quint16(0xC738));
}
TEST(TestMitsuColtCanProtocol, checksum_accepts_byte_view)
{
    ASSERT_EQ(checksum(test_bytes::bytesFromHex("01020304")), std::uint16_t(0x000A));
    const bytes::Bytes data(200, bytes::Byte{0xFF});
    ASSERT_EQ(checksum(data), std::uint16_t(0xC738));
}
TEST(TestMitsuColtCanProtocol, request_download_frame_layout)
{
    QByteArray f = buildRequestDownloadFrame(0x008000, 0x000100);
    ASSERT_EQ(f, QByteArray::fromHex("3400800000000100"));
}
TEST(TestMitsuColtCanProtocol, byte_native_request_download_frame_layout)
{
    ASSERT_EQ(buildRequestDownload(0x008000, 0x000100), test_bytes::bytesFromHex("3400800000000100"));
}
TEST(TestMitsuColtCanProtocol, transfer_data_frames_chunk_at_256_bytes)
{
    QByteArray payload(300, char(0x5A));
    QVector<QByteArray> frames = buildTransferDataFrames(payload);
    ASSERT_EQ(frames.size(), 2);
    ASSERT_EQ(frames.at(0).size(), 257); // SID + 256 bytes
    ASSERT_EQ(quint8(frames.at(0).at(0)), quint8(kServiceTransferData));
    ASSERT_EQ(frames.at(1).size(), 45); // SID + 44 remaining bytes
}
TEST(TestMitsuColtCanProtocol, byte_native_transfer_data_frames_chunk_at_256_bytes)
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
    ASSERT_EQ(buildRoutineCheckCrcFrame(0x008000), QByteArray::fromHex("31E102")); // < 0x800000 -> flash (2)
    ASSERT_EQ(buildRoutineCheckCrcFrame(0x805568), QByteArray::fromHex("31E101")); // >= 0x800000 -> memory (1)
}
TEST(TestMitsuColtCanProtocol, byte_native_routine_check_crc_selects_flash_vs_memory)
{
    ASSERT_EQ(buildRoutineCheckCrc(0x008000), test_bytes::bytesFromHex("31E102")); // < 0x800000 -> flash (2)
    ASSERT_EQ(buildRoutineCheckCrc(0x805568), test_bytes::bytesFromHex("31E101")); // >= 0x800000 -> memory (1)
}
TEST(TestMitsuColtCanProtocol, routine_erase_frame_is_two_bytes_no_selector)
{
    ASSERT_EQ(buildRoutineEraseFrame(), QByteArray::fromHex("31E0"));
}
TEST(TestMitsuColtCanProtocol, byte_native_routine_erase_frame_is_two_bytes_no_selector)
{
    ASSERT_EQ(buildRoutineErase(), test_bytes::bytesFromHex("31E0"));
}
TEST(TestMitsuColtCanProtocol, request_reflash_unlock_frame_matches_source_bytes)
{
    // Ported verbatim from externals/livemonitor/obdsessionwidget.cpp:180-181.
    // The original author's comment on this exact payload: "caused bootloader lockup".
    QByteArray expected = QByteArray::fromHex("3B9A01015263757330300001");
    ASSERT_EQ(buildRequestReflashUnlockFrame(), expected);
}
TEST(TestMitsuColtCanProtocol, byte_native_request_reflash_unlock_frame_matches_source_bytes)
{
    ASSERT_EQ(buildRequestReflashUnlock(), test_bytes::bytesFromHex("3B9A01015263757330300001"));
}
TEST(TestMitsuColtCanProtocol, read_memory_by_address_frame_layout)
{
    ASSERT_EQ(buildReadMemoryByAddressFrame(0x008000, 192), QByteArray::fromHex("23008000C0"));
}
TEST(TestMitsuColtCanProtocol, byte_native_read_memory_by_address_frame_layout)
{
    ASSERT_EQ(buildReadMemoryByAddress(0x008000, 192), test_bytes::bytesFromHex("23008000C0"));
}
TEST(TestMitsuColtCanProtocol, diagnostic_session_frame_layout)
{
    ASSERT_EQ(buildDiagnosticSessionFrame(kSessionBootload), QByteArray::fromHex("1085"));
    ASSERT_EQ(buildDiagnosticSessionFrame(kSessionBasic), QByteArray::fromHex("1081"));
}
TEST(TestMitsuColtCanProtocol, byte_native_diagnostic_session_frame_layout)
{
    ASSERT_EQ(buildDiagnosticSession(kSessionBootload), test_bytes::bytesFromHex("1085"));
    ASSERT_EQ(buildDiagnosticSession(kSessionBasic), test_bytes::bytesFromHex("1081"));
}
TEST(TestMitsuColtCanProtocol, security_access_frame_layout)
{
    ASSERT_EQ(buildSecurityAccessSeedRequestFrame(), QByteArray::fromHex("2705"));
    QByteArray key = QByteArray::fromHex("9F720606");
    ASSERT_EQ(buildSecurityAccessKeyFrame(key), QByteArray::fromHex("2706") + key);
}
TEST(TestMitsuColtCanProtocol, byte_native_security_access_frame_layout)
{
    ASSERT_EQ(buildSecurityAccessSeedRequest(), test_bytes::bytesFromHex("2705"));
    const bytes::Bytes key = test_bytes::bytesFromHex("9F720606");
    ASSERT_EQ(buildSecurityAccessKey(key), test_bytes::bytesFromHex("27069F720606"));
}
TEST(TestMitsuColtCanProtocol, erase_and_write_page_routines_match_colt_flasher_xml_sizes)
{
    ASSERT_EQ(sizeof(kErasePageRoutine), size_t(160));
    ASSERT_EQ(sizeof(kWritePageRoutine), size_t(176));
    ASSERT_EQ(kErasePageRoutine[0], quint8(0x94));
    ASSERT_EQ(kWritePageRoutine[0], quint8(0x94));
}
TEST(TestMitsuColtCanProtocol, erase_and_write_redirect_routines_match_reflash_dir_checksums)
{
    // Checksums verified against mmc-patches/m32r/47110032/reflash/
    // build output -- see
    // docs/superpowers/specs/2026-07-11-z37a-top128k-can-reflash-design.md
    // in the parent claude-hobby project.
    ASSERT_EQ(sizeof(kEraseRedirectRoutine), size_t(192));
    ASSERT_EQ(sizeof(kWriteRedirectRoutine), size_t(188));
    ASSERT_EQ(checksum(QByteArray(reinterpret_cast<const char *>(kEraseRedirectRoutine), sizeof(kEraseRedirectRoutine))), quint16(0x5079));
    ASSERT_EQ(checksum(QByteArray(reinterpret_cast<const char *>(kWriteRedirectRoutine), sizeof(kWriteRedirectRoutine))), quint16(0x514e));
}
