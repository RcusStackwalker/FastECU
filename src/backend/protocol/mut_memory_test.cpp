#include "src/backend/protocol/mut_memory.h"

#include <algorithm>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/testing/scripted_kline_transport.h"

using namespace mutdma;
using fastecu::ErrorKind;
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAreArray;

namespace
{
// One read chunk: handshake for `len` one-byte channels, then a stream frame
// carrying `data`.
void script_chunk(ScriptedKlineTransport& t, std::uint16_t addr, const bytes::Bytes& data)
{
    const auto channels = planReadChannels(addr, static_cast<int>(data.size()));
    t.expectWrite(buildSetupFrame(0xA0, static_cast<bytes::Byte>(channels.size())));
    t.queueRead(buildCommandFrame(0xA5, bytes::Bytes{}, TRAILER_STD));
    t.expectWrite(buildIdListFrame(0xA1, channels));
    t.queueRead(buildCommandFrame(0x05, bytes::Bytes{}, TRAILER_STD));
    bytes::Bytes frame{0x51};
    frame.insert(frame.end(), data.begin(), data.end());
    frame.push_back(sum8(frame));
    frame.push_back(TRAILER_STD);
    t.queueRead(frame);
}

bytes::Bytes counting(std::size_t n, bytes::Byte first = 0)
{
    bytes::Bytes out(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        out[i] = static_cast<bytes::Byte>(first + i);
    }
    return out;
}
} // namespace

TEST(MutMemory, WriteBelowTheWindowIsRefusedWithoutIo)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    EXPECT_THAT(write_memory(t, 0x3FFF, bytes::Bytes{0x01}, token), IsErr(ErrorKind::InvalidConfig));
    EXPECT_TRUE(t.scriptConsumed());
}

TEST(MutMemory, WriteAboveTheWindowIsRefusedWithoutIo)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    EXPECT_THAT(write_memory(t, 0xC000, bytes::Bytes{0x01}, token), IsErr(ErrorKind::InvalidConfig));
    EXPECT_TRUE(t.scriptConsumed());
}

TEST(MutMemory, WriteAtBothWindowEdgesReachesTheDriver)
{
    for (const std::uint16_t addr : {std::uint16_t{0x4000}, std::uint16_t{0xBFFF}})
    {
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        const bytes::Bytes data{0xAB};
        t.expectWrite(buildWriteFrames(addr, data).at(0));
        t.queueRead(buildCommandFrame(0x87, bytes::Bytes{0x80, 0x00}, TRAILER_STD));
        EXPECT_THAT(write_memory(t, addr, data, token), IsOk()) << std::hex << addr;
        EXPECT_TRUE(t.scriptConsumed());
    }
}

TEST(MutMemory, ReadSplitsIntoFortyByteChunks)
{
    for (const std::size_t len : {std::size_t{39}, std::size_t{40}, std::size_t{41}, std::size_t{80}})
    {
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        const bytes::Bytes expected = counting(len);
        for (std::size_t off = 0; off < len; off += 40)
        {
            const std::size_t n = std::min<std::size_t>(40, len - off);
            script_chunk(t, static_cast<std::uint16_t>(0x8000 + off),
                         bytes::Bytes(expected.begin() + static_cast<std::ptrdiff_t>(off),
                                      expected.begin() + static_cast<std::ptrdiff_t>(off + n)));
        }
        EXPECT_THAT(read_memory(t, 0x8000, len, token), IsOkAnd(ElementsAreArray(expected))) << len;
        EXPECT_TRUE(t.scriptConsumed()) << len;
    }
}

TEST(MutMemory, ReadReturnsWhatItHadWhenALaterChunkFails)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    const bytes::Bytes first = counting(40);
    script_chunk(t, 0x8000, first);
    t.expectWrite(buildSetupFrame(0xA0, 40));
    t.queue_error(ErrorKind::Disconnected);
    EXPECT_THAT(read_memory(t, 0x8000, 80, token), IsOkAnd(ElementsAreArray(first)));
}

TEST(MutMemory, ReadFailsWhenTheFirstChunkFails)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    t.expectWrite(buildSetupFrame(0xA0, 4));
    t.queue_error(ErrorKind::Disconnected);
    EXPECT_THAT(read_memory(t, 0x8000, 4, token), IsErr(ErrorKind::Disconnected));
}

TEST(MutMemory, ReadSkipsAChunkWhosePollReturnsNoFrame)
{
    // Today's loop only stops on a poll *error*; an empty poll appends nothing
    // and moves to the next chunk.
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    const auto channels = planReadChannels(0x8000, 40);
    t.expectWrite(buildSetupFrame(0xA0, 40));
    t.queueRead(buildCommandFrame(0xA5, bytes::Bytes{}, TRAILER_STD));
    t.expectWrite(buildIdListFrame(0xA1, channels));
    t.queueRead(buildCommandFrame(0x05, bytes::Bytes{}, TRAILER_STD));
    t.queue_no_frame();
    const bytes::Bytes second = counting(10, 0x40);
    script_chunk(t, 0x8028, second);
    EXPECT_THAT(read_memory(t, 0x8000, 50, token), IsOkAnd(ElementsAreArray(second)));
}
