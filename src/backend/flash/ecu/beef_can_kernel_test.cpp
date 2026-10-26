#include "src/backend/flash/ecu/beef_can_kernel.h"

#include <chrono>
#include <span>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/uds/testing/scripted_uds_channel.h"

namespace fastecu::flash
{
namespace
{

using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::Contains;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;
using ::testing::StrEq;

constexpr DensoBeefCanProfile kPetrol{.device = "ECU", .family = "petrol", .pdu_format = BeefPduFormat::kHex};
constexpr DensoBeefCanProfile kDiesel{.device = "ECU", .family = "diesel", .pdu_format = BeefPduFormat::kDescribe};
constexpr DensoBeefCanProfile kTcu{.device = "TCU", .family = "TCU", .pdu_format = BeefPduFormat::kHex};
constexpr BeefKernelTransferPolicy kPetrolPolicy{{50ms, 2000ms, 3000ms, 10}, 2000ms};

class BeefCanKernelTest : public ::testing::Test
{
  protected:
    BeefCanKernel Kernel(const DensoBeefCanProfile& profile = kPetrol)
    {
        return BeefCanKernel(transport_, clock_, cancellation_, events_, channel_, profile);
    }

    void Download(bytes::ByteView request)
    {
        channel_.ExpectSend(request);
        channel_.QueueReceive(bytes::Bytes{0x74, 0x20});
    }

    // Literal encrypted words from the legacy 59f4e442 algorithm, as in the
    // family transcript oracle. No production encoder constructs expectations.
    void Expect129ByteTransfer()
    {
        Download(bytes::Bytes{0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00});
        bytes::Bytes first{0xB6, 0xFF, 0x40, 0x00};
        for (int word = 0; word < 32; ++word)
        {
            first.insert(first.end(), {0xE7, 0xE2, 0x14, 0x30});
        }
        channel_.ExpectSend(first);
        bytes::Bytes second{0xB6, 0xFF, 0x40, 0x80, 0xC0, 0x41, 0xD4, 0xCA};
        for (int word = 0; word < 30; ++word)
        {
            second.insert(second.end(), {0xE7, 0xE2, 0x14, 0x30});
        }
        second.insert(second.end(), {0x42, 0x61, 0xDB, 0x2C});
        channel_.ExpectSend(second);
        channel_.ExpectSend(bytes::Bytes{0xB6, 0xFF, 0x41, 0x00});
    }

    KernelImage Image129()
    {
        bytes::Bytes image(129);
        image.back() = 1;
        return {.id = "kernel", .load_address = 0xFFFF4000U, .bytes = image};
    }

    ScriptedCanFlashTransport transport_;
    FakeClock clock_;
    FakeCancellationToken cancellation_;
    RecordingEventSink events_;
    uds::ScriptedUdsChannel channel_;
};

TEST_F(BeefCanKernelTest, ReadBeefMemoryTruncatesTheLastPageAndStopsOnFailureOrCancellation)
{
    const MemoryRegion region{0x1000, 0x500};
    PhaseReporter progress(events_, "Read", 1, 1, 0x500);
    const bytes::Bytes first_request{0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x10, 0x00, 0x04, 0x00};
    const bytes::Bytes second_request{0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x14, 0x00, 0x04, 0x00};
    bytes::Bytes reply{0xBE, 0xEF, 0x04, 0x01, 0x43};
    reply.resize(0x405, 0xAB);
    channel_.ExpectSend(first_request);
    channel_.QueueReceive(reply);
    channel_.ExpectSend(second_request);
    channel_.QueueReceive(reply);

    const auto rom = Kernel().Read(region, progress);

    ASSERT_THAT(rom, IsOk());
    EXPECT_THAT(*rom, ::testing::AllOf(::testing::SizeIs(0x500), test_bytes::BytesFilledWith(bytes::Byte{0xAB})));
    EXPECT_EQ(channel_.SendsConsumed(), 2U);
    EXPECT_TRUE(channel_.ScriptConsumed());

    channel_.ExpectSend(first_request);
    channel_.QueueReceive(reply);
    channel_.ExpectSend(second_request);
    channel_.QueueError(ErrorKind::kTimeout, "no reply");
    EXPECT_THAT(Kernel().Read(region, progress), IsErr(ErrorKind::kTimeout));
    cancellation_.SetCancelled(true);
    EXPECT_THAT(Kernel().Read(region, progress), IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelTest, ReadKeepsTheFixedPageLengthAndBoundsTailAndProgressToTheRequestedRegion)
{
    const MemoryRegion region{0x1000, 0x401};
    // A larger reporter total exposes the operation's own progress cap.
    PhaseReporter progress(events_, "Read", 1, 1, 0x800);
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x10, 0x00, 0x04, 0x00});
    bytes::Bytes first_reply{0xBE, 0xEF, 0x04, 0x01, 0x43};
    first_reply.resize(0x405, 0xAB);
    channel_.QueueReceive(first_reply);
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x07, 0x03, 0x00, 0x00, 0x14, 0x00, 0x04, 0x00});
    bytes::Bytes last_reply{0xBE, 0xEF, 0x04, 0x01, 0x43};
    last_reply.resize(0x405, 0xCD);
    channel_.QueueReceive(last_reply);

    const auto rom = Kernel().Read(region, progress);

    ASSERT_THAT(rom, IsOk());
    ASSERT_EQ(rom->size(), 0x400U + 1U);
    EXPECT_THAT(bytes::ByteView(*rom).first(0x400), test_bytes::BytesFilledWith(bytes::Byte{0xAB}));
    EXPECT_EQ(rom->back(), 0xCD);
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_THAT(channel_.timeouts, ElementsAre(2000ms, 2000ms));
    ASSERT_EQ(events_.phase_progress_calls.size(), 3U);
    EXPECT_EQ(events_.phase_progress_calls[0].done, 0);
    EXPECT_EQ(events_.phase_progress_calls[1].done, 0x400);
    EXPECT_EQ(events_.phase_progress_calls[2].done, 0x401);
}

TEST_F(BeefCanKernelTest, KernelDownloadRequestEncodesItsTwo24BitFields)
{
    Expect129ByteTransfer();
    for (int block = 0; block < 3; ++block)
    {
        transport_.QueueNoFrame();
    }

    EXPECT_THAT(Kernel().TransferPayload(Image129(), kPetrolPolicy), IsOk());
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_TRUE(transport_.ScriptConsumed());
}

TEST_F(BeefCanKernelTest, BeefKernelUploadPayloadNamesTheFamilyInBothSizeErrors)
{
    EXPECT_THAT(Kernel().TransferPayload({.id = "empty", .load_address = 0x1000, .bytes = {}}, kPetrolPolicy),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("petrol kernel upload size is invalid")));
    EXPECT_THAT(Kernel(kDiesel).TransferPayload(
                    {.id = "large", .load_address = 0x1000, .bytes = bytes::Bytes(0x1000000)}, kPetrolPolicy),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("diesel kernel upload exceeds the 24-bit wire length")));
    EXPECT_EQ(channel_.SendCalls(), 0U);
}

TEST_F(BeefCanKernelTest, BeefKernelPayloadReplacesThePaddedLastWordWithTheChecksum)
{
    // A 125-byte image pads to 128; AA in the last word is replaced by
    // 5AA5A559, the complement of the first word 00000001. The first word,
    // thirty zero words and checksum encrypt to these fixed literal words.
    bytes::Bytes image(125);
    image[3] = 1;
    image.back() = 0xAA;
    Download(bytes::Bytes{0x34, 0x04, 0x33, 0x00, 0x10, 0x00, 0x00, 0x00, 0x80});
    bytes::Bytes block{0xB6, 0x00, 0x10, 0x00, 0xFA, 0x4F, 0x60, 0x15};
    for (int word = 0; word < 30; ++word)
    {
        block.insert(block.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    block.insert(block.end(), {0x86, 0xA7, 0x05, 0x3A});
    ASSERT_EQ(block.size(), 132U);
    channel_.ExpectSend(block);
    channel_.ExpectSend(bytes::Bytes{0xB6, 0x00, 0x10, 0x80});
    transport_.QueueNoFrame();
    transport_.QueueNoFrame();

    EXPECT_THAT(Kernel().TransferPayload({.id = "kernel", .load_address = 0x1000, .bytes = image}, kPetrolPolicy),
                IsOk());
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_TRUE(transport_.ScriptConsumed());
    EXPECT_THAT(Kernel().TransferPayload({.id = "empty", .load_address = 0x1000, .bytes = {}}, kPetrolPolicy),
                IsErr(ErrorKind::kInvalidConfig));
}

TEST_F(BeefCanKernelTest, UploadBeefKernelBlocksEndsWithAnAddressOnlyBlock)
{
    Expect129ByteTransfer();
    transport_.QueueNoFrame();
    transport_.QueueNoFrame();
    transport_.QueueNoFrame();

    EXPECT_THAT(Kernel().TransferPayload(Image129(), kPetrolPolicy), IsOk());
    EXPECT_EQ(channel_.SendsConsumed(), 4U);
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_TRUE(transport_.ScriptConsumed());
}

TEST_F(BeefCanKernelTest, FamilyPoliciesPreserveDownloadAndBlockReplyTiming)
{
    for (const auto& [profile, policy] :
         {std::pair{&kPetrol, kPetrolPolicy},
          std::pair{&kDiesel, BeefKernelTransferPolicy{{50ms, 10ms, 3000ms, 10}, 500ms}},
          std::pair{&kTcu, BeefKernelTransferPolicy{{50ms, 2000ms, 3000ms, 10}, 2000ms}}})
    {
        SCOPED_TRACE(profile->family);
        for (int pending = 0; pending < 10; ++pending)
        {
            channel_.QueueReceive(bytes::Bytes{0x7F, 0x34, 0x78});
        }
        Expect129ByteTransfer();
        transport_.QueueNoFrame();
        transport_.QueueNoFrame();
        transport_.QueueNoFrame();
        const auto start = clock_.Elapsed();
        const auto reads = channel_.timeouts.size();
        const auto raw_reads = transport_.ReadTimeouts().size();

        ASSERT_THAT(Kernel(*profile).TransferPayload(Image129(), policy), IsOk());
        EXPECT_EQ(clock_.Elapsed() - start, 50ms);
        EXPECT_EQ(channel_.timeouts.at(reads), policy.download_policy.read_timeout);
        EXPECT_EQ(channel_.timeouts.size(), reads + 11);
        EXPECT_THAT(std::span{channel_.timeouts}.subspan(reads + 1), Each(3000ms));
        EXPECT_EQ(transport_.ReadTimeouts().size(), raw_reads + 3);
        EXPECT_THAT(std::span{transport_.ReadTimeouts()}.subspan(raw_reads),
                    ElementsAre(policy.block_reply_timeout, policy.block_reply_timeout, policy.block_reply_timeout));
        EXPECT_TRUE(channel_.ScriptConsumed());
        EXPECT_TRUE(transport_.ScriptConsumed());
    }
}

TEST_F(BeefCanKernelTest, DiscardB6ReplyIgnoresStaleContentButNotCancellationOrDisconnection)
{
    for (const auto kind : {ErrorKind::kBadResponse, ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(kind));
        ScriptedCanFlashTransport transport;
        uds::ScriptedUdsChannel channel;
        channel.ExpectSend(bytes::Bytes{0x34, 0x04, 0x33, 0x00, 0x10, 0x00, 0x00, 0x00, 0x80});
        channel.QueueReceive(bytes::Bytes{0x74, 0x20});
        bytes::Bytes block{0xB6, 0x00, 0x10, 0x00};
        for (int word = 0; word < 31; ++word)
        {
            block.insert(block.end(), {0xE7, 0xE2, 0x14, 0x30});
        }
        block.insert(block.end(), {0x8F, 0xC6, 0x58, 0x3A});
        EXPECT_EQ(block.size(), 132U);
        channel.ExpectSend(block);
        transport.QueueError(kind, "reply");
        if (kind == ErrorKind::kBadResponse)
        {
            channel.ExpectSend(bytes::Bytes{0xB6, 0x00, 0x10, 0x80});
            transport.QueueNoFrame();
        }
        BeefCanKernel kernel(transport, clock_, cancellation_, events_, channel, kPetrol);
        const auto status =
            kernel.TransferPayload({.id = "small", .load_address = 0x1000, .bytes = bytes::Bytes(5)}, kPetrolPolicy);
        if (kind == ErrorKind::kBadResponse)
        {
            EXPECT_THAT(status, IsOk());
        }
        else
        {
            EXPECT_THAT(status, IsErr(kind));
        }
        EXPECT_TRUE(channel.ScriptConsumed());
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST_F(BeefCanKernelTest, BlockSendFailureAndCancellationStopTheTransfer)
{
    Download(bytes::Bytes{0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00});
    bytes::Bytes first{0xB6, 0xFF, 0x40, 0x00};
    for (int word = 0; word < 32; ++word)
    {
        first.insert(first.end(), {0xE7, 0xE2, 0x14, 0x30});
    }
    channel_.ExpectSend(first, Fail(ErrorKind::kTimeout, "no ack"));
    EXPECT_THAT(Kernel().TransferPayload(Image129(), kPetrolPolicy), IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_THAT(transport_.ReadTimeouts(), IsEmpty());
    cancellation_.SetCancelled(true);
    EXPECT_THAT(Kernel().TransferPayload(Image129(), kPetrolPolicy), IsErr(ErrorKind::kCancelled));
}

TEST_F(BeefCanKernelTest, DownloadFailuresPreserveEachProfilesLogWording)
{
    for (const auto *profile : {&kPetrol, &kDiesel, &kTcu})
    {
        SCOPED_TRACE(profile->family);
        for (const auto kind : {ErrorKind::kTimeout, ErrorKind::kBadResponse})
        {
            SCOPED_TRACE(static_cast<int>(kind));
            events_.logs.clear();
            channel_.ExpectSend(bytes::Bytes{0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00});
            channel_.QueueError(kind, "bad download");
            EXPECT_THAT(Kernel(*profile).TransferPayload(Image129(), kPetrolPolicy), IsErr(kind));
            if (profile == &kDiesel)
            {
                EXPECT_THAT(events_.logs,
                            Contains(Pair(LogLevel::kError, kind == ErrorKind::kTimeout
                                                                ? "No valid response from ECU"
                                                                : "Wrong response from ECU: bad download")));
            }
            else
            {
                EXPECT_THAT(events_.logs, ::testing::Not(Contains(Pair(LogLevel::kError, ::testing::_))));
            }
        }
        events_.logs.clear();
        channel_.ExpectSend(bytes::Bytes{0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00});
        channel_.QueueReceive(bytes::Bytes{0x74, 0x21});
        EXPECT_THAT(Kernel(*profile).TransferPayload(Image129(), kPetrolPolicy),
                    IsErrWith(ErrorKind::kBadResponse, StrEq("unexpected response during kernel download")));
        const auto expected = profile == &kDiesel
                                  ? "Wrong response from ECU: unexpected response during kernel download"
                              : profile == &kPetrol ? "Wrong response from ECU during kernel download"
                                                    : "Wrong response from TCU during kernel download";
        EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kError, expected)));
        EXPECT_TRUE(channel_.ScriptConsumed());
    }
}

} // namespace
} // namespace fastecu::flash
