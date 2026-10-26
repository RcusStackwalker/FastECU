#include "src/backend/flash/ecu/beef_can_kernel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <functional>
#include <iterator>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/flash_validation.h"
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
constexpr DensoBeefCanProfile kTcu{.device = "TCU",
                                   .family = "TCU",
                                   .pdu_format = BeefPduFormat::kHex,
                                   .init_timeout = 2000ms,
                                   .upper_hex_sizes = true};
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

bytes::Bytes Be32(std::uint32_t value)
{
    return {static_cast<bytes::Byte>(value >> 24U), static_cast<bytes::Byte>(value >> 16U),
            static_cast<bytes::Byte>(value >> 8U), static_cast<bytes::Byte>(value)};
}

// Independent wire fixtures; no production framing or checksum helper builds expectations.
bytes::Bytes BeefFrame(bytes::Byte opcode, bytes::ByteView payload = {})
{
    const auto length = payload.size() + 1;
    bytes::Bytes frame{0xBE, 0xEF, static_cast<bytes::Byte>(length >> 8U), static_cast<bytes::Byte>(length), opcode};
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

struct BlockFixture
{
    MemoryRegion region;
    std::uint32_t zero_crc;
};

// Literal SH7058 geometry and all-zero CRCs retained from the session suite.
constexpr std::array<BlockFixture, 16> kBlocks{{
    {{0x00000000, 0x00001000}, 0xF722EF49},
    {{0x00001000, 0x00001000}, 0xF722EF49},
    {{0x00002000, 0x00001000}, 0xF722EF49},
    {{0x00003000, 0x00001000}, 0xF722EF49},
    {{0x00004000, 0x00001000}, 0xF722EF49},
    {{0x00005000, 0x00001000}, 0xF722EF49},
    {{0x00006000, 0x00001000}, 0xF722EF49},
    {{0x00007000, 0x00001000}, 0xF722EF49},
    {{0x00008000, 0x00018000}, 0xC85F0DED},
    {{0x00020000, 0x00020000}, 0xC39BE4A8},
    {{0x00040000, 0x00020000}, 0xC39BE4A8},
    {{0x00060000, 0x00020000}, 0xC39BE4A8},
    {{0x00080000, 0x00020000}, 0xC39BE4A8},
    {{0x000A0000, 0x00020000}, 0xC39BE4A8},
    {{0x000C0000, 0x00020000}, 0xC39BE4A8},
    {{0x000E0000, 0x00020000}, 0xC39BE4A8},
}};

// Test-owned adapter exposes accepted-reply boundaries for cancellation and
// defensive validation cases. A validation test may synchronously mutate its
// genuinely nonconst plan after compare, so the pre-erase guard stays observable
// through Write. There is no production callback or access to private helpers.
class ValidationBoundaryChannel final : public uds::IUdsChannel
{
  public:
    explicit ValidationBoundaryChannel(uds::ScriptedUdsChannel& script) : script_(script)
    {
    }

    Status Send(bytes::ByteView pdu, const ICancellationToken& cancellation) override
    {
        requests.emplace_back(pdu.begin(), pdu.end());
        return script_.Send(pdu, cancellation);
    }
    Result<std::optional<bytes::Bytes>> Receive(std::chrono::milliseconds timeout,
                                                const ICancellationToken& cancellation) override
    {
        auto reply = script_.Receive(timeout, cancellation);
        if (after_reply)
        {
            after_reply(requests.back()[4]);
        }
        return reply;
    }

    std::function<void(bytes::Byte)> after_reply;
    std::vector<bytes::Bytes> requests;

  private:
    uds::ScriptedUdsChannel& script_;
};

class BeefCanKernelWriteTest : public BeefCanKernelTest
{
  protected:
    Result<FlashPlan> PlanFor(FlashOperation operation = FlashOperation::kWrite)
    {
        return BuildSubaruDensoSh7058CanPlan(
            operation, "sub_ecu_denso_sh7058_can", "SH7058", bytes::Bytes(0x100000),
            KernelImage{.id = "petrol-sh7058-kernel", .load_address = 0xFFFF3000, .bytes = {1, 2, 3, 4}});
    }

    Result<FlashPlan> SmallPlan(bytes::Bytes image = bytes::Bytes(0x1000), std::uint32_t base = 0x2000,
                                std::vector<MemoryRegion> regions = {{0x2000, 0x1000}},
                                FlashOperation operation = FlashOperation::kWrite)
    {
        const auto size = static_cast<std::uint32_t>(image.size());
        return ValidateAndBuild({.operation = operation,
                                 .family = FlashFamily::kSubaruDensoSh7058Can,
                                 .transport = TransportKind::kCanIso15765,
                                 .target_id = "kernel-test",
                                 .mcu_name = "SH7058",
                                 .transfer_region = {base, std::max(size, 1U)},
                                 .erase_regions = std::move(regions),
                                 .image = std::move(image),
                                 .kernel = KernelImage{.id = "kernel", .load_address = 0xFFFF3000, .bytes = {1}},
                                 .family_plan = SubaruDensoSh7058CanPlan{}});
    }

    Status RunWrite(const FlashPlan& plan, const DensoBeefCanProfile& profile = kPetrol)
    {
        PhaseSequence phases(events_, 4);
        phases.Start("Kernel", 1).Complete();
        auto compare = phases.Start("Compare", static_cast<int>(plan.EraseRegions().size()));
        BeefCanKernel kernel(transport_, clock_, cancellation_, events_, boundary_, profile);
        const auto status = kernel.Write(plan, phases, compare);
        if (status.has_value())
        {
            phases.Start("Complete", 1).Complete();
        }
        return status;
    }

    void ScriptCrc(const MemoryRegion& block, std::uint32_t crc, bool drain = true)
    {
        bytes::Bytes payload = Be32(block.start);
        const auto length = Be32(block.length);
        payload.insert(payload.end(), length.begin(), length.end());
        channel_.ExpectSend(BeefFrame(0x02, payload));
        channel_.QueueReceive(BeefFrame(0x42, Be32(crc)));
        if (drain)
        {
            transport_.QueueNoFrame();
        }
    }

    void ScriptCompare(std::optional<std::size_t> differing = std::nullopt)
    {
        for (std::size_t index = 0; index < kBlocks.size(); ++index)
        {
            ScriptCrc(kBlocks[index].region, index == differing ? 0xDEADBEEF : kBlocks[index].zero_crc);
        }
    }

    void ScriptInitialize(bytes::Byte mode_opcode = 0x20, std::uint32_t message_size = 0x1000,
                          std::uint32_t block_size = 0x200)
    {
        channel_.ExpectSend(BeefFrame(0x05));
        channel_.QueueReceive(BeefFrame(0x45, Be32(message_size)));
        channel_.ExpectSend(BeefFrame(0x06));
        channel_.QueueReceive(BeefFrame(0x46, Be32(block_size)));
        channel_.ExpectSend(BeefFrame(mode_opcode));
        channel_.QueueReceive(BeefFrame(static_cast<bytes::Byte>(mode_opcode | 0x40), {}));
    }

    void ScriptVoltage()
    {
        channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x04});
        channel_.QueueReceive(bytes::Bytes{0xBE, 0xEF, 0x00, 0x03, 0x44, 0x00, 0x64});
    }

    // One 0x1000 zero-filled window: erase, eight 0x200 chunks, commit/validate.
    // The CRC F722EF49 is the independent literal retained from the session suite.
    void ScriptWindow(std::uint32_t address = 0x2000, bool test_write = false, int last_exchange = 9,
                      bool fail_last = false)
    {
        for (int step = 0; step <= last_exchange; ++step)
        {
            bytes::Byte opcode = step == 0 ? 0x25 : step == 9 ? (test_write ? 0x23 : 0x24) : 0x22;
            bytes::Bytes payload =
                Be32(address + (step > 0 && step < 9 ? static_cast<std::uint32_t>(step - 1) * 0x200 : 0));
            if (step > 0 && step < 9)
            {
                payload.resize(0x204, 0);
            }
            else if (step == 9)
            {
                payload.insert(payload.end(), {0x10, 0x00, 0xF7, 0x22, 0xEF, 0x49});
            }
            channel_.ExpectSend(BeefFrame(opcode, payload));
            if (fail_last && step == last_exchange)
            {
                channel_.QueueError(ErrorKind::kTimeout, "no reply");
            }
            else
            {
                channel_.QueueReceive(BeefFrame(static_cast<bytes::Byte>(opcode | 0x40)));
            }
        }
    }

    std::vector<bytes::Byte> Opcodes(std::size_t first = 0) const
    {
        std::vector<bytes::Byte> opcodes;
        std::ranges::transform(std::span{boundary_.requests}.subspan(std::min(first, boundary_.requests.size())),
                               std::back_inserter(opcodes), [](const auto& request) { return request[4]; });
        return opcodes;
    }

    ValidationBoundaryChannel boundary_{channel_};
};

TEST_F(BeefCanKernelWriteTest, QueryCrcReadsFourBytesAndDrainsTheStaleFrame)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
    EXPECT_THAT(RunWrite(*plan), IsOk());
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kDebug, "ROM CRC: 0xf722ef49 IMG CRC: 0xf722ef49")));
    EXPECT_EQ(channel_.last_timeout, 3000ms);
    EXPECT_THAT(transport_.ReadTimeouts(), ElementsAre(200ms));
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_TRUE(transport_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, CompareBlocksNamesTheFamilyWhenABlockLeavesTheImage)
{
    const auto plan = SmallPlan({}, 0x2000);
    ASSERT_THAT(plan, IsOk());
    for (const auto *profile : {&kPetrol, &kDiesel, &kTcu})
    {
        SCOPED_TRACE(profile->family);
        EXPECT_THAT(
            RunWrite(*plan, *profile),
            IsErrWith(ErrorKind::kInvalidConfig, std::format("{} CRC block is outside the image", profile->family)));
    }
    EXPECT_EQ(channel_.SendCalls(), 0U);
}

TEST_F(BeefCanKernelWriteTest, CompareBlocksFlagsOnlyDifferingBlocks)
{
    const auto plan = PlanFor();
    ASSERT_THAT(plan, IsOk());
    ScriptCompare(3);
    ScriptInitialize();
    ScriptVoltage();
    ScriptWindow(0x3000);
    ScriptCompare();
    ASSERT_THAT(RunWrite(*plan), IsOk());
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, " (total: 1)")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "3, ")));
    const auto opcodes = Opcodes();
    EXPECT_EQ(std::count(opcodes.begin(), opcodes.end(), bytes::Byte{0x25}), 1);
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_TRUE(transport_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, InitializeFlashSelectsTestOrRealWriteMode)
{
    for (const auto operation : {FlashOperation::kTestWrite, FlashOperation::kWrite})
    {
        SCOPED_TRACE(static_cast<int>(operation));
        const auto plan = SmallPlan(bytes::Bytes(0x1000), 0x2000, {{0x2000, 0x1000}}, operation);
        ASSERT_THAT(plan, IsOk());
        const bool test_write = operation == FlashOperation::kTestWrite;
        const auto first = boundary_.requests.size();
        const auto timeout_start = channel_.timeouts.size();
        ScriptCrc({0x2000, 0x1000}, 0xDEADBEEF);
        ScriptInitialize(test_write ? 0x21 : 0x20);
        ScriptVoltage();
        ScriptWindow(0x2000, test_write);
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
        ASSERT_THAT(RunWrite(*plan), IsOk());
        ASSERT_GE(channel_.timeouts.size(), timeout_start + 2);
        EXPECT_EQ(channel_.timeouts.at(timeout_start + 1), 500ms);
        EXPECT_THAT(Opcodes(first), ElementsAre(0x02, 0x05, 0x06, test_write ? 0x21 : 0x20, 0x04, 0x25, 0x22, 0x22,
                                                0x22, 0x22, 0x22, 0x22, 0x22, 0x22, test_write ? 0x23 : 0x24, 0x02));
    }
    EXPECT_THAT(events_.logs,
                Contains(Pair(LogLevel::kInfo, "Test write mode on, no actual flash write is performed")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "Test write mode off, perform actual flash write")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, ": 0x1000")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, ": 0x0200")));
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_TRUE(transport_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, InitializeFlashUsesTheProfileTimeoutAndHexCase)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    for (const auto *profile : {&kPetrol, &kTcu})
    {
        SCOPED_TRACE(profile->family);
        const auto timeouts = channel_.timeouts.size();
        ScriptCrc({0x2000, 0x1000}, 0xDEADBEEF);
        ScriptInitialize(0x20, 0x0ABC, 0x0ABC);
        ScriptVoltage();
        ScriptWindow();
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
        ASSERT_THAT(RunWrite(*plan, *profile), IsOk());
        ASSERT_GE(channel_.timeouts.size(), timeouts + 4);
        EXPECT_THAT(std::span{channel_.timeouts}.subspan(timeouts + 1, 3), Each(profile == &kPetrol ? 500ms : 2000ms));
        EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, profile == &kPetrol ? ": 0x0abc" : ": 0x0ABC")));
        events_.logs.clear();
    }
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, WriteMemoryWithNoDifferenceFlashesNothing)
{
    const auto plan = PlanFor();
    ASSERT_THAT(plan, IsOk());
    ScriptCompare();
    ASSERT_THAT(RunWrite(*plan), IsOk());
    EXPECT_THAT(events_.logs,
                Contains(Pair(LogLevel::kInfo,
                              "*** Compare results no difference between ROM and ECU data, no flashing needed! ***")));
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_THAT(Opcodes(), Each(0x02));
    std::vector<std::string> phase_names;
    for (const auto& call : events_.phase_progress_calls)
    {
        SCOPED_TRACE(std::format("{} done={}", call.phase_name, call.done));
        if (call.done == 0)
        {
            phase_names.emplace_back(call.phase_name);
        }
        EXPECT_EQ(call.phase_count, 4);
    }
    EXPECT_THAT(phase_names, ElementsAre("Kernel", "Compare", "Write", "Complete"));
    EXPECT_EQ(events_.phase_progress_calls.back().done, 1);
}

TEST_F(BeefCanKernelWriteTest, WriteMemoryFailsWithTheExchangeErrorWhenTheVoltageCheckTimesOut)
{
    const auto plan = PlanFor();
    ASSERT_THAT(plan, IsOk());
    ScriptCompare(0);
    ScriptInitialize();
    channel_.ExpectSend(BeefFrame(0x04));
    channel_.QueueNoFrame();
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "Block 0 reflash failed.")));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, CompareBeefBlocksChecksEachRegionAgainstItsOwnSliceOfTheImage)
{
    bytes::Bytes image(12);
    std::iota(image.begin(), image.end(), bytes::Byte{1});
    const auto plan = SmallPlan(image, 0x1000, {{0x1000, 4}, {0x1004, 4}, {0x1008, 4}});
    ASSERT_THAT(plan, IsOk());
    // Independent bitwise CRCs (polynomial 5AA5A55A) for 01..04, 05..08 and 09..0C.
    ScriptCrc({0x1000, 4}, 0xC802BEF9);
    ScriptCrc({0x1004, 4}, 0x947F66CA); // differing by one bit
    ScriptCrc({0x1008, 4}, 0xE3B4222E);
    channel_.ExpectSend(BeefFrame(0x05));
    channel_.QueueError(ErrorKind::kTimeout, "stop after compare");
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kTimeout, "stop after compare"));
    const auto opcodes = Opcodes();
    EXPECT_EQ(std::count(opcodes.begin(), opcodes.end(), bytes::Byte{0x02}), 3);
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, " (total: 1)")));
    std::vector<std::string> comparisons;
    for (const auto& [level, text] : events_.logs)
    {
        if (level == LogLevel::kInfo && (text == "\tYES" || text == "\tNO"))
        {
            comparisons.push_back(text);
        }
    }
    EXPECT_THAT(comparisons, ElementsAre("\tYES", "\tNO", "\tYES"));
    EXPECT_EQ(clock_.Elapsed(), 5ms * 3);
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "1, ")));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, CompareBeefBlocksRejectsRegionsOutsideTheImageAndNamesTheFamily)
{
    for (const auto& [size, base] : {std::pair{11U, 0x1000U}, std::pair{12U, 0x1001U}, std::pair{12U, 0x1000U}})
    {
        SCOPED_TRACE(std::format("size={} base={}", size, base));
        const auto plan = SmallPlan(bytes::Bytes(size), base, {{0x1000, 4}, {0x1004, 4}, {0x1008, 4}});
        ASSERT_THAT(plan, IsOk());
        if (base == 0x1000)
        {
            ScriptCrc({0x1000, 4}, 0xCD3AA48B);
            ScriptCrc({0x1004, 4}, 0xCD3AA48B);
            if (size == 12)
            {
                ScriptCrc({0x1008, 4}, 0xCD3AA48B);
            }
        }
        if (size == 12 && base == 0x1000)
        {
            EXPECT_THAT(RunWrite(*plan), IsOk());
        }
        else
        {
            EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kInvalidConfig, "petrol CRC block is outside the image"));
        }
        EXPECT_TRUE(channel_.ScriptConsumed());
        EXPECT_TRUE(transport_.ScriptConsumed());
    }
}

TEST_F(BeefCanKernelWriteTest, CompareBeefBlocksStopsOnQueryFailureAndOnCancellationDuringTheSettle)
{
    const auto plan = SmallPlan(bytes::Bytes(12), 0x1000, {{0x1000, 4}, {0x1004, 4}, {0x1008, 4}});
    ASSERT_THAT(plan, IsOk());
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x09, 0x02, 0, 0, 0x10, 0, 0, 0, 0, 4});
    channel_.QueueError(ErrorKind::kTimeout, "no CRC reply");
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kTimeout, "no CRC reply"));
    EXPECT_TRUE(channel_.ScriptConsumed());

    class CancelDuringSettle final : public FakeClock
    {
      public:
        explicit CancelDuringSettle(FakeCancellationToken& token) : token_(token)
        {
        }
        Status Sleep(std::chrono::milliseconds duration, const ICancellationToken& token) override
        {
            token_.SetCancelled(true);
            return FakeClock::Sleep(duration, token);
        }

      private:
        FakeCancellationToken& token_;
    } clock(cancellation_);
    ScriptCrc({0x1000, 4}, 0xCD3AA48B);
    BeefCanKernel kernel(transport_, clock, cancellation_, events_, channel_, kPetrol);
    PhaseSequence phases(events_, 4);
    auto progress = phases.Start("Compare", 3);
    EXPECT_THAT(kernel.Write(*plan, phases, progress), IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_TRUE(transport_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, InitializeBeefFlashSelectsTheModeAndStopsAtTheFirstFailingExchange)
{
    for (const bool test_write : {false, true})
    {
        SCOPED_TRACE(test_write);
        const auto plan = SmallPlan(bytes::Bytes(0x1000), 0x2000, {{0x2000, 0x1000}},
                                    test_write ? FlashOperation::kTestWrite : FlashOperation::kWrite);
        ASSERT_THAT(plan, IsOk());
        const auto first = boundary_.requests.size();
        ScriptCrc({0x2000, 0x1000}, 0);
        ScriptInitialize(test_write ? 0x21 : 0x20, 0xABCD, 0xABCD);
        ScriptVoltage();
        ScriptWindow(0x2000, test_write);
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
        EXPECT_THAT(RunWrite(*plan, test_write ? kTcu : kPetrol), IsOk());
        const auto opcodes = Opcodes(first);
        ASSERT_GE(opcodes.size(), 4U);
        EXPECT_THAT(std::span{opcodes}.subspan(1, 3), ElementsAre(0x05, 0x06, test_write ? 0x21 : 0x20));
        EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, test_write ? ": 0xABCD" : ": 0xabcd")));
    }
    for (const std::size_t failing : {0U, 1U, 2U})
    {
        SCOPED_TRACE(failing);
        const auto plan = SmallPlan();
        ASSERT_THAT(plan, IsOk());
        ScriptCrc({0x2000, 0x1000}, 0);
        const auto first = boundary_.requests.size();
        const std::array<bytes::Byte, 3> opcodes{0x05, 0x06, 0x20};
        for (std::size_t index = 0; index <= failing; ++index)
        {
            channel_.ExpectSend(BeefFrame(opcodes[index]));
            if (index == failing)
            {
                channel_.QueueError(ErrorKind::kTimeout, "no reply");
            }
            else
            {
                channel_.QueueReceive(BeefFrame(static_cast<bytes::Byte>(opcodes[index] | 0x40), bytes::Bytes(4)));
            }
        }
        EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kTimeout, "no reply"));
        EXPECT_EQ(boundary_.requests.size() - first - 1, failing + 1);
        EXPECT_TRUE(channel_.ScriptConsumed());
    }
}

TEST_F(BeefCanKernelWriteTest, FlashBeefBlockRejectsBlocksThatDoNotFitTheImageAndNamesTheFamily)
{
    struct Case
    {
        MemoryRegion block;
        std::uint32_t base;
        const char *detail;
    };
    for (const auto& test :
         std::array{Case{{0x2000, 0x300}, 0x2000, "diesel flash block is not aligned to write windows"},
                    Case{{0x2000, 0}, 0x2000, "diesel flash block is not aligned to write windows"},
                    Case{{0x2000, 0x1000}, 0x2001, "diesel flash block starts before the image"},
                    Case{{0x2000, 0x1000}, 0x1001, "diesel flash block extends beyond the image"}})
    {
        SCOPED_TRACE(test.detail);
        // This plan is nonconst. Change only its region after the voltage reply:
        // compare has validated the original region, and FlashBlock must check
        // the new region before erase or taking an image subspan.
        auto plan = SmallPlan(bytes::Bytes(0x1000), test.base, {{test.base, 0x1000}});
        ASSERT_THAT(plan, IsOk());
        auto& validation_region = const_cast<MemoryRegion&>(plan->EraseRegions()[0]);
        ScriptCrc({test.base, 0x1000}, 0);
        ScriptInitialize();
        ScriptVoltage();
        boundary_.after_reply = [&](bytes::Byte opcode)
        {
            if (opcode == 0x04)
            {
                validation_region = test.block;
            }
        };
        EXPECT_THAT(RunWrite(*plan, kDiesel), IsErrWith(ErrorKind::kInvalidConfig, test.detail));
        boundary_.after_reply = {};
        EXPECT_TRUE(channel_.ScriptConsumed());
        EXPECT_EQ(channel_.SendCalls(), channel_.SendsConsumed());
        EXPECT_THAT(Opcodes(), ::testing::Not(Contains(0x25)));
    }
}

TEST_F(BeefCanKernelWriteTest, FlashBeefBlockErasesWritesAndCommitsOrValidatesOneWindow)
{
    for (const bool test_write : {false, true})
    {
        SCOPED_TRACE(test_write);
        const auto plan = SmallPlan(bytes::Bytes(0x1000), 0x2000, {{0x2000, 0x1000}},
                                    test_write ? FlashOperation::kTestWrite : FlashOperation::kWrite);
        ASSERT_THAT(plan, IsOk());
        const auto first = boundary_.requests.size();
        const auto timeouts = channel_.timeouts.size();
        ScriptCrc({0x2000, 0x1000}, 0);
        ScriptInitialize(test_write ? 0x21 : 0x20);
        ScriptVoltage();
        ScriptWindow(0x2000, test_write);
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
        EXPECT_THAT(RunWrite(*plan), IsOk());
        const auto opcodes = Opcodes(first + 5);
        ASSERT_EQ(opcodes.size(), 11U); // ten block exchanges, then final CRC
        EXPECT_EQ(opcodes.front(), 0x25);
        EXPECT_EQ(opcodes[9], test_write ? 0x23 : 0x24);
        EXPECT_THAT(events_.phase_progress_calls,
                    Contains(::testing::AllOf(::testing::Field(&RecordedPhaseProgress::phase_name, "Write"),
                                              ::testing::Field(&RecordedPhaseProgress::done, 0x1000))));
        EXPECT_THAT(std::span{channel_.timeouts}.subspan(timeouts),
                    ElementsAre(3000ms, 500ms, 500ms, 500ms, 500ms, 3000ms, 800ms, 800ms, 800ms, 800ms, 800ms, 800ms,
                                800ms, 800ms, 3000ms, 3000ms));
        EXPECT_TRUE(channel_.ScriptConsumed());
        EXPECT_TRUE(transport_.ScriptConsumed());
    }
    for (const int failing : {0, 1, 9})
    {
        SCOPED_TRACE(failing);
        const auto plan = SmallPlan();
        ASSERT_THAT(plan, IsOk());
        ScriptCrc({0x2000, 0x1000}, 0);
        ScriptInitialize();
        ScriptVoltage();
        ScriptWindow(0x2000, false, failing, true);
        EXPECT_THAT(RunWrite(*plan), IsErr(ErrorKind::kTimeout));
        EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kError,
                                                "Reflash error! Do not panic, do not reset the ECU immediately. The "
                                                "kernel is most likely still running and receiving commands!")));
        EXPECT_TRUE(channel_.ScriptConsumed());
        events_.logs.clear();
    }
    for (const int cancel_at : {0, 8})
    {
        SCOPED_TRACE(cancel_at);
        const auto plan = SmallPlan();
        ASSERT_THAT(plan, IsOk());
        ScriptCrc({0x2000, 0x1000}, 0);
        ScriptInitialize();
        ScriptVoltage();
        ScriptWindow(0x2000, false, cancel_at);
        int block_calls = -1;
        boundary_.after_reply = [&](bytes::Byte opcode)
        {
            if ((opcode == 0x25 || opcode == 0x22) && ++block_calls == cancel_at)
            {
                cancellation_.SetCancelled(true);
            }
        };
        EXPECT_THAT(RunWrite(*plan), IsErr(ErrorKind::kCancelled));
        EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kError,
                                                "Reflash error! Do not panic, do not reset the ECU immediately. The "
                                                "kernel is most likely still running and receiving commands!")));
        EXPECT_TRUE(channel_.ScriptConsumed());
        EXPECT_EQ(channel_.SendCalls(), channel_.SendsConsumed());
        boundary_.after_reply = {};
        cancellation_.SetCancelled(false);
        events_.logs.clear();
    }
}

TEST_F(BeefCanKernelWriteTest, WriteBeefMemoryLogsTheVoltageAndTheTimeLeftFromTheBytesStillToWrite)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0);
    ScriptInitialize();
    ScriptVoltage();
    ScriptWindow();
    ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
    EXPECT_THAT(RunWrite(*plan), IsOk());
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, ": 2V")));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, FlashBeefBlockReportsTheTimeLeftForTheRestOfTheWrite)
{
    // The public operation computes remaining bytes itself. A second changed
    // region makes the same 5-second log observable without injecting a counter.
    constexpr std::uint32_t kTotal = 0x200 + 5 * 512000 - 1;
    const auto plan = SmallPlan(bytes::Bytes(kTotal), 0x2000, {{0x2000, 0x1000}, {0x3000, kTotal - 0x1000}});
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 1);
    ScriptCrc({0x3000, kTotal - 0x1000}, 1);
    ScriptInitialize();
    ScriptVoltage();
    ScriptWindow();
    channel_.ExpectSend(BeefFrame(0x04));
    channel_.QueueError(ErrorKind::kTimeout, "stop after first block");
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kTimeout, "stop after first block"));
    EXPECT_THAT(events_.logs,
                Contains(Pair(LogLevel::kInfo, "Write flash buffer: 0x00002000 (0% - 512000 B/s, ~ 5 s)")));
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, "Block 0 reflash complete.")));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, WriteExchangeWrapsAndUnwrapsBeef)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0);
    ScriptInitialize(); // Exact 05 request and 00 00 10 00 reply retained.
    ScriptVoltage();
    ScriptWindow();
    ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
    ASSERT_THAT(RunWrite(*plan), IsOk());
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, ": 0x1000")));
    ASSERT_GE(channel_.timeouts.size(), 2U);
    EXPECT_EQ(channel_.timeouts.at(1), 500ms);
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, WriteExchangeRejectsAWrongOpcode)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0);
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0, 1, 0x05});
    channel_.QueueReceive(bytes::Bytes{0xBE, 0xEF, 0, 1, 0x46});
    EXPECT_THAT(RunWrite(*plan),
                IsErrWith(ErrorKind::kBadResponse, "unexpected BEEF response opcode 0x46, expected 0x45"));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, WriteExchangeRejectsAShortPayload)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0);
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0, 1, 0x05});
    channel_.QueueReceive(bytes::Bytes{0xBE, 0xEF, 0, 2, 0x45, 0x01});
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kBadResponse, "short BEEF response payload"));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, WriteExchangeTimeoutIsTheReturnedErrorAndLogsNothing)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0);
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0, 1, 0x05});
    channel_.QueueNoFrame();
    std::size_t logs_before_reply = 0;
    boundary_.after_reply = [&](bytes::Byte) { logs_before_reply = events_.logs.size(); };
    EXPECT_THAT(RunWrite(*plan, kDiesel), IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"));
    EXPECT_THAT(std::span{events_.logs}.subspan(logs_before_reply), IsEmpty());
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, WriteExchangeSurfacesAChannelBadResponse)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0);
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0, 1, 0x05});
    channel_.QueueError(ErrorKind::kBadResponse, "expected CAN reply id 0x7e8, got 0x123");
    std::size_t logs_before_reply = 0;
    boundary_.after_reply = [&](bytes::Byte) { logs_before_reply = events_.logs.size(); };
    EXPECT_THAT(RunWrite(*plan, kDiesel), IsErrWith(ErrorKind::kBadResponse, "expected CAN reply id 0x7e8, got 0x123"));
    EXPECT_THAT(std::span{events_.logs}.subspan(logs_before_reply), IsEmpty());
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, BeefExchangeReturnsThePayloadOfAnOpcodeOrSizeCheckedReply)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0);
    ScriptInitialize();
    // The old arbitrary-exchange case's two-byte AA BB payload is observable
    // through voltage (minimum 2); Write's init operation requires four bytes.
    channel_.ExpectSend(BeefFrame(0x04));
    channel_.QueueReceive(bytes::Bytes{0xBE, 0xEF, 0, 3, 0x44, 0xAA, 0xBB});
    ScriptWindow();
    ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
    EXPECT_THAT(RunWrite(*plan), IsOk());
    EXPECT_THAT(events_.logs, Contains(Pair(LogLevel::kInfo, ": 874.14V")));

    for (const auto& [reply, detail] : std::array{
             std::pair{bytes::Bytes{0xBE, 0xEF, 0, 3, 0x45, 0xAA, 0xBB}, "short BEEF response payload"},
             std::pair{bytes::Bytes{0xBE, 0xEF, 0, 1, 0x46}, "unexpected BEEF response opcode 0x46, expected 0x45"},
             std::pair{bytes::Bytes{0xBE}, "short BEEF response envelope"}})
    {
        SCOPED_TRACE(detail);
        ScriptCrc({0x2000, 0x1000}, 0);
        channel_.ExpectSend(BeefFrame(0x05));
        channel_.QueueReceive(reply);
        EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kBadResponse, detail));
        EXPECT_TRUE(channel_.ScriptConsumed());
    }
    ScriptCrc({0x2000, 0x1000}, 0);
    channel_.ExpectSend(BeefFrame(0x05));
    channel_.QueueNoFrame();
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"));
    ScriptCrc({0x2000, 0x1000}, 0);
    channel_.ExpectSend(BeefFrame(0x05));
    channel_.QueueError(ErrorKind::kDisconnected, "gone");
    EXPECT_THAT(RunWrite(*plan), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, CrcStaleDrainIgnoresContentButPropagatesCancellationAndDisconnection)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    for (const auto kind : {ErrorKind::kTimeout, ErrorKind::kBadResponse, ErrorKind::kInternal, ErrorKind::kCancelled,
                            ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(kind));
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49, false);
        transport_.QueueError(kind, "stale read");
        const auto status = RunWrite(*plan);
        if (kind == ErrorKind::kCancelled || kind == ErrorKind::kDisconnected)
        {
            EXPECT_THAT(status, IsErrWith(kind, "stale read"));
        }
        else
        {
            EXPECT_THAT(status, IsOk());
        }
        EXPECT_TRUE(channel_.ScriptConsumed());
        EXPECT_TRUE(transport_.ScriptConsumed());
    }
    for (const auto& stale : {bytes::Bytes{0xBE}, bytes::Bytes{0, 0, 1, 0x23, 0x7F, 2, 0x13}})
    {
        SCOPED_TRACE(::testing::PrintToString(stale));
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49, false);
        transport_.QueueRead(stale);
        EXPECT_THAT(RunWrite(*plan), IsOk());
    }
    EXPECT_THAT(transport_.ReadTimeouts(), Each(200ms));
    EXPECT_TRUE(transport_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, CrcDrainChecksCancellationAfterAnAcceptedReply)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0xF722EF49, false);
    boundary_.after_reply = [&](bytes::Byte) { cancellation_.SetCancelled(true); };
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kCancelled, "CRC stale-frame drain cancelled"));
    EXPECT_THAT(transport_.ReadTimeouts(), IsEmpty());
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, BeefRequestFramesOpcodeAndPayloadLength)
{
    const auto plan = SmallPlan(bytes::Bytes(0x1000), 0x010203, {{0x010203, 0x1000}});
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x010203, 0x1000}, 0xF722EF49);
    ASSERT_THAT(RunWrite(*plan), IsOk());
    ASSERT_EQ(boundary_.requests.size(), 1U);
    const auto& framed = boundary_.requests.front();
    // The public CRC operation replaces the old arbitrary B6/three-byte
    // payload. Its address carries the same 01 02 03 bytes after a zero prefix.
    ASSERT_EQ(framed.size(), 13U);
    EXPECT_EQ(framed[0], 0xBE);
    EXPECT_EQ(framed[1], 0xEF);
    EXPECT_EQ(framed[2], 0x00);
    EXPECT_EQ(framed[3], 0x09);
    EXPECT_EQ(framed[4], 0x02);
    EXPECT_EQ(framed[5], 0x00);
    EXPECT_EQ(framed[6], 0x01);
    EXPECT_EQ(framed[7], 0x02);
    EXPECT_EQ(framed[8], 0x03);
    EXPECT_THAT(std::span{framed}.subspan(9), ElementsAre(0x00, 0x00, 0x10, 0x00));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, BeefRequestWithNoPayloadStillCountsTheOpcode)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptCrc({0x2000, 0x1000}, 0);
    ScriptInitialize();
    ScriptVoltage();
    ScriptWindow();
    ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
    ASSERT_THAT(RunWrite(*plan), IsOk());
    ASSERT_GE(boundary_.requests.size(), 2U);
    const auto& framed = boundary_.requests[1];
    ASSERT_EQ(framed.size(), 5U);
    EXPECT_EQ(framed[3], 0x01);
    EXPECT_EQ(framed[4], 0x05);
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWriteTest, ElapsedMillisecondsReportsAtLeastOne)
{
    const auto plan = SmallPlan();
    ASSERT_THAT(plan, IsOk());
    for (const auto elapsed : {0ms, 250ms})
    {
        SCOPED_TRACE(elapsed.count());
        clock_.SetNowAutoAdvance(elapsed);
        ScriptCrc({0x2000, 0x1000}, 0);
        ScriptInitialize();
        ScriptVoltage();
        ScriptWindow();
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
        EXPECT_THAT(RunWrite(*plan), IsOk());
        EXPECT_THAT(
            events_.logs,
            Contains(Pair(LogLevel::kInfo, elapsed == 0ms ? "Write flash buffer: 0x00002000 (0% - 512000 B/s, ~ 1 s)"
                                                          : "Write flash buffer: 0x00002000 (0% - 2048 B/s, ~ 2 s)")));
        EXPECT_TRUE(channel_.ScriptConsumed());
        events_.logs.clear();
    }
}

} // namespace
} // namespace fastecu::flash
