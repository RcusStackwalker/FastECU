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
void ScriptChunk(ScriptedKlineTransport& t, std::uint16_t addr, const bytes::Bytes& data)
{
    const auto channels = PlanReadChannels(addr, static_cast<int>(data.size()));
    t.ExpectWrite(BuildSetupFrame(0xA0, static_cast<bytes::Byte>(channels.size())));
    t.QueueRead(BuildCommandFrame(0xA5, bytes::Bytes{}, kTrailerStd));
    t.ExpectWrite(BuildIdListFrame(0xA1, channels));
    t.QueueRead(BuildCommandFrame(0x05, bytes::Bytes{}, kTrailerStd));
    bytes::Bytes frame{0x51};
    frame.insert(frame.end(), data.begin(), data.end());
    frame.push_back(Sum8(frame));
    frame.push_back(kTrailerStd);
    t.QueueRead(frame);
}

bytes::Bytes Counting(std::size_t n, bytes::Byte first = 0)
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
    EXPECT_THAT(WriteMemory(t, 0x3FFF, bytes::Bytes{0x01}, token), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(t.ScriptConsumed());
}

TEST(MutMemory, WriteAboveTheWindowIsRefusedWithoutIo)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    EXPECT_THAT(WriteMemory(t, 0xC000, bytes::Bytes{0x01}, token), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(t.ScriptConsumed());
}

TEST(MutMemory, WriteAtBothWindowEdgesReachesTheDriver)
{
    for (const std::uint16_t addr : {std::uint16_t{0x4000}, std::uint16_t{0xBFFF}})
    {
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        const bytes::Bytes data{0xAB};
        t.ExpectWrite(BuildWriteFrames(addr, data).at(0));
        t.QueueRead(BuildCommandFrame(0x87, bytes::Bytes{0x80, 0x00}, kTrailerStd));
        EXPECT_THAT(WriteMemory(t, addr, data, token), IsOk()) << std::hex << addr;
        EXPECT_TRUE(t.ScriptConsumed());
    }
}

TEST(MutMemory, ReadSplitsIntoFortyByteChunks)
{
    for (const std::size_t len : {std::size_t{39}, std::size_t{40}, std::size_t{41}, std::size_t{80}})
    {
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        const bytes::Bytes expected = Counting(len);
        for (std::size_t off = 0; off < len; off += 40)
        {
            const std::size_t n = std::min<std::size_t>(40, len - off);
            ScriptChunk(t, static_cast<std::uint16_t>(0x8000 + off),
                        bytes::Bytes(expected.begin() + static_cast<std::ptrdiff_t>(off),
                                     expected.begin() + static_cast<std::ptrdiff_t>(off + n)));
        }
        EXPECT_THAT(ReadMemory(t, 0x8000, len, token), IsOkAnd(ElementsAreArray(expected))) << len;
        EXPECT_TRUE(t.ScriptConsumed()) << len;
    }
}

TEST(MutMemory, ReadReturnsWhatItHadWhenALaterChunkFails)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    const bytes::Bytes first = Counting(40);
    ScriptChunk(t, 0x8000, first);
    t.ExpectWrite(BuildSetupFrame(0xA0, 40));
    t.QueueError(ErrorKind::kDisconnected);
    EXPECT_THAT(ReadMemory(t, 0x8000, 80, token), IsOkAnd(ElementsAreArray(first)));
}

TEST(MutMemory, ReadFailsWhenTheFirstChunkFails)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    t.ExpectWrite(BuildSetupFrame(0xA0, 4));
    t.QueueError(ErrorKind::kDisconnected);
    EXPECT_THAT(ReadMemory(t, 0x8000, 4, token), IsErr(ErrorKind::kDisconnected));
}

TEST(MutMemory, ReadSkipsAChunkWhosePollReturnsNoFrame)
{
    // Today's loop only stops on a poll *error*; an empty poll appends nothing
    // and moves to the next chunk.
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    const auto channels = PlanReadChannels(0x8000, 40);
    t.ExpectWrite(BuildSetupFrame(0xA0, 40));
    t.QueueRead(BuildCommandFrame(0xA5, bytes::Bytes{}, kTrailerStd));
    t.ExpectWrite(BuildIdListFrame(0xA1, channels));
    t.QueueRead(BuildCommandFrame(0x05, bytes::Bytes{}, kTrailerStd));
    t.QueueNoFrame();
    const bytes::Bytes second = Counting(10, 0x40);
    ScriptChunk(t, 0x8028, second);
    EXPECT_THAT(ReadMemory(t, 0x8000, 50, token), IsOkAnd(ElementsAreArray(second)));
}
