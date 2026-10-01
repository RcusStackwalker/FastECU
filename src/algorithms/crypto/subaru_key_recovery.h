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
    NoMatchingKey, // no 16-bit k2 or k3 is consistent with the last unique pair
};

// The attack reads exactly this many leading bytes of each input.
inline constexpr std::size_t kAnalyzedBytes = 0x20000;

std::uint16_t f_function(std::uint16_t word, std::uint16_t key);

// Four rounds under keys, then the final half swap.
std::uint32_t encrypt(std::uint32_t plain, const Keys& keys);

// k1 and k4 come from a linear approximation over every distinct plaintext
// word; k2 and k3 are then the lowest keys consistent with the last distinct
// word alone. The f-function is not injective in its key, so k2 and k3 can
// differ from the keys that produced the ciphertext.
std::expected<Keys, Failure> recover_keys(bytes::ByteView plain, bytes::ByteView cipher);

} // namespace subaru_key_recovery
