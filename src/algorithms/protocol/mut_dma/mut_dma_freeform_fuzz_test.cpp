#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace
{
std::vector<mutdma::Channel> ToChannels(const std::vector<std::tuple<std::uint16_t, std::uint8_t>>& specs)
{
    std::vector<mutdma::Channel> channels;
    for (const auto& [id, len] : specs)
    {
        channels.push_back({id, len});
    }
    return channels;
}

// Channel widths come from logger definitions, so they are not trusted to be
// 1, 2 or 4. A payload of exactly the channels' total width decodes to one
// little-endian value per channel; any other length decodes to nothing.
void DecodesOnlyACompletePayload(const std::vector<std::tuple<std::uint16_t, std::uint8_t>>& specs,
                                 const std::vector<std::uint8_t>& data)
{
    const std::vector<mutdma::Channel> channels = ToChannels(specs);
    const std::vector<std::uint32_t> values = mutdma::DecodeStreamValues(channels, bytes::ByteView(data));
    if (data.size() != mutdma::ResponseDataLength(channels))
    {
        EXPECT_TRUE(values.empty());
        return;
    }
    ASSERT_EQ(values.size(), channels.size());
    std::size_t offset = 0;
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        const std::size_t width = channels[i].len;
        std::uint32_t expected = 0;
        for (std::size_t b = 0; width <= 4 && b < width; ++b)
        {
            expected |= std::uint32_t(data[offset + b]) << (8 * b);
        }
        EXPECT_EQ(values[i], expected) << "channel " << i << " width " << width;
        offset += width;
    }
}
FUZZ_TEST(MutDmaFreeformFuzz, DecodesOnlyACompletePayload)
    .WithDomains(fuzztest::VectorOf(fuzztest::TupleOf(fuzztest::Arbitrary<std::uint16_t>(),
                                                      fuzztest::InRange<std::uint8_t>(0, 6)))
                     .WithMaxSize(16),
                 fuzztest::VectorOf(fuzztest::Arbitrary<std::uint8_t>()).WithMaxSize(100));

// The same, for payloads built to be complete, so the fuzzer does not have to
// guess the exact length.
void DecodesEveryCompletePayload(const std::vector<std::tuple<std::uint16_t, std::uint8_t>>& specs, std::uint8_t fill)
{
    const std::vector<mutdma::Channel> channels = ToChannels(specs);
    const std::vector<std::uint8_t> data(mutdma::ResponseDataLength(channels), fill);
    EXPECT_EQ(mutdma::DecodeStreamValues(channels, bytes::ByteView(data)).size(), channels.size());
}
FUZZ_TEST(MutDmaFreeformFuzz, DecodesEveryCompletePayload)
    .WithDomains(fuzztest::VectorOf(fuzztest::TupleOf(fuzztest::Arbitrary<std::uint16_t>(),
                                                      fuzztest::InRange<std::uint8_t>(0, 6)))
                     .WithMaxSize(16),
                 fuzztest::Arbitrary<std::uint8_t>());
} // namespace
