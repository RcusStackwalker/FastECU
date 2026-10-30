#include "src/backend/flash/ecu/beef_can_kernel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <functional>
#include <numeric>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_types.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/mock_clock.h"
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
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::Pair;
using ::testing::StrEq;
using ReceiveOutcome = Result<std::optional<bytes::Bytes>>;

// Independent encrypted words from the legacy transcript oracle.
bytes::Bytes UploadBlock(bytes::Bytes head, int zero_words, bytes::ByteView tail = {})
{
    constexpr std::array<bytes::Byte, 4> kZeroWord{0xE7, 0xE2, 0x14, 0x30};
    head.append_range(std::views::repeat(kZeroWord, zero_words) | std::views::join);
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

constexpr DensoBeefCanProfile kPetrol{.device = "ECU", .family = "petrol", .pdu_format = BeefPduFormat::kHex};
constexpr DensoBeefCanProfile kDiesel{.device = "ECU", .family = "diesel", .pdu_format = BeefPduFormat::kDescribe};
constexpr DensoBeefCanProfile kTcu{.device = "TCU",
                                   .family = "TCU",
                                   .pdu_format = BeefPduFormat::kHex,
                                   .init_timeout = 2000ms,
                                   .upper_hex_sizes = true};
constexpr BeefKernelTransferPolicy kPetrolPolicy{{50ms, 2000ms, 3000ms, 10}, 2000ms};

// A test may synchronously change its nonconst plan after compare to exercise
// the pre-erase guard through Write. No production callback or private seam.
class ValidationBoundaryChannel final : public uds::ScriptedUdsChannel
{
  public:
    ReceiveOutcome Receive(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        auto reply = ScriptedUdsChannel::Receive(timeout, cancellation);
        after_reply();
        return reply;
    }
    std::function<void()> after_reply = [] {};
};

class BeefCanKernel : public ::testing::Test
{
  protected:
    void ExpectLog(std::string_view text, LogLevel level = LogLevel::kInfo)
    {
        EXPECT_THAT(events_.logs, Contains(Pair(level, text)));
    }
    fastecu::flash::BeefCanKernel Kernel(const DensoBeefCanProfile& profile = kPetrol)
    {
        return fastecu::flash::BeefCanKernel(transport_, clock_, cancellation_, events_, channel_, profile);
    }

    Status Transfer(const KernelImage& image, const BeefKernelTransferPolicy& policy = kPetrolPolicy,
                    const DensoBeefCanProfile& profile = kPetrol)
    {
        const auto status = Kernel(profile).TransferPayload(image, policy);
        EXPECT_TRUE(channel_.ScriptConsumed());
        EXPECT_TRUE(transport_.ScriptConsumed());
        return status;
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
        channel_.ExpectSend(UploadBlock({0xB6, 0xFF, 0x40, 0x00}, 32));
        channel_.ExpectSend(
            UploadBlock({0xB6, 0xFF, 0x40, 0x80, 0xC0, 0x41, 0xD4, 0xCA}, 30, bytes::Bytes{0x42, 0x61, 0xDB, 0x2C}));
        channel_.ExpectSend(bytes::Bytes{0xB6, 0xFF, 0x41, 0x00});
        for (int block = 0; block < 3; ++block)
        {
            transport_.QueueNoFrame();
        }
    }

    void ReadPage(bool second = false, bytes::Byte fill = 0xAB, bool fail = false)
    {
        channel_.ExpectSend(
            bytes::Bytes{0xBE, 0xEF, 0, 7, 3, 0, 0, static_cast<bytes::Byte>(second ? 0x14 : 0x10), 0, 4, 0});
        bytes::Bytes reply{0xBE, 0xEF, 4, 1, 0x43};
        reply.resize(0x405, fill);
        channel_.QueueOutcome(fail ? Fail(ErrorKind::kTimeout, "no reply") : ReceiveOutcome(std::move(reply)));
    }

    KernelImage Image129()
    {
        bytes::Bytes image(129);
        image.back() = 1;
        return {.id = "kernel", .load_address = 0xFFFF4000U, .bytes = image};
    }

    ScriptedCanFlashTransport transport_;
    MockClock clock_;
    FakeCancellationToken cancellation_;
    RecordingEventSink events_;
    ValidationBoundaryChannel channel_;
};

TEST_F(BeefCanKernel, ReadBeefMemoryTruncatesTheLastPageAndStopsOnFailureOrCancellation)
{
    const MemoryRegion region{0x1000, 0x500};
    PhaseReporter progress(events_, "Read", 1, 1, 0x500);
    ReadPage();
    ReadPage(true);

    const auto rom = Kernel().Read(region, progress);

    ASSERT_THAT(rom, IsOk());
    EXPECT_THAT(*rom, ::testing::AllOf(::testing::SizeIs(0x500), test_bytes::BytesFilledWith(bytes::Byte{0xAB})));
    EXPECT_EQ(channel_.SendsConsumed(), 2U);
    EXPECT_TRUE(channel_.ScriptConsumed());

    ReadPage();
    ReadPage(true, 0xAB, true);
    EXPECT_THAT(Kernel().Read(region, progress), IsErr(ErrorKind::kTimeout));
    cancellation_.SetCancelled(true);
    EXPECT_THAT(Kernel().Read(region, progress), IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernel, ReadKeepsTheFixedPageLengthAndBoundsTailAndProgressToTheRequestedRegion)
{
    const MemoryRegion region{0x1000, 0x401};
    // A larger reporter total exposes the operation's own progress cap.
    PhaseReporter progress(events_, "Read", 1, 1, 0x800);
    ReadPage();
    ReadPage(true, 0xCD);

    const auto rom = Kernel().Read(region, progress);

    ASSERT_THAT(rom, IsOk());
    ASSERT_EQ(rom->size(), 0x400U + 1U);
    EXPECT_THAT(bytes::ByteView(*rom).first(0x400), test_bytes::BytesFilledWith(bytes::Byte{0xAB}));
    EXPECT_EQ(rom->back(), 0xCD);
    EXPECT_TRUE(channel_.ScriptConsumed());
    EXPECT_THAT(channel_.timeouts, ElementsAre(2000ms, 2000ms));
    EXPECT_THAT(events_.phase_progress_calls | std::views::transform(&RecordedPhaseProgress::done),
                ElementsAre(0, 0x400, 0x401));
}

TEST_F(BeefCanKernel, KernelDownloadRequestEncodesItsTwo24BitFields)
{
    Expect129ByteTransfer();

    EXPECT_THAT(Transfer(Image129()), IsOk());
}

TEST_F(BeefCanKernel, BeefKernelUploadPayloadNamesTheFamilyInBothSizeErrors)
{
    EXPECT_THAT(Transfer({.id = "empty", .load_address = 0x1000, .bytes = {}}),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("petrol kernel upload size is invalid")));
    EXPECT_THAT(
        Transfer({.id = "large", .load_address = 0x1000, .bytes = bytes::Bytes(0x1000000)}, kPetrolPolicy, kDiesel),
        IsErrWith(ErrorKind::kInvalidConfig, StrEq("diesel kernel upload exceeds the 24-bit wire length")));
    EXPECT_EQ(channel_.SendCalls(), 0U);
}

TEST_F(BeefCanKernel, BeefKernelPayloadReplacesThePaddedLastWordWithTheChecksum)
{
    // A 125-byte image pads to 128; AA in the last word is replaced by
    // 5AA5A559, the complement of the first word 00000001. The first word,
    // thirty zero words and checksum encrypt to these fixed literal words.
    bytes::Bytes image(125);
    image[3] = 1;
    image.back() = 0xAA;
    Download(bytes::Bytes{0x34, 0x04, 0x33, 0x00, 0x10, 0x00, 0x00, 0x00, 0x80});
    const auto block =
        UploadBlock({0xB6, 0x00, 0x10, 0x00, 0xFA, 0x4F, 0x60, 0x15}, 30, bytes::Bytes{0x86, 0xA7, 0x05, 0x3A});
    ASSERT_EQ(block.size(), 132U);
    channel_.ExpectSend(block);
    channel_.ExpectSend(bytes::Bytes{0xB6, 0x00, 0x10, 0x80});
    transport_.QueueNoFrame();
    transport_.QueueNoFrame();

    EXPECT_THAT(Transfer({.id = "kernel", .load_address = 0x1000, .bytes = image}), IsOk());
    EXPECT_THAT(Transfer({.id = "empty", .load_address = 0x1000, .bytes = {}}), IsErr(ErrorKind::kInvalidConfig));
}

TEST_F(BeefCanKernel, UploadBeefKernelBlocksEndsWithAnAddressOnlyBlock)
{
    Expect129ByteTransfer();

    EXPECT_THAT(Transfer(Image129()), IsOk());
    EXPECT_EQ(channel_.SendsConsumed(), 4U);
}

TEST_F(BeefCanKernel, FamilyPoliciesPreserveDownloadAndBlockReplyTiming)
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
        const auto start = clock_.Elapsed();
        const auto reads = channel_.timeouts.size();
        const auto raw_reads = transport_.ReadTimeouts().size();

        ASSERT_THAT(Transfer(Image129(), policy, *profile), IsOk());
        EXPECT_EQ(clock_.Elapsed() - start, 50ms);
        EXPECT_EQ(channel_.timeouts.at(reads), policy.download_policy.read_timeout);
        EXPECT_EQ(channel_.timeouts.size(), reads + 11);
        EXPECT_THAT(std::span{channel_.timeouts}.subspan(reads + 1), Each(3000ms));
        EXPECT_THAT(std::span{transport_.ReadTimeouts()}.subspan(raw_reads),
                    ElementsAre(policy.block_reply_timeout, policy.block_reply_timeout, policy.block_reply_timeout));
    }
}

TEST_F(BeefCanKernel, DiscardB6ReplyIgnoresStaleContentButNotCancellationOrDisconnection)
{
    for (const auto kind : {ErrorKind::kBadResponse, ErrorKind::kCancelled, ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(kind));
        channel_.ExpectSend(bytes::Bytes{0x34, 0x04, 0x33, 0x00, 0x10, 0x00, 0x00, 0x00, 0x80});
        channel_.QueueReceive(bytes::Bytes{0x74, 0x20});
        const auto block = UploadBlock({0xB6, 0x00, 0x10, 0x00}, 31, bytes::Bytes{0x8F, 0xC6, 0x58, 0x3A});
        EXPECT_EQ(block.size(), 132U);
        channel_.ExpectSend(block);
        transport_.QueueError(kind, "reply");
        if (kind == ErrorKind::kBadResponse)
        {
            channel_.ExpectSend(bytes::Bytes{0xB6, 0x00, 0x10, 0x80});
            transport_.QueueNoFrame();
        }
        const ::testing::Matcher<Status> expected =
            kind == ErrorKind::kBadResponse ? ::testing::Matcher<Status>(IsOk()) : IsErr(kind);
        EXPECT_THAT(Transfer({.id = "small", .load_address = 0x1000, .bytes = bytes::Bytes(5)}), expected);
    }
}

TEST_F(BeefCanKernel, BlockSendFailureAndCancellationStopTheTransfer)
{
    Download(bytes::Bytes{0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00});
    channel_.ExpectSend(UploadBlock({0xB6, 0xFF, 0x40, 0x00}, 32), Fail(ErrorKind::kTimeout, "no ack"));
    EXPECT_THAT(Transfer(Image129()), IsErr(ErrorKind::kTimeout));
    EXPECT_THAT(transport_.ReadTimeouts(), IsEmpty());
    cancellation_.SetCancelled(true);
    EXPECT_THAT(Transfer(Image129()), IsErr(ErrorKind::kCancelled));

    cancellation_.SetCancelled(false);
    Download(bytes::Bytes{0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00});
    channel_.after_reply = [&] { cancellation_.SetCancelled(true); };
    EXPECT_THAT(Transfer(Image129()), IsErrWith(ErrorKind::kCancelled, "kernel upload cancelled"));
    channel_.after_reply = [] {};

    cancellation_.SetCancelled(false);
    Download(bytes::Bytes{0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00});
    channel_.ExpectSend(UploadBlock({0xB6, 0xFF, 0x40, 0x00}, 32));
    transport_.QueueNoFrame();
    cancellation_.CancelOnCheck(cancellation_.CheckCount() + 8);
    EXPECT_THAT(Transfer(Image129()), IsErrWith(ErrorKind::kCancelled, "kernel upload cancelled"));
}

TEST_F(BeefCanKernel, DownloadFailuresPreserveEachProfilesLogWording)
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
            EXPECT_THAT(Transfer(Image129(), kPetrolPolicy, *profile), IsErr(kind));
            if (profile == &kDiesel)
            {
                ExpectLog(kind == ErrorKind::kTimeout ? "No valid response from ECU"
                                                      : "Wrong response from ECU: bad download",
                          LogLevel::kError);
            }
            else
            {
                EXPECT_THAT(events_.logs, ::testing::Not(Contains(Pair(LogLevel::kError, ::testing::_))));
            }
        }
        events_.logs.clear();
        channel_.ExpectSend(bytes::Bytes{0x34, 0x04, 0x33, 0xFF, 0x40, 0x00, 0x00, 0x01, 0x00});
        channel_.QueueReceive(bytes::Bytes{0x74, 0x21});
        EXPECT_THAT(Transfer(Image129(), kPetrolPolicy, *profile),
                    IsErrWith(ErrorKind::kBadResponse, StrEq("unexpected response during kernel download")));
        ExpectLog(profile == &kDiesel   ? "Wrong response from ECU: unexpected response during kernel download"
                  : profile == &kPetrol ? "Wrong response from ECU during kernel download"
                                        : "Wrong response from TCU during kernel download",
                  LogLevel::kError);
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

class BeefCanKernelWrite : public BeefCanKernel
{
  protected:
    BeefCanKernelWrite()
        : plans_{std::move(SmallPlan()).value(),
                 std::move(SmallPlan(bytes::Bytes(0x1000), 0x2000, {{0x2000, 0x1000}}, FlashOperation::kTestWrite))
                     .value()}
    {
    }

    Result<FlashPlan> PlanFor(FlashOperation operation = FlashOperation::kWrite)
    {
        return BuildSubaruDensoSh7058CanPlan(
            operation, "sub_ecu_denso_sh7058_can", "SH7058", bytes::Bytes(0x100000),
            KernelImage{.id = "petrol-sh7058-kernel", .load_address = 0xFFFF3000, .bytes = {1, 2, 3, 4}});
    }

    static Result<FlashPlan> SmallPlan(bytes::Bytes image = bytes::Bytes(0x1000), std::uint32_t base = 0x2000,
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
        const auto status =
            Kernel(profile).Write(plan, phases, compare).transform([&] { phases.Start("Complete", 1).Complete(); });
        EXPECT_TRUE(channel_.ScriptConsumed());
        EXPECT_TRUE(transport_.ScriptConsumed());
        EXPECT_EQ(channel_.SendCalls(), channel_.SendsConsumed());
        return status;
    }

    void ScriptCrc(const MemoryRegion& block, std::uint32_t crc, bool drain = true)
    {
        bytes::Bytes payload = Be32(block.start);
        payload.append_range(Be32(block.length));
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
        channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x05});
        channel_.QueueReceive(BeefFrame(0x45, Be32(message_size)));
        channel_.ExpectSend(BeefFrame(0x06));
        channel_.QueueReceive(BeefFrame(0x46, Be32(block_size)));
        channel_.ExpectSend(BeefFrame(mode_opcode));
        channel_.QueueReceive(BeefFrame(static_cast<bytes::Byte>(mode_opcode | 0x40), {}));
    }

    void ScriptVoltage(std::uint16_t voltage = 100)
    {
        channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x04});
        channel_.QueueReceive(bytes::Bytes{0xBE, 0xEF, 0x00, 0x03, 0x44, static_cast<bytes::Byte>(voltage >> 8U),
                                           static_cast<bytes::Byte>(voltage)});
    }

    struct WriteReplies
    {
        std::uint32_t crc = 0;
        std::uint32_t message_size = 0x1000;
        std::uint32_t block_size = 0x200;
        std::uint16_t voltage = 100;
    };

    void ScriptBeforeWindow(bool test_write, const WriteReplies& replies)
    {
        ScriptCrc({0x2000, 0x1000}, replies.crc);
        ScriptInitialize(test_write ? 0x21 : 0x20, replies.message_size, replies.block_size);
        ScriptVoltage(replies.voltage);
    }

    Status CompleteWindow(bool test_write = false, const DensoBeefCanProfile& profile = kPetrol,
                          const WriteReplies& replies = {0, 0x1000, 0x200, 100}, std::uint32_t after_crc = 0xF722EF49)
    {
        const auto timeouts = channel_.timeouts.size();
        const auto progress = events_.phase_progress_calls.size();
        ScriptBeforeWindow(test_write, replies);
        ScriptWindow(0x2000, test_write);
        ScriptCrc({0x2000, 0x1000}, after_crc);
        const auto status = RunWrite(plans_[test_write ? 1 : 0], profile);
        EXPECT_THAT(channel_.timeouts | std::views::drop(timeouts),
                    ElementsAre(3000ms, profile.init_timeout, profile.init_timeout, profile.init_timeout, 500ms, 3000ms,
                                800ms, 800ms, 800ms, 800ms, 800ms, 800ms, 800ms, 800ms, 3000ms, 3000ms));
        EXPECT_THAT(events_.phase_progress_calls | std::views::drop(progress),
                    Contains(::testing::AllOf(Field(&RecordedPhaseProgress::phase_name, "Write"),
                                              Field(&RecordedPhaseProgress::done, 0x1000))));
        return status;
    }

    void ExpectInitFailure(ReceiveOutcome reply, const ::testing::Matcher<Status>& expected,
                           const DensoBeefCanProfile& profile = kPetrol, std::size_t failing = 0)
    {
        ScriptCrc({0x2000, 0x1000}, 0);
        const std::array<bytes::Byte, 3> opcodes{0x05, 0x06, 0x20};
        for (std::size_t index = 0; index < failing; ++index)
        {
            channel_.ExpectSend(BeefFrame(opcodes[index]));
            channel_.QueueReceive(BeefFrame(static_cast<bytes::Byte>(opcodes[index] | 0x40), bytes::Bytes(4)));
        }
        channel_.ExpectSend(BeefFrame(opcodes[failing]));
        channel_.QueueOutcome(std::move(reply));
        std::size_t logs_before_reply = 0;
        channel_.after_reply = [&] { logs_before_reply = events_.logs.size(); };
        EXPECT_THAT(RunWrite(plans_[0], profile), expected);
        EXPECT_THAT(std::span{events_.logs}.subspan(logs_before_reply), IsEmpty());
        channel_.after_reply = [] {};
    }

    // One 0x1000 zero-filled window: erase, eight 0x200 chunks, commit/validate.
    // The CRC F722EF49 is the independent literal retained from the session suite.
    void ScriptWindow(std::uint32_t address = 0x2000, bool test_write = false, int last_exchange = 9,
                      bool fail_last = false)
    {
        for (int step = 0; step <= last_exchange; ++step)
        {
            bytes::Byte opcode = step == 0 ? 0x25 : step == 9 ? (test_write ? 0x23 : 0x24) : 0x22;
            const bool chunk = step > 0 && step < 9;
            bytes::Bytes payload = Be32(address + (chunk ? static_cast<std::uint32_t>(step - 1) * 0x200 : 0));
            payload.resize(chunk ? 0x204 : 4, 0);
            payload.append_range(step == 9 ? bytes::Bytes{0x10, 0x00, 0xF7, 0x22, 0xEF, 0x49} : bytes::Bytes{});
            channel_.ExpectSend(BeefFrame(opcode, payload));
            channel_.QueueOutcome(fail_last && step == last_exchange
                                      ? Fail(ErrorKind::kTimeout, "no reply")
                                      : ReceiveOutcome(BeefFrame(static_cast<bytes::Byte>(opcode | 0x40))));
        }
    }

    const std::array<FlashPlan, 2> plans_;
};

TEST_F(BeefCanKernelWrite, QueryCrcReadsFourBytesAndDrainsTheStaleFrame)
{

    ScriptCrc({0x2000, 0x1000}, 0xF722EF49);
    EXPECT_THAT(RunWrite(plans_[0]), IsOk());
    ExpectLog("ROM CRC: 0xf722ef49 IMG CRC: 0xf722ef49", LogLevel::kDebug);
    EXPECT_EQ(channel_.last_timeout, 3000ms);
    EXPECT_THAT(transport_.ReadTimeouts(), ElementsAre(200ms));
}

TEST_F(BeefCanKernelWrite, CompareBlocksNamesTheFamilyWhenABlockLeavesTheImage)
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

TEST_F(BeefCanKernelWrite, CompareBlocksFlagsOnlyDifferingBlocks)
{
    const auto plan = PlanFor();
    ASSERT_THAT(plan, IsOk());
    ScriptCompare(3);
    ScriptInitialize();
    ScriptVoltage();
    ScriptWindow(0x3000);
    ScriptCompare();
    ASSERT_THAT(RunWrite(*plan), IsOk());
    ExpectLog(" (total: 1)");
    ExpectLog("3, ");
}

TEST_F(BeefCanKernelWrite, InitializeFlashSelectsTestOrRealWriteMode)
{
    for (const bool test_write : {true, false})
    {
        SCOPED_TRACE(test_write);
        ASSERT_THAT(CompleteWindow(test_write, kPetrol, {.crc = 0xDEADBEEF}), IsOk());
    }
    ExpectLog("Test write mode on, no actual flash write is performed");
    ExpectLog("Test write mode off, perform actual flash write");
    ExpectLog(": 0x1000");
    ExpectLog(": 0x0200");
}

TEST_F(BeefCanKernelWrite, InitializeFlashUsesTheProfileTimeoutAndHexCase)
{
    for (const auto *profile : {&kPetrol, &kTcu})
    {
        SCOPED_TRACE(profile->family);
        ASSERT_THAT(CompleteWindow(false, *profile, {.crc = 0xDEADBEEF, .message_size = 0xABC, .block_size = 0xABC}),
                    IsOk());
        ExpectLog(profile == &kPetrol ? ": 0x0abc" : ": 0x0ABC");
        events_.logs.clear();
    }
}

TEST_F(BeefCanKernelWrite, WriteMemoryWithNoDifferenceFlashesNothing)
{
    const auto plan = PlanFor();
    ASSERT_THAT(plan, IsOk());
    ScriptCompare();
    ASSERT_THAT(RunWrite(*plan), IsOk());
    ExpectLog("*** Compare results no difference between ROM and ECU data, no flashing needed! ***");

    EXPECT_THAT(events_.phase_progress_calls, Each(Field(&RecordedPhaseProgress::phase_count, 4)));
    EXPECT_THAT(events_.phase_progress_calls | std::views::filter([](const auto& call) { return call.done == 0; }) |
                    std::views::transform(&RecordedPhaseProgress::phase_name) | std::ranges::to<std::vector>(),
                ElementsAre("Kernel", "Compare", "Write", "Complete"));
    EXPECT_EQ(events_.phase_progress_calls.back().done, 1);
}

TEST_F(BeefCanKernelWrite, WriteMemoryFailsWithTheExchangeErrorWhenTheVoltageCheckTimesOut)
{
    const auto plan = PlanFor();
    ASSERT_THAT(plan, IsOk());
    ScriptCompare(0);
    ScriptInitialize();
    channel_.ExpectSend(BeefFrame(0x04));
    channel_.QueueNoFrame();
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"));
    ExpectLog("Block 0 reflash failed.");

    ScriptCompare(0);
    ScriptInitialize();
    cancellation_.CancelOnCheck(cancellation_.CheckCount() + kBlocks.size() * 7 + 10);
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kCancelled, "flash block loop cancelled"));
}

TEST_F(BeefCanKernelWrite, CompareBeefBlocksChecksEachRegionAgainstItsOwnSliceOfTheImage)
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
    ExpectLog(" (total: 1)");
    EXPECT_THAT(events_.logs |
                    std::views::filter(
                        [](const auto& log)
                        { return log.first == LogLevel::kInfo && (log.second == "\tYES" || log.second == "\tNO"); }) |
                    std::views::values | std::ranges::to<std::vector>(),
                ElementsAre("\tYES", "\tNO", "\tYES"));
    EXPECT_EQ(clock_.Elapsed(), 5ms * 3);
    ExpectLog("1, ");
}

TEST_F(BeefCanKernelWrite, CompareBeefBlocksRejectsRegionsOutsideTheImageAndNamesTheFamily)
{
    for (const auto& [size, base, queried] : {std::tuple{11U, 0x1000U, 2U}, {12U, 0x1001U, 0U}, {12U, 0x1000U, 3U}})
    {
        SCOPED_TRACE(std::format("size={} base={}", size, base));
        const auto plan = SmallPlan(bytes::Bytes(size), base, {{0x1000, 4}, {0x1004, 4}, {0x1008, 4}});
        ASSERT_THAT(plan, IsOk());
        for (std::uint32_t index = 0; index < queried; ++index)
        {
            ScriptCrc({0x1000 + 4 * index, 4}, 0xCD3AA48B);
        }
        const ::testing::Matcher<Status> expected =
            queried == 3 ? ::testing::Matcher<Status>(IsOk())
                         : IsErrWith(ErrorKind::kInvalidConfig, "petrol CRC block is outside the image");
        EXPECT_THAT(RunWrite(*plan), expected);
    }
}

TEST_F(BeefCanKernelWrite, CompareBeefBlocksStopsOnQueryFailureAndOnCancellationDuringTheSettle)
{
    const auto plan = SmallPlan(bytes::Bytes(12), 0x1000, {{0x1000, 4}, {0x1004, 4}, {0x1008, 4}});
    ASSERT_THAT(plan, IsOk());
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x09, 0x02, 0, 0, 0x10, 0, 0, 0, 0, 4});
    channel_.QueueError(ErrorKind::kTimeout, "no CRC reply");
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kTimeout, "no CRC reply"));
    EXPECT_CALL(clock_, Sleep(5ms, ::testing::_))
        .WillOnce(
            [&](auto duration, const auto& token)
            {
                cancellation_.SetCancelled(true);
                return clock_.Fake().Sleep(duration, token);
            });
    ScriptCrc({0x1000, 4}, 0xCD3AA48B);
    EXPECT_THAT(RunWrite(*plan), IsErr(ErrorKind::kCancelled));

    cancellation_.SetCancelled(false);
    ASSERT_TRUE(::testing::Mock::VerifyAndClearExpectations(&clock_));
    cancellation_.CancelOnCheck(cancellation_.CheckCount() + 8);
    ScriptCrc({0x1000, 4}, 0xCD3AA48B);
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kCancelled, "ROM compare cancelled"));
}

TEST_F(BeefCanKernelWrite, ASendFailureStopsTheExchangeBeforeReceiving)
{
    const auto plan = SmallPlan(bytes::Bytes(4), 0x1000, {{0x1000, 4}});
    ASSERT_THAT(plan, IsOk());
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x09, 0x02, 0, 0, 0x10, 0, 0, 0, 0, 4},
                        Fail(ErrorKind::kDisconnected, "send failed"));
    EXPECT_THAT(RunWrite(*plan), IsErrWith(ErrorKind::kDisconnected, "send failed"));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernelWrite, InitializeBeefFlashSelectsTheModeAndStopsAtTheFirstFailingExchange)
{
    for (const bool test_write : {false, true})
    {
        SCOPED_TRACE(test_write);
        EXPECT_THAT(
            CompleteWindow(test_write, test_write ? kTcu : kPetrol, {.message_size = 0xABCD, .block_size = 0xABCD}),
            IsOk());
        ExpectLog(test_write ? ": 0xABCD" : ": 0xabcd");
    }
    for (const std::size_t failing : {0U, 1U, 2U})
    {
        SCOPED_TRACE(failing);

        ExpectInitFailure(Fail(ErrorKind::kTimeout, "no reply"), IsErrWith(ErrorKind::kTimeout, "no reply"), kPetrol,
                          failing);
    }
}

TEST_F(BeefCanKernelWrite, FlashBeefBlockRejectsBlocksThatDoNotFitTheImageAndNamesTheFamily)
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
        const auto voltage_reply = channel_.SendsConsumed() + 5; // CRC, three init replies, voltage
        channel_.after_reply = [&]
        {
            if (channel_.SendsConsumed() == voltage_reply)
            {
                validation_region = test.block;
            }
        };
        EXPECT_THAT(RunWrite(*plan, kDiesel), IsErrWith(ErrorKind::kInvalidConfig, test.detail));
        channel_.after_reply = [] {};
    }
}

TEST_F(BeefCanKernelWrite, FlashBeefBlockErasesWritesAndCommitsOrValidatesOneWindow)
{
    for (const bool test_write : {false, true})
    {
        SCOPED_TRACE(test_write);
        EXPECT_THAT(CompleteWindow(test_write), IsOk());
    }
    for (const auto& [last, kind] : {std::pair{0, ErrorKind::kTimeout},
                                     {1, ErrorKind::kTimeout},
                                     {9, ErrorKind::kTimeout},
                                     {0, ErrorKind::kCancelled},
                                     {8, ErrorKind::kCancelled}})
    {
        SCOPED_TRACE(std::format("last={} kind={}", last, static_cast<int>(kind)));
        ScriptBeforeWindow(false, {});
        ScriptWindow(0x2000, false, last, kind == ErrorKind::kTimeout);
        const auto cancel_after = channel_.SendsConsumed() + 6 + static_cast<std::size_t>(last);
        channel_.after_reply = [&]
        {
            if (kind == ErrorKind::kCancelled && channel_.SendsConsumed() == cancel_after)
            {
                cancellation_.SetCancelled(true);
            }
        };
        EXPECT_THAT(RunWrite(plans_[0]), IsErr(kind));
        ExpectLog("Reflash error! Do not panic, do not reset the ECU immediately. The "
                  "kernel is most likely still running and receiving commands!",
                  LogLevel::kError);
        channel_.after_reply = [] {};
        cancellation_.SetCancelled(false);
        events_.logs.clear();
    }
}

TEST_F(BeefCanKernelWrite, WriteBeefMemoryLogsTheVoltageAndTheTimeLeftFromTheBytesStillToWrite)
{
    EXPECT_THAT(CompleteWindow(false, kPetrol, {}, 0xDEADBEEF), IsOk());
    ExpectLog(": 2V");
    ExpectLog("*** ERROR IN FLASH PROCESS ***", LogLevel::kError);
    ExpectLog("Don't power off your ECU, kernel is still running and you can try flashing again!", LogLevel::kError);
}

TEST_F(BeefCanKernelWrite, FlashBeefBlockReportsTheTimeLeftForTheRestOfTheWrite)
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
    ExpectLog("Write flash buffer: 0x00002000 (0% - 512000 B/s, ~ 5 s)");
    ExpectLog("Block 0 reflash complete.");
}

TEST_F(BeefCanKernelWrite, WriteExchangeWrapsAndUnwrapsBeef)
{

    ASSERT_THAT(CompleteWindow(), IsOk());
    ExpectLog(": 0x1000");
}

TEST_F(BeefCanKernelWrite, WriteExchangeRejectsAWrongOpcode)
{
    ExpectInitFailure(bytes::Bytes{0xBE, 0xEF, 0, 1, 0x46},
                      IsErrWith(ErrorKind::kBadResponse, "unexpected BEEF response opcode 0x46, expected 0x45"));
}

TEST_F(BeefCanKernelWrite, WriteExchangeRejectsAShortPayload)
{
    ExpectInitFailure(bytes::Bytes{0xBE, 0xEF, 0, 2, 0x45, 0x01},
                      IsErrWith(ErrorKind::kBadResponse, "short BEEF response payload"));
}

TEST_F(BeefCanKernelWrite, WriteExchangeTimeoutIsTheReturnedErrorAndLogsNothing)
{
    ExpectInitFailure(std::nullopt, IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"), kDiesel);
}

TEST_F(BeefCanKernelWrite, WriteExchangeSurfacesAChannelBadResponse)
{
    ExpectInitFailure(Fail(ErrorKind::kBadResponse, "expected CAN reply id 0x7e8, got 0x123"),
                      IsErrWith(ErrorKind::kBadResponse, "expected CAN reply id 0x7e8, got 0x123"), kDiesel);
}

TEST_F(BeefCanKernelWrite, BeefExchangeReturnsThePayloadOfAnOpcodeOrSizeCheckedReply)
{
    // The two-byte AA BB payload is observed through voltage's minimum of two.
    EXPECT_THAT(CompleteWindow(false, kPetrol, {.voltage = 0xAABB}), IsOk());
    ExpectLog(": 874.14V");
    for (const auto& [reply, detail] : std::array{
             std::pair{bytes::Bytes{0xBE, 0xEF, 0, 3, 0x45, 0xAA, 0xBB}, "short BEEF response payload"},
             std::pair{bytes::Bytes{0xBE, 0xEF, 0, 1, 0x46}, "unexpected BEEF response opcode 0x46, expected 0x45"},
             std::pair{bytes::Bytes{0xBE}, "short BEEF response envelope"}})
    {
        SCOPED_TRACE(detail);
        ExpectInitFailure(reply, IsErrWith(ErrorKind::kBadResponse, detail));
    }
    ExpectInitFailure(std::nullopt, IsErrWith(ErrorKind::kTimeout, "no CAN response within the read timeout"));
    ExpectInitFailure(Fail(ErrorKind::kDisconnected, "gone"), IsErr(ErrorKind::kDisconnected));
}

TEST_F(BeefCanKernelWrite, CrcStaleDrainIgnoresContentButPropagatesCancellationAndDisconnection)
{

    for (const auto kind : {ErrorKind::kTimeout, ErrorKind::kBadResponse, ErrorKind::kInternal, ErrorKind::kCancelled,
                            ErrorKind::kDisconnected})
    {
        SCOPED_TRACE(static_cast<int>(kind));
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49, false);
        transport_.QueueError(kind, "stale read");
        const ::testing::Matcher<Status> expected = kind == ErrorKind::kCancelled || kind == ErrorKind::kDisconnected
                                                        ? ::testing::Matcher<Status>(IsErrWith(kind, "stale read"))
                                                        : IsOk();
        EXPECT_THAT(RunWrite(plans_[0]), expected);
    }
    for (const auto& stale : {bytes::Bytes{0xBE}, bytes::Bytes{0, 0, 1, 0x23, 0x7F, 2, 0x13}})
    {
        SCOPED_TRACE(::testing::PrintToString(stale));
        ScriptCrc({0x2000, 0x1000}, 0xF722EF49, false);
        transport_.QueueRead(stale);
        EXPECT_THAT(RunWrite(plans_[0]), IsOk());
    }
    EXPECT_THAT(transport_.ReadTimeouts(), Each(200ms));
}

TEST_F(BeefCanKernelWrite, CrcDrainChecksCancellationAfterAnAcceptedReply)
{

    ScriptCrc({0x2000, 0x1000}, 0xF722EF49, false);
    channel_.after_reply = [&] { cancellation_.SetCancelled(true); };
    EXPECT_THAT(RunWrite(plans_[0]), IsErrWith(ErrorKind::kCancelled, "CRC stale-frame drain cancelled"));
    EXPECT_THAT(transport_.ReadTimeouts(), IsEmpty());
}

TEST_F(BeefCanKernelWrite, BeefRequestFramesOpcodeAndPayloadLength)
{
    const auto plan = SmallPlan(bytes::Bytes(0x1000), 0x010203, {{0x010203, 0x1000}});
    ASSERT_THAT(plan, IsOk());
    // The public CRC operation replaces the old arbitrary B6/three-byte
    // payload. Its address carries the same 01 02 03 bytes after a zero prefix.
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x09, 0x02, 0x00, 0x01, 0x02, 0x03, 0x00, 0x00, 0x10, 0x00});
    channel_.QueueReceive(BeefFrame(0x42, Be32(0xF722EF49)));
    transport_.QueueNoFrame();
    ASSERT_THAT(RunWrite(*plan), IsOk());
}

TEST_F(BeefCanKernelWrite, BeefRequestWithNoPayloadStillCountsTheOpcode)
{

    ASSERT_THAT(CompleteWindow(), IsOk());
}

TEST_F(BeefCanKernelWrite, ElapsedMillisecondsReportsAtLeastOne)
{

    for (const auto elapsed : {0ms, 250ms})
    {
        SCOPED_TRACE(elapsed.count());
        clock_.Fake().SetNowAutoAdvance(elapsed);
        EXPECT_THAT(CompleteWindow(), IsOk());
        ExpectLog(elapsed == 0ms ? "Write flash buffer: 0x00002000 (0% - 512000 B/s, ~ 1 s)"
                                 : "Write flash buffer: 0x00002000 (0% - 2048 B/s, ~ 2 s)");

        events_.logs.clear();
    }
}

TEST(DensoBeefCanProfileTest, FamilyProfilesKeepTheirLogWording)
{
    const auto expect_profile = [](const DensoBeefCanProfile& profile, std::string_view device, std::string_view family,
                                   BeefPduFormat pdu_format, std::chrono::milliseconds init_timeout,
                                   bool upper_hex_sizes)
    {
        SCOPED_TRACE(family);
        EXPECT_EQ(profile.device, device);
        EXPECT_EQ(profile.family, family);
        EXPECT_EQ(profile.pdu_format, pdu_format);
        EXPECT_EQ(profile.init_timeout, init_timeout);
        EXPECT_EQ(profile.upper_hex_sizes, upper_hex_sizes);
    };

    expect_profile(kSubaruDensoSh7058CanBeefProfile, "ECU", "petrol", BeefPduFormat::kHex, 500ms, false);
    expect_profile(kSubaruDensoSh7058CanDieselBeefProfile, "ECU", "diesel", BeefPduFormat::kDescribe, 500ms, false);
    expect_profile(kSubaruTcuDensoSh705xCanBeefProfile, "TCU", "TCU", BeefPduFormat::kHex, 2000ms, true);
}

TEST_F(BeefCanKernel, CancelledIfRequestedReportsTheCallerDetail)
{
    const MemoryRegion region{0x1000, 1};
    PhaseReporter progress(events_, "Read", 1, 1, 1);
    ReadPage();
    EXPECT_THAT(Kernel().Read(region, progress), IsOk());

    class CancelOnSecondCheck final : public ICancellationToken
    {
      public:
        bool Cancelled() const noexcept override
        {
            return checks_++ > 0;
        }

      private:
        mutable int checks_{};
    } token;
    fastecu::flash::BeefCanKernel kernel(transport_, clock_, token, events_, channel_, kPetrol);
    EXPECT_THAT(kernel.Read(region, progress), IsErrWith(ErrorKind::kCancelled, StrEq("cancelled before CAN request")));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

TEST_F(BeefCanKernel, SendChannelRequestSleepsOnlyForAPositiveDelay)
{
    // Warm BEEF exchanges have no post-send delay.
    PhaseReporter progress(events_, "Read", 1, 1, 1);
    EXPECT_CALL(clock_, Sleep).Times(0);
    ReadPage();
    EXPECT_THAT(Kernel().Read({0x1000, 1}, progress), IsOk());
    EXPECT_EQ(clock_.Elapsed(), 0ms);
    ::testing::Mock::VerifyAndClearExpectations(&clock_);

    // Download's positive pre-read delay is observed through the public
    // transfer operation. Fake time deliberately advances only five ms.
    clock_.Fake().SetSleepAdvance(5ms);
    EXPECT_CALL(clock_, Sleep(10ms, ::testing::_)).WillOnce(clock_.SleepOnFake());
    EXPECT_CALL(clock_, Now()).Times(::testing::AnyNumber());
    Expect129ByteTransfer();
    EXPECT_THAT(Transfer(Image129(), {{10ms, 2000ms, 3000ms, 10}, 2000ms}), IsOk());
    EXPECT_EQ(clock_.Elapsed(), 5ms);
}

TEST_F(BeefCanKernelWrite, ParseBeefBoundsEveryDeclaredField)
{
    for (const auto& [pdu, detail] :
         std::array{std::pair{bytes::Bytes{0xBE, 0xEF, 0x00}, "short BEEF response envelope"},
                    std::pair{bytes::Bytes{0x12, 0x34, 0x00, 0x01, 0x00}, "wrong BEEF response marker"},
                    std::pair{bytes::Bytes{0xBE, 0xEF, 0x00, 0x09, 0x00}, "invalid BEEF response length"},
                    std::pair{bytes::Bytes{0xBE, 0xEF, 0x00, 0x00, 0x00}, "invalid BEEF response length"}})
    {
        SCOPED_TRACE(::testing::PrintToString(pdu));
        ExpectInitFailure(pdu, IsErrWith(ErrorKind::kBadResponse, StrEq(detail)));
    }
    // Both declared bodies parse successfully, exposing the original B6
    // opcode to the warm exchange's opcode check. The trailing FF is padding.
    for (const auto& pdu :
         {bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0xB6}, bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0xB6, 0xAA, 0xFF}})
    {
        SCOPED_TRACE(::testing::PrintToString(pdu));
        ExpectInitFailure(
            pdu, IsErrWith(ErrorKind::kBadResponse, StrEq("unexpected BEEF response opcode 0xb6, expected 0x45")));
    }
    // Matching voltage opcodes reach payload validation: zero declared bytes
    // are short, and one declared byte cannot borrow the trailing FF padding.
    for (const auto& pdu :
         {bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x44}, bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0x44, 0xAA, 0xFF}})
    {
        SCOPED_TRACE(::testing::PrintToString(pdu));
        ScriptCrc({0x2000, 0x1000}, 0);
        ScriptInitialize();
        channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x04});
        channel_.QueueReceive(pdu);
        EXPECT_THAT(RunWrite(plans_[0]), IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response payload")));
    }
    // Observe the declared payload bound through Read: a full 1024-byte
    // physical payload is still short when its declared length is 1023.
    PhaseReporter progress(events_, "Read", 1, 1, 0x400);
    channel_.ExpectSend(bytes::Bytes{0xBE, 0xEF, 0, 7, 3, 0, 0, 0x10, 0, 4, 0});
    bytes::Bytes short_declared{0xBE, 0xEF, 4, 0, 0x43};
    short_declared.resize(0x405, 0xAA);
    channel_.QueueReceive(short_declared);
    EXPECT_THAT(Kernel().Read({0x1000, 0x400}, progress),
                IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response payload")));
    EXPECT_TRUE(channel_.ScriptConsumed());
}

} // namespace
} // namespace fastecu::flash
