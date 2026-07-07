#include <QtTest>
#include "protocol/mitsu_colt_can_protocol.h"
#include "byte_test_utils.h"
#include "test_mitsu_colt_can_protocol.h"
using namespace MitsuColtCan;

class TestMitsuColtCanProtocol : public QObject { Q_OBJECT
private slots:
    void seed_key_word_matches_known_vectors() {
        // sk = (pk * 135 + 1542) mod 65536, hand-computed from
        // externals/livemonitor/obdengine.cpp's ObdSessionInitCommandSequence.
        QCOMPARE(seedKeyWord(0x0000), quint16(0x0606));
        QCOMPARE(seedKeyWord(0x1234), quint16(0x9F72));
        QCOMPARE(seedKeyWord(0xFFFF), quint16(0x057F));
    }
    void seed_key_combines_both_halves() {
        QByteArray seed = QByteArray::fromHex("12340000");
        QByteArray key = seedKey(seed);
        QCOMPARE(key.size(), 4);
        QCOMPARE(quint8(key.at(0)), quint8(0x9F));
        QCOMPARE(quint8(key.at(1)), quint8(0x72));
        QCOMPARE(quint8(key.at(2)), quint8(0x06));
        QCOMPARE(quint8(key.at(3)), quint8(0x06));
    }
    void seed_key_accepts_byte_view_and_returns_bytes() {
        const bytes::Bytes seed = test_bytes::bytesFromHex("12340000");
        QCOMPARE(seedKey(seed), test_bytes::bytesFromHex("9F720606"));
    }
    void checksum_sums_every_byte_with_wraparound() {
        QCOMPARE(checksum(QByteArray::fromHex("01020304")), quint16(0x000A));
        QCOMPARE(checksum(QByteArray(200, char(0xFF))), quint16(0xC738));
    }
    void checksum_accepts_byte_view() {
        QCOMPARE(checksum(test_bytes::bytesFromHex("01020304")), std::uint16_t(0x000A));
        const bytes::Bytes data(200, bytes::Byte{0xFF});
        QCOMPARE(checksum(data), std::uint16_t(0xC738));
    }
    void request_download_frame_layout() {
        QByteArray f = buildRequestDownloadFrame(0x008000, 0x000100);
        QCOMPARE(f, QByteArray::fromHex("3400800000000100"));
    }
    void byte_native_request_download_frame_layout() {
        QCOMPARE(buildRequestDownload(0x008000, 0x000100), test_bytes::bytesFromHex("3400800000000100"));
    }
    void transfer_data_frames_chunk_at_256_bytes() {
        QByteArray payload(300, char(0x5A));
        QVector<QByteArray> frames = buildTransferDataFrames(payload);
        QCOMPARE(frames.size(), 2);
        QCOMPARE(frames.at(0).size(), 257); // SID + 256 bytes
        QCOMPARE(quint8(frames.at(0).at(0)), quint8(kServiceTransferData));
        QCOMPARE(frames.at(1).size(), 45);  // SID + 44 remaining bytes
    }
    void byte_native_transfer_data_frames_chunk_at_256_bytes() {
        bytes::Bytes payload(300, bytes::Byte{0x5A});
        const std::vector<bytes::Bytes> frames = buildTransferDataFrames(payload);
        QCOMPARE(frames.size(), std::size_t(2));
        QCOMPARE(frames.at(0).size(), std::size_t(257)); // SID + 256 bytes
        QCOMPARE(frames.at(0).at(0), kServiceTransferData);
        QCOMPARE(frames.at(1).size(), std::size_t(45));  // SID + 44 remaining bytes
    }
    void routine_check_crc_selects_flash_vs_memory() {
        QCOMPARE(buildRoutineCheckCrcFrame(0x008000), QByteArray::fromHex("31E102")); // < 0x800000 -> flash (2)
        QCOMPARE(buildRoutineCheckCrcFrame(0x805568), QByteArray::fromHex("31E101")); // >= 0x800000 -> memory (1)
    }
    void byte_native_routine_check_crc_selects_flash_vs_memory() {
        QCOMPARE(buildRoutineCheckCrc(0x008000), test_bytes::bytesFromHex("31E102")); // < 0x800000 -> flash (2)
        QCOMPARE(buildRoutineCheckCrc(0x805568), test_bytes::bytesFromHex("31E101")); // >= 0x800000 -> memory (1)
    }
    void routine_erase_frame_is_two_bytes_no_selector() {
        QCOMPARE(buildRoutineEraseFrame(), QByteArray::fromHex("31E0"));
    }
    void byte_native_routine_erase_frame_is_two_bytes_no_selector() {
        QCOMPARE(buildRoutineErase(), test_bytes::bytesFromHex("31E0"));
    }
    void request_reflash_unlock_frame_matches_source_bytes() {
        // Ported verbatim from externals/livemonitor/obdsessionwidget.cpp:180-181.
        // The original author's comment on this exact payload: "caused bootloader lockup".
        QByteArray expected = QByteArray::fromHex("3B9A01015263757330300001");
        QCOMPARE(buildRequestReflashUnlockFrame(), expected);
    }
    void byte_native_request_reflash_unlock_frame_matches_source_bytes() {
        QCOMPARE(buildRequestReflashUnlock(), test_bytes::bytesFromHex("3B9A01015263757330300001"));
    }
    void read_memory_by_address_frame_layout() {
        QCOMPARE(buildReadMemoryByAddressFrame(0x008000, 192), QByteArray::fromHex("23008000C0"));
    }
    void byte_native_read_memory_by_address_frame_layout() {
        QCOMPARE(buildReadMemoryByAddress(0x008000, 192), test_bytes::bytesFromHex("23008000C0"));
    }
    void diagnostic_session_frame_layout() {
        QCOMPARE(buildDiagnosticSessionFrame(kSessionBootload), QByteArray::fromHex("1085"));
        QCOMPARE(buildDiagnosticSessionFrame(kSessionBasic), QByteArray::fromHex("1081"));
    }
    void byte_native_diagnostic_session_frame_layout() {
        QCOMPARE(buildDiagnosticSession(kSessionBootload), test_bytes::bytesFromHex("1085"));
        QCOMPARE(buildDiagnosticSession(kSessionBasic), test_bytes::bytesFromHex("1081"));
    }
    void security_access_frame_layout() {
        QCOMPARE(buildSecurityAccessSeedRequestFrame(), QByteArray::fromHex("2705"));
        QByteArray key = QByteArray::fromHex("9F720606");
        QCOMPARE(buildSecurityAccessKeyFrame(key), QByteArray::fromHex("2706") + key);
    }
    void byte_native_security_access_frame_layout() {
        QCOMPARE(buildSecurityAccessSeedRequest(), test_bytes::bytesFromHex("2705"));
        const bytes::Bytes key = test_bytes::bytesFromHex("9F720606");
        QCOMPARE(buildSecurityAccessKey(key), test_bytes::bytesFromHex("27069F720606"));
    }
    void erase_and_write_page_routines_match_colt_flasher_xml_sizes() {
        QCOMPARE(sizeof(kErasePageRoutine), size_t(160));
        QCOMPARE(sizeof(kWritePageRoutine), size_t(176));
        QCOMPARE(kErasePageRoutine[0], quint8(0x94));
        QCOMPARE(kWritePageRoutine[0], quint8(0x94));
    }
};
int run_test_mitsu_colt_can_protocol(int argc, char** argv) {
    TestMitsuColtCanProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_protocol.moc"
