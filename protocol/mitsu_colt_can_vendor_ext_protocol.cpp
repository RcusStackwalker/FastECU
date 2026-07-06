#include "protocol/mitsu_colt_can_vendor_ext_protocol.h"

namespace MitsuColtCanVendorExt {

namespace {

// Bit permutation used at the start of every round. Confirmed to partition
// all 32 bits with zero overlap — see the design doc for the derivation.
// It is its own inverse (a permutation built purely of 2-cycles).
quint32 permuteBits(quint32 x) {
    return ((x & 0x15555555u) << 3)
         | ((x & 0xaaaaaaa8u) >> 3)
         | ((x & 0x40000000u) >> 29)
         | ((x & 0x00000002u) << 29);
}

// Nibble swap within each byte. Also its own inverse: applying it twice is
// a no-op.
quint32 swapNibbles(quint32 x) {
    return ((x & 0x0f0f0f0fu) << 4) | ((x & 0xf0f0f0f0u) >> 4);
}

// Round constants read from ROM flash at 0x5fddc..0x5fddc+15 (big-endian
// u32 each) — literally the first 16 bytes of the "EcuTek ROM Patch" ASCII
// string embedded elsewhere in the same ROM, reinterpreted as words.
constexpr quint32 kRoundConstants[4] = {
    0x45637554u, // "EcuT"
    0x656b2052u, // "ek R"
    0x4f4d2050u, // "OM P"
    0x61746368u, // "atch"
};

} // namespace

quint32 challengeTransform(quint32 secret) {
    quint32 x = secret;
    for (int i = 0; i < 4; ++i) {
        x = swapNibbles(permuteBits(x) + kRoundConstants[i]);
    }
    return x;
}

quint32 challengeInverseTransform(quint32 seed) {
    quint32 x = seed;
    for (int i = 3; i >= 0; --i) {
        x = permuteBits(swapNibbles(x) - kRoundConstants[i]);
    }
    return x;
}

quint32 bytesToSeed(const QByteArray &seedBytes) {
    Q_ASSERT(seedBytes.size() == 4);
    return (quint32(quint8(seedBytes.at(0))) << 24)
         | (quint32(quint8(seedBytes.at(1))) << 16)
         | (quint32(quint8(seedBytes.at(2))) << 8)
         |  quint32(quint8(seedBytes.at(3)));
}

QByteArray keyToBytes(quint32 key) {
    QByteArray bytes;
    bytes.append(char((key >> 24) & 0xFF));
    bytes.append(char((key >> 16) & 0xFF));
    bytes.append(char((key >> 8) & 0xFF));
    bytes.append(char(key & 0xFF));
    return bytes;
}

QByteArray buildChallengeSeedRequestFrame() {
    QByteArray f;
    f.append(char(kServiceSecurityAccess));
    f.append(char(kVendorChallengeSeedSubfunction));
    return f;
}

QByteArray buildChallengeKeyFrame(quint32 key) {
    QByteArray f;
    f.append(char(kServiceSecurityAccess));
    f.append(char(kVendorChallengeKeySubfunction));
    f.append(keyToBytes(key));
    return f;
}

} // namespace MitsuColtCanVendorExt
