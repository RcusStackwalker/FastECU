#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"

#include <cassert>

namespace MitsuColtCanVendorExt
{

namespace
{

// Bit permutation used at the start of every round. Confirmed to partition
// all 32 bits with zero overlap — see the design doc for the derivation.
// It is its own inverse (a permutation built purely of 2-cycles).
std::uint32_t permuteBits(std::uint32_t x)
{
    return ((x & 0x15555555u) << 3) | ((x & 0xaaaaaaa8u) >> 3) | ((x & 0x40000000u) >> 29) | ((x & 0x00000002u) << 29);
}

// Nibble swap within each byte. Also its own inverse: applying it twice is
// a no-op.
std::uint32_t swapNibbles(std::uint32_t x)
{
    return ((x & 0x0f0f0f0fu) << 4) | ((x & 0xf0f0f0f0u) >> 4);
}

// Round constants read from ROM flash at 0x5fddc..0x5fddc+15 (big-endian
// u32 each) — literally the first 16 bytes of the "EcuTek ROM Patch" ASCII
// string embedded elsewhere in the same ROM, reinterpreted as words.
constexpr std::uint32_t kRoundConstants[4] = {
    0x45637554u, // "EcuT"
    0x656b2052u, // "ek R"
    0x4f4d2050u, // "OM P"
    0x61746368u, // "atch"
};

} // namespace

std::uint32_t challengeTransform(std::uint32_t secret)
{
    std::uint32_t x = secret;
    for (int i = 0; i < 4; ++i)
    {
        x = swapNibbles(permuteBits(x) + kRoundConstants[i]);
    }
    return x;
}

std::uint32_t challengeInverseTransform(std::uint32_t seed)
{
    std::uint32_t x = seed;
    for (int i = 3; i >= 0; --i)
    {
        x = permuteBits(swapNibbles(x) - kRoundConstants[i]);
    }
    return x;
}

std::uint32_t bytesToSeed(bytes::ByteView seedBytes)
{
    assert(seedBytes.size() == 4);
    return bytes::readU32Be(seedBytes);
}

bytes::Bytes keyBytes(std::uint32_t key)
{
    bytes::Bytes bytes;
    bytes.reserve(4);
    bytes::appendU32Be(bytes, key);
    return bytes;
}

bytes::Bytes buildChallengeSeedRequest()
{
    bytes::Bytes f;
    f.reserve(3);
    f.push_back(kServiceReadMemoryByAddress);
    f.push_back(kVendorChallengeSelector);
    f.push_back(kVendorChallengeSeedSubfunction);
    return f;
}

bytes::Bytes buildChallengeKey(std::uint32_t key)
{
    bytes::Bytes f;
    f.reserve(7);
    f.push_back(kServiceReadMemoryByAddress);
    f.push_back(kVendorChallengeSelector);
    f.push_back(kVendorChallengeKeySubfunction);
    bytes::appendU32Be(f, key);
    return f;
}

} // namespace MitsuColtCanVendorExt
