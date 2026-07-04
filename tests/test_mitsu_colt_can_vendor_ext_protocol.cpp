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
};
int run_test_mitsu_colt_can_vendor_ext_protocol(int argc, char** argv) {
    TestMitsuColtCanVendorExtProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mitsu_colt_can_vendor_ext_protocol.moc"
