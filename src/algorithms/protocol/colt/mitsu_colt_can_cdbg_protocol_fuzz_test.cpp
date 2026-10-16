#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <vector>

#include <fuzztest/fuzztest.h>
#include <gtest/gtest.h>

namespace
{
using mitsu_colt_can_cdbg::CdbgChannel;

std::vector<CdbgChannel> ToChannels(const std::vector<std::tuple<std::uint32_t, std::uint8_t>>& specs)
{
    std::vector<CdbgChannel> channels;
    for (const auto& [pointer, size] : specs)
    {
        channels.push_back({pointer, size});
    }
    return channels;
}

// Security replies of any length parse without a fault, with the documented
// short-reply fallbacks.
void ParsesAnySecurityReply(const std::vector<std::uint8_t>& reply)
{
    const bytes::ByteView view(reply);
    const std::uint32_t seed = mitsu_colt_can_cdbg::ExtractSeed(view);
    if (reply.size() < 8)
    {
        EXPECT_EQ(seed, 0U);
    }
    EXPECT_EQ(mitsu_colt_can_cdbg::SecurityGranted(view), reply.size() >= 4 && reply[3] != 0);
}
FUZZ_TEST(MitsuColtCdbgFuzz, ParsesAnySecurityReply);

// A streamed frame decodes to one value per channel only when it starts with
// the expected index and holds every channel; otherwise to nothing.
void DecodesAnyStreamedFrame(std::uint8_t expected_index,
                             const std::vector<std::tuple<std::uint32_t, std::uint8_t>>& specs,
                             const std::vector<std::uint8_t>& frame)
{
    const std::vector<CdbgChannel> channels = ToChannels(specs);
    std::size_t need = 1;
    for (const CdbgChannel& channel : channels)
    {
        need += channel.size;
    }
    const bool decodable = !frame.empty() && frame[0] == expected_index && frame.size() >= need;
    const std::vector<std::uint32_t> values =
        mitsu_colt_can_cdbg::DecodeFrame(expected_index, channels, bytes::ByteView(frame));
    EXPECT_EQ(values.size(), decodable ? channels.size() : 0U);
}
FUZZ_TEST(MitsuColtCdbgFuzz, DecodesAnyStreamedFrame)
    .WithDomains(fuzztest::InRange<std::uint8_t>(0, 3),
                 fuzztest::VectorOf(fuzztest::TupleOf(fuzztest::Arbitrary<std::uint32_t>(),
                                                      fuzztest::InRange<std::uint8_t>(0, 6)))
                     .WithMaxSize(8),
                 fuzztest::VectorOf(fuzztest::Arbitrary<std::uint8_t>()).WithMaxSize(16));

// Batching never loses or reorders a channel, and every frame fits the 7-byte
// payload of a streamed reply.
void BatchesChannelsWithinTheFrameLimits(const std::vector<std::tuple<std::uint32_t, std::uint8_t>>& specs)
{
    const std::vector<CdbgChannel> channels = ToChannels(specs);
    std::vector<std::vector<CdbgChannel>> frames;
    if (!mitsu_colt_can_cdbg::BatchChannelsIntoFrames(channels, frames))
    {
        EXPECT_TRUE(frames.empty());
        return;
    }
    EXPECT_LE(frames.size(), static_cast<std::size_t>(mitsu_colt_can_cdbg::kMaxFrames));
    std::size_t next = 0;
    for (const std::vector<CdbgChannel>& frame : frames)
    {
        std::size_t bytes_used = 0;
        for (const CdbgChannel& channel : frame)
        {
            bytes_used += channel.size;
            ASSERT_LT(next, channels.size());
            EXPECT_EQ(channel.pointer, channels[next].pointer);
            ++next;
        }
        EXPECT_LE(bytes_used, static_cast<std::size_t>(mitsu_colt_can_cdbg::kMaxFrameBytes));
    }
    EXPECT_EQ(next, channels.size());
}
FUZZ_TEST(MitsuColtCdbgFuzz, BatchesChannelsWithinTheFrameLimits)
    .WithDomains(fuzztest::VectorOf(fuzztest::TupleOf(fuzztest::Arbitrary<std::uint32_t>(),
                                                      fuzztest::ElementOf<std::uint8_t>({1, 2, 4})))
                     .WithMaxSize(64));
} // namespace
