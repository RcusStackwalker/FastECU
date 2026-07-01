#include <QtTest>
#include "protocol/mitsu_colt_can_cdbg_protocol.h"
#include "test_mitsu_colt_can_cdbg_protocol.h"
using namespace MitsuColtCanCdbg;

class TestMitsuColtCanCdbgProtocol : public QObject { Q_OBJECT
private slots:
    void init_frame_layout() {
        QCOMPARE(buildInitFrame(), QByteArray::fromHex("0101000000000000"));
    }
    void security_seed_request_frame_layout() {
        QCOMPARE(buildSecuritySeedRequestFrame(), QByteArray::fromHex("1200020000000000"));
    }
    void seed_to_key_matches_known_vectors_across_all_parity_branches() {
        // Hand-verified by simulating cdbgengine.cpp's seed_to_key() in Python
        // (per-byte increment keyed on low 2 bits, 8-bit rotate-left-3, then a
        // parity-keyed byte permutation, then two 16-bit multiply-accumulates).
        // Each case below exercises a different parity (0-4) branch.
        QCOMPARE(seedToKey(0x00000000), quint32(0xAA04C31C)); // parity 0
        QCOMPARE(seedToKey(0x00000009), quint32(0x2104C31C)); // parity 1
        QCOMPARE(seedToKey(0x12345678), quint32(0x8C536B33)); // parity 2
        QCOMPARE(seedToKey(0x00090914), quint32(0x1E0C3241)); // parity 3
        QCOMPARE(seedToKey(0x09090909), quint32(0x1B632D75)); // parity 4 (default branch)
    }
    void extract_seed_reads_big_endian_bytes_4_to_7() {
        QCOMPARE(extractSeed(QByteArray::fromHex("0000000012345678")), quint32(0x12345678));
    }
    void extract_seed_returns_zero_for_short_reply() {
        QCOMPARE(extractSeed(QByteArray::fromHex("001122")), quint32(0));
    }
    void security_key_frame_layout() {
        QCOMPARE(buildSecurityKeyFrame(0x8C536B33), QByteArray::fromHex("13008C536B330000"));
    }
    void security_granted_checks_byte_3() {
        QVERIFY(securityGranted(QByteArray::fromHex("0000000100000000")));
        QVERIFY(!securityGranted(QByteArray::fromHex("0000000000000000")));
    }
    void security_granted_false_for_short_reply() {
        QVERIFY(!securityGranted(QByteArray::fromHex("0011")));
    }
    void log_reset_frame_layout() {
        QCOMPARE(buildLogResetFrame(0), QByteArray::fromHex("1400000000000631"));
        QCOMPARE(buildLogResetFrame(2), QByteArray::fromHex("1400020000000631"));
    }
    void log_start_frame_uses_milliseconds_when_it_fits_in_16_bits() {
        QCOMPARE(buildLogStartFrame(0, 1, 10), QByteArray::fromHex("060001000100000A"));
    }
    void log_start_frame_switches_to_tens_of_ms_above_65535() {
        // 100000 ms > 65535, so unit flag = 1 and the field carries 100000/10 = 10000 = 0x2710.
        QCOMPARE(buildLogStartFrame(0, 3, 100000), QByteArray::fromHex("0600010003012710"));
    }
};
int run_test_mitsu_colt_can_cdbg_protocol(int argc, char** argv) {
    TestMitsuColtCanCdbgProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_cdbg_protocol.moc"
