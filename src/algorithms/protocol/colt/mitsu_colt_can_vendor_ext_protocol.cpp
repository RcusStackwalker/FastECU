#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"

#include <cassert>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_pdu.h"
#include <array>

namespace mitsu_colt_can_vendor_ext
{
using bytes::ComposeBe;

namespace
{

// Bit permutation used at the start of every round. Confirmed to partition
// all 32 bits with zero overlap — see the design doc for the derivation.
// It is its own inverse (a permutation built purely of 2-cycles).
std::uint32_t PermuteBits(std::uint32_t x)
{
    return ((x & 0x15555555U) << 3) | ((x & 0xaaaaaaa8U) >> 3) | ((x & 0x40000000U) >> 29) | ((x & 0x00000002U) << 29);
}

// Nibble swap within each byte. Also its own inverse: applying it twice is
// a no-op.
std::uint32_t SwapNibbles(std::uint32_t x)
{
    return ((x & 0x0f0f0f0fU) << 4) | ((x & 0xf0f0f0f0U) >> 4);
}

// Round constants read from ROM flash at 0x5fddc..0x5fddc+15 (big-endian
// u32 each) — literally the first 16 bytes of the "EcuTek ROM Patch" ASCII
// string embedded elsewhere in the same ROM, reinterpreted as words.
constexpr std::array<std::uint32_t, 4> kRoundConstants{
    0x45637554U, // "EcuT"
    0x656b2052U, // "ek R"
    0x4f4d2050U, // "OM P"
    0x61746368U, // "atch"
};

} // namespace

std::uint32_t ChallengeTransform(std::uint32_t secret)
{
    std::uint32_t x = secret;
    for (int i = 0; i < 4; ++i)
    {
        x = SwapNibbles(PermuteBits(x) + kRoundConstants[i]);
    }
    return x;
}

std::uint32_t ChallengeInverseTransform(std::uint32_t seed)
{
    std::uint32_t x = seed;
    for (int i = 3; i >= 0; --i)
    {
        x = PermuteBits(SwapNibbles(x) - kRoundConstants[i]);
    }
    return x;
}

std::uint32_t BytesToSeed(bytes::ByteView seed_bytes)
{
    assert(seed_bytes.size() == 4);
    return bytes::ReadU32Be(seed_bytes);
}

bytes::Bytes KeyBytes(std::uint32_t key)
{
    return ComposeBe(key);
}

bytes::Bytes BuildChallengeSeedRequest()
{
    return uds::BuildRequest(kServiceReadMemoryByAddress,
                             ComposeBe(kVendorChallengeSelector, kVendorChallengeSeedSubfunction));
}

bytes::Bytes BuildChallengeKey(std::uint32_t key)
{
    return uds::BuildRequest(kServiceReadMemoryByAddress,
                             ComposeBe(kVendorChallengeSelector, kVendorChallengeKeySubfunction, key));
}

} // namespace mitsu_colt_can_vendor_ext
