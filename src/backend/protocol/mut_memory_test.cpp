#include "src/backend/protocol/mut_memory.h"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>

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
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::IsEmpty;

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
        SCOPED_TRACE(addr);
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
        SCOPED_TRACE(len);
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

TEST(MutMemory, ReadFailsWhenALaterChunkFails)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    const bytes::Bytes first = Counting(40);
    ScriptChunk(t, 0x8000, first);
    t.ExpectWrite(BuildSetupFrame(0xA0, 40));
    t.QueueError(ErrorKind::kDisconnected);
    EXPECT_THAT(ReadMemory(t, 0x8000, 80, token), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(t.ScriptConsumed());
    EXPECT_TRUE(t.Ok());
}

TEST(MutMemory, ReadFailsWhenTheFirstChunkFails)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    t.ExpectWrite(BuildSetupFrame(0xA0, 4));
    t.QueueError(ErrorKind::kDisconnected);
    EXPECT_THAT(ReadMemory(t, 0x8000, 4, token), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(t.ScriptConsumed());
    EXPECT_TRUE(t.Ok());
}

TEST(MutMemory, ReadFailsWhenPollReturnsNoFrame)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    const auto channels = PlanReadChannels(0x8000, 40);
    t.ExpectWrite(BuildSetupFrame(0xA0, 40));
    t.QueueRead(BuildCommandFrame(0xA5, bytes::Bytes{}, kTrailerStd));
    t.ExpectWrite(BuildIdListFrame(0xA1, channels));
    t.QueueRead(BuildCommandFrame(0x05, bytes::Bytes{}, kTrailerStd));
    t.QueueNoFrame();
    EXPECT_THAT(ReadMemory(t, 0x8000, 50, token), IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(t.ScriptConsumed());
    EXPECT_TRUE(t.Ok());
}

TEST(MutMemory, ReadRejectsAddressWrapWithoutIo)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    EXPECT_THAT(ReadMemory(t, 0xFFFF, 2, token), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(t.ScriptConsumed());
    EXPECT_TRUE(t.Ok());
}

TEST(MutMemory, WriteRejectsCrossingWindowWithoutIo)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    EXPECT_THAT(WriteMemory(t, 0xBFFF, bytes::Bytes{0x01, 0x02}, token), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(t.ScriptConsumed());
    EXPECT_TRUE(t.Ok());
}

TEST(MutMemory, ReadAtLastAddressSucceeds)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    ScriptChunk(t, 0xFFFF, bytes::Bytes{0xA5});
    EXPECT_THAT(ReadMemory(t, 0xFFFF, 1, token), IsOkAnd(ElementsAreArray(bytes::Bytes{0xA5})));
    EXPECT_TRUE(t.ScriptConsumed());
    EXPECT_TRUE(t.Ok());
}

TEST(MutMemory, ZeroLengthReadSucceedsWithoutIo)
{
    for (const std::uint16_t addr : {std::uint16_t{0x0000}, std::uint16_t{0xFFFF}})
    {
        SCOPED_TRACE(addr);
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        EXPECT_THAT(ReadMemory(t, addr, 0, token), IsOkAnd(IsEmpty()));
        EXPECT_TRUE(t.ScriptConsumed());
        EXPECT_TRUE(t.Ok());
    }
}

TEST(MutMemory, ReadRejectsOversizedLengthWithoutIo)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    EXPECT_THAT(ReadMemory(t, 0x0000, std::numeric_limits<std::size_t>::max(), token),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_TRUE(t.ScriptConsumed());
    EXPECT_TRUE(t.Ok());
}

TEST(MutMemory, WriteAcceptsCompleteIntervalsInsideWindow)
{
    struct Case
    {
        std::uint16_t addr;
        std::size_t len;
    };
    for (const auto& [addr, len] : {Case{0xBFFF, 1}, Case{0xBFD5, 43}, Case{0x4000, 0x8000}})
    {
        SCOPED_TRACE(::testing::Message() << "address " << addr << ", length " << len);
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        const bytes::Bytes data = Counting(len);
        for (const auto& frame : BuildWriteFrames(addr, data))
        {
            t.ExpectWrite(frame);
            t.QueueRead(BuildCommandFrame(0x87, bytes::Bytes{0x80, 0x00}, kTrailerStd));
        }
        EXPECT_THAT(WriteMemory(t, addr, data, token), IsOk());
        EXPECT_TRUE(t.ScriptConsumed());
        EXPECT_TRUE(t.Ok());
    }
}

TEST(MutMemory, WriteRejectsIntervalsCrossingWindowWithoutIo)
{
    struct Case
    {
        std::uint16_t addr;
        std::size_t len;
    };
    for (const auto& [addr, len] : {Case{0xBFD5, 44}, Case{0x4000, 0x8001}})
    {
        SCOPED_TRACE(::testing::Message() << "address " << addr << ", length " << len);
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        EXPECT_THAT(WriteMemory(t, addr, Counting(len), token), IsErr(ErrorKind::kInvalidConfig));
        EXPECT_TRUE(t.ScriptConsumed());
        EXPECT_TRUE(t.Ok());
    }
}

TEST(MutMemory, EmptyWriteRetainsStartAddressValidation)
{
    struct Case
    {
        std::uint16_t addr;
        bool valid;
    };
    for (const auto& [addr, valid] : {Case{0x3FFF, false}, Case{0x4000, true}, Case{0xBFFF, true}, Case{0xC000, false}})
    {
        SCOPED_TRACE(addr);
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        const auto result = WriteMemory(t, addr, bytes::Bytes{}, token);
        if (valid)
        {
            EXPECT_THAT(result, IsOk());
        }
        else
        {
            EXPECT_THAT(result, IsErr(ErrorKind::kInvalidConfig));
        }
        EXPECT_TRUE(t.ScriptConsumed());
        EXPECT_TRUE(t.Ok());
    }
}

TEST(MutMemory, ReadPropagatesFirstAndLaterExchangeErrorsWithDetails)
{
    enum class Stage
    {
        kSetupAck,
        kListAck,
        kPoll
    };
    for (const bool later : {false, true})
    {
        SCOPED_TRACE(later ? "later chunk" : "first chunk");
        for (const auto stage : {Stage::kSetupAck, Stage::kListAck, Stage::kPoll})
        {
            SCOPED_TRACE(static_cast<int>(stage));
            ScriptedKlineTransport t;
            fastecu::FakeCancellationToken token;
            if (later)
            {
                ScriptChunk(t, 0x8000, Counting(40));
            }
            constexpr std::string_view kDetail = "transport failed at scripted exchange";
            const std::uint16_t addr = later ? 0x8028 : 0x8000;
            t.ExpectWrite(BuildSetupFrame(0xA0, 40));
            if (stage == Stage::kSetupAck)
            {
                t.QueueError(ErrorKind::kDisconnected, std::string(kDetail));
            }
            else
            {
                t.QueueRead(BuildCommandFrame(0xA5, bytes::Bytes{}, kTrailerStd));
                t.ExpectWrite(BuildIdListFrame(0xA1, PlanReadChannels(addr, 40)));
                if (stage == Stage::kListAck)
                {
                    t.QueueError(ErrorKind::kDisconnected, std::string(kDetail));
                }
                else
                {
                    t.QueueRead(BuildCommandFrame(0x05, bytes::Bytes{}, kTrailerStd));
                    t.QueueError(ErrorKind::kDisconnected, std::string(kDetail));
                }
            }
            EXPECT_THAT(ReadMemory(t, 0x8000, 120, token), IsErrWith(ErrorKind::kDisconnected, Eq(kDetail)));
            EXPECT_TRUE(t.ScriptConsumed());
            EXPECT_TRUE(t.Ok());
        }
    }
}

TEST(MutMemory, ReadPropagatesInitializationAndSetupWriteErrorsWithDetails)
{
    for (const bool wake_failure : {false, true})
    {
        SCOPED_TRACE(wake_failure ? "wake" : "setup write");
        ScriptedKlineTransport t;
        fastecu::FakeCancellationToken token;
        constexpr std::string_view kDetail = "transport initialization failed";
        if (wake_failure)
        {
            t.QueueSetBaudError(ErrorKind::kDisconnected, std::string(kDetail));
        }
        else
        {
            t.ExpectWrite(BuildSetupFrame(0xA0, 40));
            t.QueueWriteError(ErrorKind::kDisconnected, std::string(kDetail));
        }
        EXPECT_THAT(ReadMemory(t, 0x8000, 80, token), IsErrWith(ErrorKind::kDisconnected, Eq(kDetail)));
        EXPECT_TRUE(t.ScriptConsumed());
        EXPECT_TRUE(t.Ok());
    }
}

TEST(MutMemory, ReadRejectsEmptyMalformedAndShortPollsWithoutLaterExchange)
{
    enum class Reply
    {
        kNoFrame,
        kInvalidChecksum,
        kShortPayload
    };
    for (const bool later : {false, true})
    {
        SCOPED_TRACE(later ? "later chunk" : "first chunk");
        for (const auto reply : {Reply::kNoFrame, Reply::kInvalidChecksum, Reply::kShortPayload})
        {
            SCOPED_TRACE(static_cast<int>(reply));
            ScriptedKlineTransport t;
            fastecu::FakeCancellationToken token;
            if (later)
            {
                ScriptChunk(t, 0x8000, Counting(40));
            }
            const std::uint16_t addr = later ? 0x8028 : 0x8000;
            t.ExpectWrite(BuildSetupFrame(0xA0, 40));
            t.QueueRead(BuildCommandFrame(0xA5, bytes::Bytes{}, kTrailerStd));
            t.ExpectWrite(BuildIdListFrame(0xA1, PlanReadChannels(addr, 40)));
            t.QueueRead(BuildCommandFrame(0x05, bytes::Bytes{}, kTrailerStd));
            if (reply == Reply::kNoFrame)
            {
                t.QueueNoFrame();
            }
            else
            {
                bytes::Bytes frame{0x51};
                const auto payload = Counting(reply == Reply::kShortPayload ? 39 : 40);
                frame.insert(frame.end(), payload.begin(), payload.end());
                frame.push_back(static_cast<bytes::Byte>(Sum8(frame) + (reply == Reply::kInvalidChecksum ? 1 : 0)));
                frame.push_back(kTrailerStd);
                t.QueueRead(frame);
            }
            EXPECT_THAT(ReadMemory(t, 0x8000, 120, token),
                        IsErr(reply == Reply::kNoFrame ? ErrorKind::kTimeout : ErrorKind::kBadResponse));
            EXPECT_TRUE(t.ScriptConsumed());
            EXPECT_TRUE(t.Ok());
        }
    }
}

TEST(MutMemory, ReadPropagatesCancellationAfterFirstChunk)
{
    ScriptedKlineTransport t;
    fastecu::FakeCancellationToken token;
    token.CancelOnCheck(4);
    ScriptChunk(t, 0x8000, Counting(40));
    t.ExpectWrite(BuildSetupFrame(0xA0, 40));
    EXPECT_THAT(ReadMemory(t, 0x8000, 120, token),
                IsErrWith(ErrorKind::kCancelled, Eq("scripted K-Line read cancelled")));
    EXPECT_TRUE(t.ScriptConsumed());
    EXPECT_TRUE(t.Ok());
}
