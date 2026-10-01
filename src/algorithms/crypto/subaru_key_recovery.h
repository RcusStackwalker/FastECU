#pragma once

#include "src/algorithms/protocol/bytes.h"

#include <array>
#include <cstdint>
#include <expected>

// Recovery of the four round keys of the Subaru ROM cipher (the SSM payload
// cipher, SsmProtocol::calculatePayload with kIndexTransformationStock) from a
// plaintext ROM and its encrypted counterpart. Moved from the
// GetKeyOperationsSubaru dialog.
namespace subaru_key_recovery
{

// k1..k4, in the order the rounds apply them.
using Keys = std::array<std::uint16_t, 4>;

enum class Failure
{
    InputTooShort, // either input is shorter than kAnalyzedBytes
    NoMatchingKey, // no k2 or k3 is consistent with more than half the distinct pairs
};

// The attack reads exactly this many leading bytes of each input.
inline constexpr std::size_t kAnalyzedBytes = 0x20000;

std::uint16_t f_function(std::uint16_t word, std::uint16_t key);

// Four rounds under keys, then the final half swap.
std::uint32_t encrypt(std::uint32_t plain, const Keys& keys);

struct Recovery
{
    Keys keys;
    // Pairs whose plaintext word does not occur earlier in the input.
    std::size_t distinct_pairs = 0;
    // Distinct pairs for which encrypt(plain, keys) == cipher.
    std::size_t reproduced_pairs = 0;

    bool operator==(const Recovery&) const = default;
};

// k1 and k4 come from a linear approximation over every distinct plaintext
// word. Each distinct pair then votes for the k2 and k3 values consistent
// with it; the key with the most votes wins, the lowest on a tie, and must
// have more than half of them. A few mismatched words are outvoted, while
// unrelated files or a wrong k1 or k4 fail with NoMatchingKey.
std::expected<Recovery, Failure> recover_keys(bytes::ByteView plain, bytes::ByteView cipher);

} // namespace subaru_key_recovery
