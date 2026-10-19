#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace
{
constexpr std::size_t kHeaderLength = 4;
constexpr std::size_t kChecksumLength = 1;
// Any table works for a size property; this is the Hitachi M32R K-Line one.
constexpr std::array<std::uint16_t, 4> kKeyToGenerateIndex{0x78f1, 0x2962, 0x9312, 0x7c03};

bytes::Bytes ToBytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

// Any reply off the wire classifies without a memory fault, and an accepted
// frame is exactly as long as its length byte says. The IDs come from the
// frame itself so mutated seeds keep reaching the checksum comparison.
void ClassifiesAnyReply(const std::string& reply, std::size_t prefix_length)
{
    const bytes::Bytes frame = ToBytes(reply);
    const bytes::Byte receiver = frame.size() > 1 ? frame[1] : 0;
    const bytes::Byte sender = frame.size() > 2 ? frame[2] : 0;
    const bytes::ByteView view(frame);

    const bool valid = ssm_protocol::HasValidFrame(view, receiver, sender);
    if (valid)
    {
        EXPECT_EQ(frame.size(), kHeaderLength + frame[3] + kChecksumLength);
    }

    const bytes::ByteView prefix = view.first(std::min(prefix_length, frame.size()));
    const bool prefixed = ssm_protocol::HasPayloadPrefix(view, prefix, receiver, sender);
    EXPECT_TRUE(valid || !prefixed);
}
FUZZ_TEST(SsmProtocolCoreFuzz, ClassifiesAnyReply)
    .WithDomains(fuzztest::Arbitrary<std::string>(), fuzztest::InRange<std::size_t>(0, 260))
    .WithSeeds(
        []
        {
            std::vector<std::tuple<std::string, std::size_t>> seeds;
            for (auto& [reply] : fuzztest::ReadFilesFromDirectory("src/algorithms/protocol/ssm/testdata/fuzz/replies"))
            {
                seeds.emplace_back(std::move(reply), 0);
            }
            return seeds;
        });

// Every request AddHeader frames is a valid reply for the swapped IDs, and its
// whole payload is a prefix of it.
void AcceptsWhatAddHeaderFrames(const std::vector<std::uint8_t>& payload, bytes::Byte tester, bytes::Byte target)
{
    const bytes::Bytes frame = ssm_protocol::AddHeader(bytes::ByteView(payload), tester, target);
    EXPECT_TRUE(ssm_protocol::HasValidFrame(bytes::ByteView(frame), target, tester));
    EXPECT_TRUE(ssm_protocol::HasPayloadPrefix(bytes::ByteView(frame), bytes::ByteView(payload), target, tester));
}
FUZZ_TEST(SsmProtocolCoreFuzz, AcceptsWhatAddHeaderFrames)
    .WithDomains(fuzztest::VectorOf(fuzztest::Arbitrary<std::uint8_t>()).WithMaxSize(255),
                 fuzztest::Arbitrary<bytes::Byte>(), fuzztest::Arbitrary<bytes::Byte>());

// The payload transform consumes whole 32-bit words of the buffer and drops any
// trailing partial word.
void TransformsWholeWordsOfTheBuffer(const std::vector<std::uint8_t>& buffer)
{
    const bytes::Bytes encrypted =
        ssm_protocol::CalculatePayload(bytes::ByteView(buffer), kKeyToGenerateIndex, ssm_protocol::kStockPairTable);
    const std::size_t words = buffer.size() / 4;
    EXPECT_EQ(encrypted.size(), words * 4);
}
FUZZ_TEST(SsmProtocolCoreFuzz, TransformsWholeWordsOfTheBuffer);
} // namespace
