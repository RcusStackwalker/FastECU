#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace
{
// Every frame BuildCommandFrame produces verifies, and corrupting any byte
// covered by the checksum, or the checksum itself, makes it fail.
void VerifiesOnlyUncorruptedFrames(std::uint8_t cmd, const std::vector<std::uint8_t>& payload, bool freeform,
                                   std::size_t corrupt_at, std::uint8_t flip)
{
    const bytes::Byte trailer = freeform ? mutdma::kTrailerFreeform : mutdma::kTrailerStd;
    mutdma::MutDmaFrame frame = mutdma::BuildCommandFrame(cmd, bytes::ByteView(payload), trailer);
    ASSERT_TRUE(mutdma::VerifyFrame(bytes::ByteView(frame)));

    frame.at(corrupt_at) ^= flip;
    EXPECT_FALSE(mutdma::VerifyFrame(bytes::ByteView(frame)));
}
FUZZ_TEST(MutDmaCodecFuzz, VerifiesOnlyUncorruptedFrames)
    .WithDomains(fuzztest::Arbitrary<std::uint8_t>(),
                 fuzztest::VectorOf(fuzztest::Arbitrary<std::uint8_t>()).WithMaxSize(64), fuzztest::Arbitrary<bool>(),
                 fuzztest::InRange<std::size_t>(0, mutdma::kChecksumOffset), fuzztest::InRange<std::uint8_t>(1, 0xFF));

// Any frame off the wire verifies and parses without a fault; a parsed stream
// frame's data is everything between the log id and the checksum.
void ParsesAnyFrame(const std::vector<std::uint8_t>& frame)
{
    const bytes::ByteView view(frame);
    if (mutdma::VerifyFrame(view))
    {
        EXPECT_EQ(frame.size(), static_cast<std::size_t>(mutdma::kFrameLen));
    }
    const mutdma::StreamFrame parsed = mutdma::ParseStreamFrame(view);
    if (parsed.ok)
    {
        ASSERT_GE(frame.size(), 3U);
        EXPECT_EQ(parsed.log_id, frame[0]);
        EXPECT_EQ(parsed.data, bytes::Bytes(frame.begin() + 1, frame.end() - 2));
    }
}
FUZZ_TEST(MutDmaCodecFuzz, ParsesAnyFrame);
} // namespace
