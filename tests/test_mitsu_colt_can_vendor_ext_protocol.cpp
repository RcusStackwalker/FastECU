#include <QtTest>
#include "protocol/mitsu_colt_can_vendor_ext_protocol.h"
#include "test_mitsu_colt_can_vendor_ext_protocol.h"
using namespace MitsuColtCanVendorExt;

class TestMitsuColtCanVendorExtProtocol : public QObject { Q_OBJECT
private slots:
    void challenge_transform_matches_known_vectors() {
        // Vectors confirmed by exhaustive brute-force cross-check against
        // challengeInverseTransform() across the full 32-bit domain — 0
        // mismatches out of 4294967296. See design doc "Pre-verified facts".
        QCOMPARE(challengeTransform(0x00000000u), quint32(0xF2E207C5u));
        QCOMPARE(challengeTransform(0xFFFFFFFFu), quint32(0xE4C2C64Du));
        QCOMPARE(challengeTransform(0x12345678u), quint32(0x669E0CB4u));
        QCOMPARE(challengeTransform(0x00000001u), quint32(0xE0E207D2u));
        QCOMPARE(challengeTransform(0xDEADBEEFu), quint32(0xA5654127u));
    }
    void challenge_inverse_transform_matches_known_vectors() {
        // Same five vectors as the forward test, inverted.
        QCOMPARE(challengeInverseTransform(0xF2E207C5u), quint32(0x00000000u));
        QCOMPARE(challengeInverseTransform(0xE4C2C64Du), quint32(0xFFFFFFFFu));
        QCOMPARE(challengeInverseTransform(0x669E0CB4u), quint32(0x12345678u));
        QCOMPARE(challengeInverseTransform(0xE0E207D2u), quint32(0x00000001u));
        QCOMPARE(challengeInverseTransform(0xA5654127u), quint32(0xDEADBEEFu));
    }
    void challenge_inverse_round_trips_with_forward() {
        // Lightweight regression check standing in for the one-time
        // exhaustive 2^32 proof recorded in the design doc.
        const quint32 values[] = {0x00000000u, 0xFFFFFFFFu, 0x12345678u,
                                   0x00000001u, 0xDEADBEEFu, 0x80000000u, 0x7FFFFFFFu};
        for (quint32 x : values) {
            QCOMPARE(challengeInverseTransform(challengeTransform(x)), x);
        }
    }
};
int run_test_mitsu_colt_can_vendor_ext_protocol(int argc, char** argv) {
    TestMitsuColtCanVendorExtProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_vendor_ext_protocol.moc"
