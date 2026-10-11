#include "src/backend/ports/testing/result_matchers.h"
// Equivalence tests for SubaruDensoMc68hc16y5_02Executor, the portable
// replacement for flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp.
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_executor.h"

#include <algorithm>
#include <span>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/config/catalog.h"
#include "src/backend/flash/kernel/kernelmemorymodels.h"
#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_plan.h"
#include "src/backend/flash/ecu/testing/mc68_rom_images.h"
#include "src/backend/flash/eeprom/eeprom_read_plan.h"
#include "src/backend/flash/flash_device_lookup.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/mock_clock.h"
#include "src/backend/ports/testing/in_memory_file_repository.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::ElementsAre;
using ::testing::Return;

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::ComposeBeWithChecksum;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

class ToggleCancellation final : public ICancellationToken
{
  public:
    bool Cancelled() const override
    {
        return cancelled_;
    }

    void Cancel()
    {
        cancelled_ = true;
    }

  private:
    bool cancelled_ = false;
};

class FlipAfter final : public ICancellationToken
{
  public:
    explicit FlipAfter(int allowed_checks) : allowed_checks_(allowed_checks)
    {
    }

    bool Cancelled() const override
    {
        return checks_++ >= allowed_checks_;
    }

  private:
    int allowed_checks_;
    mutable int checks_ = 0;
};

class OpenScriptedKlineFlashTransport final : public ScriptedKlineFlashTransport
{
  public:
    OpenScriptedKlineFlashTransport() : ScriptedKlineFlashTransport(ScriptedTransportInitialState::kOpen)
    {
    }
};

class ShortWriteTransport final : public ScriptedKlineFlashTransport
{
  public:
    ShortWriteTransport() : ScriptedKlineFlashTransport(ScriptedTransportInitialState::kOpen)
    {
    }

    Result<std::size_t> Write(bytes::ByteView data) override
    {
        return data.empty() ? 0U : data.size() - 1;
    }
};

class DrainCancellingTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit DrainCancellingTransport(ToggleCancellation& cancellation)
        : ScriptedKlineFlashTransport(ScriptedTransportInitialState::kOpen), cancellation_(cancellation)
    {
    }

    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (timeout == 10ms)
        {
            cancellation_.Cancel();
        }
        return ScriptedKlineFlashTransport::Read(timeout, cancellation);
    }

    Result<std::size_t> Write(bytes::ByteView data) override
    {
        write_attempts.emplace_back(data.begin(), data.end());
        return ScriptedKlineFlashTransport::Write(data);
    }

    std::vector<bytes::Bytes> write_attempts;

  private:
    ToggleCancellation& cancellation_;
};

class CancelAfterEraseTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit CancelAfterEraseTransport(ToggleCancellation& cancellation)
        : ScriptedKlineFlashTransport(ScriptedTransportInitialState::kOpen), cancellation_(cancellation)
    {
    }

    Result<std::size_t> Write(bytes::ByteView data) override
    {
        if (data.size() > 4 && data[0] == 0xBE && data[1] == 0xEF)
        {
            erase_response_pending_ = data[4] == 0x25;
            if (data[4] == 0x22)
            {
                ++flash_buffer_write_attempts;
            }
        }
        return ScriptedKlineFlashTransport::Write(data);
    }

    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        Result<OptionalBytes> result = ScriptedKlineFlashTransport::Read(timeout, cancellation);
        if (erase_response_pending_)
        {
            erase_response_pending_ = false;
            cancellation_.Cancel();
        }
        return result;
    }

    std::size_t flash_buffer_write_attempts = 0;

  private:
    ToggleCancellation& cancellation_;
    bool erase_response_pending_ = false;
};

Result<FlashPlan> StockPlan(FlashOperation operation = FlashOperation::kRead)
{
    return BuildSubaruDensoMc68hc16y502Plan(
        operation, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5",
        operation == FlashOperation::kRead ? std::nullopt
                                           : std::optional(testing::PackedMc68Image(bytes::Bytes(0x28000, 0))),
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

Result<FlashPlan> EcutekPlan(FlashOperation operation = FlashOperation::kRead)
{
    return BuildSubaruDensoMc68hc16y502Plan(
        operation, "sub_ecu_denso_mc68hc16y5_02_ecutek", "MC68HC16Y5",
        operation == FlashOperation::kRead ? std::nullopt
                                           : std::optional(testing::PackedMc68Image(bytes::Bytes(0x28000, 0))),
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

Result<FlashPlan> TpuReadPlan()
{
    return BuildSubaruDensoMc68hc16y502Plan(
        FlashOperation::kRead, "sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5_TPU", std::nullopt,
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

bytes::Bytes Framed(std::uint8_t opcode, bytes::ByteView extra = {})
{
    const std::uint16_t datalen_plus_one = static_cast<std::uint16_t>(extra.size() + 1);
    return ComposeBeWithChecksum(bytes::Sum8, std::uint16_t{0xBEEF}, datalen_plus_one, bytes::Byte(opcode), extra);
}

// Anchors framed() against hardcoded wire bytes so a bug in composeBeWithChecksum
// (e.g. the wrong checksum span) cannot move both this helper and the
// production frame() it stands in for together and hide behind a passing suite.
//
// framed(0x01): [0xBE, 0xEF, len_hi, len_lo, 0x01], datalen_plus_one = 0+1 = 1.
// Checksum (sum8) over [0xBE, 0xEF, 0x00, 0x01, 0x01]:
//   0xBE + 0xEF + 0x00 + 0x01 + 0x01 = 0x1AF -> & 0xFF = 0xAF.
TEST(SubaruDensoMc68hc16y5_02Executor, FramedHelperMatchesHardcodedWireBytesNoPayload)
{
    EXPECT_THAT(Framed(0x01), ElementsAre(0xBE, 0xEF, 0x00, 0x01, 0x01, 0xAF));
}

// framed(0x02, {0xAB, 0xCD}): datalen_plus_one = 2+1 = 3.
// Checksum (sum8) over [0xBE, 0xEF, 0x00, 0x03, 0x02, 0xAB, 0xCD]:
//   0xBE + 0xEF + 0x00 + 0x03 + 0x02 + 0xAB + 0xCD = 0x32A -> & 0xFF = 0x2A.
TEST(SubaruDensoMc68hc16y5_02Executor, FramedHelperMatchesHardcodedWireBytesWithPayload)
{
    EXPECT_THAT(Framed(0x02, bytes::Bytes{0xAB, 0xCD}), ElementsAre(0xBE, 0xEF, 0x00, 0x03, 0x02, 0xAB, 0xCD, 0x2A));
}

bytes::Bytes StockUploadRequest()
{
    return {
        0x53, 0x02, 0x00, 0x00, 0x00, 0x10, 0x64, 0x67, 0x39, 0x41, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x9A,
    };
}

void ScriptStockConnectAndUpload(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("stock connect and upload");
    transport.QueueNoFrame();
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x4D, 0x00, 0xB3});
    transport.Exchange(StockUploadRequest());
    // Legacy upload_kernel treats a real read timeout / no frame as the
    // success sentinel. Preserve OptionalBytes' distinction from a
    // present, zero-length frame in this end-to-end fixture.
    transport.QueueNoFrame();
    transport.Exchange(Framed(0x01), Framed(0x41, bytes::Bytes{'K', 'I', 'D'}));
}

void ScriptReadPage(ScriptedKlineFlashTransport& transport, std::uint32_t address, bytes::Byte fill,
                    std::uint8_t response_opcode = 0x43)
{
    const auto section = transport.Section("read page");
    transport.Exchange(Framed(0x03, ComposeBe(0x00_b, U24(address), std::uint16_t{0x400})),
                       Framed(response_opcode, bytes::Bytes(0x400, fill)));
}

std::size_t PackedBlockOffset(const FlashDevice& device, unsigned block_no)
{
    std::size_t offset = 0;
    for (unsigned index = 0; index < block_no; ++index)
    {
        offset += device.fblocks[index].len;
    }
    return offset;
}

void ScriptCrcCompare(ScriptedKlineFlashTransport& transport, const FlashDevice& device, bytes::ByteView image,
                      std::optional<unsigned> differing_block)
{
    const auto section = transport.Section("crc compare");
    std::size_t image_offset = 0;
    for (unsigned block_no = 0; block_no < device.numblocks; ++block_no)
    {
        const auto& block = device.fblocks[block_no];
        const bytes::Bytes request_payload = ComposeBe(block.start, 0x00_b, U24(block.len));
        transport.Exchange(Framed(0x02, request_payload));

        std::uint32_t ecu_crc = fastecu::checksum::Crc32(image.data() + image_offset, block.len);
        if (differing_block == block_no)
        {
            ecu_crc ^= 0x00000001U;
        }
        transport.QueueRead(Framed(0x42, ComposeBe(ecu_crc)));
        transport.QueueNoFrame();
        image_offset += block.len;
    }
}

void ScriptFlashInit(ScriptedKlineFlashTransport& transport, bool test_write)
{
    const auto section = transport.Section("flash init");
    transport.Exchange(Framed(0x05), Framed(0x45, bytes::Bytes{0x00, 0x00, 0x02, 0x06}));
    transport.Exchange(Framed(0x06), Framed(0x46, bytes::Bytes{0x00, 0x00, 0x10, 0x00}));
    const std::uint8_t enable_opcode = test_write ? 0x21 : 0x20;
    transport.Exchange(Framed(enable_opcode), Framed(static_cast<std::uint8_t>(enable_opcode | 0x40U)));
}

void ScriptProgVolt(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.Section("prog volt");
    transport.Exchange(Framed(0x04), Framed(0x44, bytes::Bytes{0x04, 0xB0}));
}

void ScriptBlockTransfer(ScriptedKlineFlashTransport& transport, const FlashDevice& device, bytes::ByteView image,
                         unsigned block_no, bool test_write)
{
    const auto section = transport.Section("block transfer");
    constexpr std::uint32_t kChunkSize = 0x200;
    constexpr std::uint32_t kCommitSize = 0x1000;
    const auto& block = device.fblocks[block_no];
    const std::size_t block_image_offset = PackedBlockOffset(device, block_no);

    if (!test_write)
    {
        transport.Exchange(Framed(0x25, ComposeBe(block.start)), Framed(0x65));
    }

    for (std::uint32_t offset = 0; offset < block.len; offset += kChunkSize)
    {
        const std::uint32_t address = block.start + offset;
        // The image bytes are spliced with a raw insert rather than folded into
        // composeBe: production builds this frame as composeBe(address, subspan),
        // so folding would make both sides the same expression and a bug in the
        // splice would cancel out instead of failing the test.
        bytes::Bytes write_payload = ComposeBe(address);
        write_payload.append_range(bytes::ByteView(image).subspan(block_image_offset + offset, kChunkSize));
        transport.Exchange(Framed(0x22, write_payload), Framed(0x62));

        if ((offset + kChunkSize) % kCommitSize == 0)
        {
            const std::uint32_t commit_offset = offset + kChunkSize - kCommitSize;
            const std::uint32_t commit_address = block.start + commit_offset;
            const std::uint32_t commit_crc =
                fastecu::checksum::Crc32(image.data() + block_image_offset + commit_offset, kCommitSize);
            // 0x10, 0x00 stay two byte literals: production spells this field
            // std::uint16_t(kCommitBlockSize), so the width derivation is not
            // shared and a byte-order bug in appendU16Be would fail this test
            // rather than move both sides together.
            const bytes::Bytes commit_payload = ComposeBe(commit_address, 0x10_b, 0x00_b, commit_crc);
            const std::uint8_t commit_opcode = test_write ? 0x23 : 0x24;
            transport.Exchange(Framed(commit_opcode, commit_payload),
                               Framed(static_cast<std::uint8_t>(commit_opcode | 0x40U)));
        }
    }
    transport.QueueNoFrame();
}

Result<FlashPlan> StockWritePlanFrom(FlashOperation operation, memory::MemoryImage image);

Result<FlashPlan> StockWritePlan(FlashOperation operation, bytes::Bytes image)
{
    return StockWritePlanFrom(operation, testing::PackedMc68Image(std::move(image)));
}

Result<FlashPlan> StockWritePlanFrom(FlashOperation operation, memory::MemoryImage image)
{
    return BuildSubaruDensoMc68hc16y502Plan(
        operation, "sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5", std::move(image),
        KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01, 0x02, 0x03, 0x04}});
}

bytes::Bytes WriteChunkRequest(const FlashDevice& device, bytes::ByteView image, unsigned block_no,
                               std::uint32_t offset)
{
    constexpr std::uint32_t kChunkSize = 0x200;
    const auto& block = device.fblocks[block_no];
    // Raw insert, not a composeBe splice — see script_write_prefix for why the
    // image bytes must not share production's compose expression.
    bytes::Bytes payload = ComposeBe(block.start + offset);
    const std::size_t image_offset = PackedBlockOffset(device, block_no) + offset;
    payload.append_range(bytes::ByteView(image).subspan(image_offset, kChunkSize));
    return Framed(0x22, payload);
}

void ScriptWritePrefix(ScriptedKlineFlashTransport& transport, const FlashDevice& device, bytes::ByteView image,
                       unsigned block_no, bool test_write)
{
    const auto section = transport.Section("write prefix");
    ScriptStockConnectAndUpload(transport);
    ScriptCrcCompare(transport, device, image, block_no);
    ScriptFlashInit(transport, test_write);
    ScriptProgVolt(transport);
    if (!test_write)
    {
        transport.Exchange(Framed(0x25, ComposeBe(device.fblocks[block_no].start)), Framed(0x65));
    }
}

Result<FlashPlan> DifferentFamilyPlan()
{
    InMemoryFileRepository files;
    files.files["kernels/kernel.bin"] = {0x01, 0x02, 0x03, 0x04};
    return BuildEepromReadPlan({.kernel_files_directory = "kernels/"},
                               config::ProtocolSpec{.name = "sub_ecu_eeprom_denso_sh7055_kline",
                                                    .mcu = "SH7055",
                                                    .kernel = "kernel.bin",
                                                    .kernel_load_address = 0xFFFF6004U},
                               EepromReadMode::kMode2, files);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WrongFamilyPlanFails)
{
    auto plan = DifferentFamilyPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

TEST(SubaruDensoMc68hc16y5_02Executor, TransportSetupReturnsPlanWireConfigurationIncludingZeroIds)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    SubaruDensoMc68hc16y5_02Executor executor;

    auto setup = executor.TransportSetup(*plan);

    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->baud, 9600);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0);
    EXPECT_EQ(setup->target_id, 0);
}

TEST(SubaruDensoMc68hc16y5_02Executor, BoundAttemptPreservesConfigureToOpenCancellationCheckpoint)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    auto transport = std::make_unique<ScriptedKlineFlashTransport>();
    auto *observed_transport = transport.get();
    auto attempt =
        BindFlashAttempt(std::move(*plan), std::make_unique<SubaruDensoMc68hc16y5_02Executor>(), std::move(transport));
    FakeClock clock;
    FlipAfter cancellation(1);
    RecordingEventSink events;

    ASSERT_THAT(attempt->Run(clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(observed_transport->last_config.has_value());
    EXPECT_EQ(observed_transport->close_call_count, 0);
    EXPECT_TRUE(observed_transport->control_line_trace.empty());
}

TEST(SubaruDensoMc68hc16y5_02Executor, MalformedFamilyPlanFailsBeforeAnyIo)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    auto plan = ValidateAndBuild(FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kSubaruDensoMc68hc16y502,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_denso_mc68hc16y5_02",
        .mcu_name = "MC68HC16Y5",
        .transfer_region = {device->fblocks[0].start, device->romsize},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0x20000, .bytes = {0x01}},
        .family_plan =
            SubaruDensoMc68hc16y5_02Plan{
                .connect_baud = 12345,
                .kernel_baud = 9600,
                .encryption_xor = 0x55,
                .kernel_magic = 0x3941,
                .bootloader_ok = {0x4d, 0x00, 0xb3},
            },
    });
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_FALSE(transport.last_config.has_value());
    EXPECT_TRUE(transport.read_timeouts.empty());
    EXPECT_TRUE(transport.control_line_trace.empty());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectsViaWrx02InitAndUploadsPaddedKernel)
{
    auto plan = StockPlan(FlashOperation::kWrite);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    transport.QueueNoFrame();
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x4D, 0x00, 0xB3});

    // The four input bytes are padded to 16 before encryption; magic patches
    // encrypted offsets 2..3, and each trailing zero encrypts to 0x65.
    const bytes::Bytes upload{
        0x53, 0x02, 0x00, 0x00, 0x00, 0x10, 0x64, 0x67, 0x39, 0x41, 0x65, 0x65,
        0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x65, 0x9A,
    };
    transport.Exchange(upload);
    transport.QueueNoFrame();

    transport.Exchange(Framed(0x01), Framed(0x41, bytes::Bytes{'K', 'I', 'D'}));
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    ScriptCrcCompare(transport, *device, plan->ImageOrEmpty(), std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.control_line_trace,
              (std::vector<ScriptedKlineFlashTransport::ControlLineAction>{
                  ScriptedKlineFlashTransport::ControlLineAction::kDisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::kPulseLec2,
                  ScriptedKlineFlashTransport::ControlLineAction::kEnableProgrammingVoltageLine,
              }));
    EXPECT_EQ(transport.operation_trace.front(), ScriptedKlineFlashTransport::Operation::kDisableLecLines);
    EXPECT_EQ(transport.operation_trace.at(1), ScriptedKlineFlashTransport::Operation::kRead10);
    EXPECT_EQ(transport.lec_2_pulse_timeouts, (std::vector<std::chrono::milliseconds>{200ms}));
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 200ms), 12);
    // 200 + 200 + 50 + 1500 + 200 ms.
    EXPECT_EQ(clock.Elapsed(), 2150ms);
}

TEST(SubaruDensoMc68hc16y5_02Executor, PresentEmptyUploadFrameIsNotNoFrameSuccess)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    transport.QueueNoFrame();
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x4D, 0x00, 0xB3});
    transport.Exchange(StockUploadRequest(), bytes::Bytes{});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectFallsBackToKernelAlivePoll)
{
    auto plan = StockPlan(FlashOperation::kWrite);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    transport.QueueRead(bytes::Bytes{0xDE, 0xAD}); // stale bytes are discarded
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x00, 0x00, 0x00});
    transport.Exchange(Framed(0x01), Framed(0x41, bytes::Bytes{'K'}));
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    ScriptCrcCompare(transport, *device, plan->ImageOrEmpty(), std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.control_line_trace,
              (std::vector<ScriptedKlineFlashTransport::ControlLineAction>{
                  ScriptedKlineFlashTransport::ControlLineAction::kDisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::kPulseLec2,
                  ScriptedKlineFlashTransport::ControlLineAction::kDisableLecLines,
                  ScriptedKlineFlashTransport::ControlLineAction::kEnableProgrammingVoltageLine,
              }));
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 200ms), 11);
    // 200 + 200 + 50 + 100 + 200 ms.
    EXPECT_EQ(clock.Elapsed(), 750ms);
}

TEST(SubaruDensoMc68hc16y5_02Executor, NoFrameBootInitFallsBackToKernelAlivePoll)
{
    auto plan = StockPlan(FlashOperation::kWrite);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    transport.QueueNoFrame(); // legacy operation.cpp:69-70 initial drain
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4});
    // Legacy connect_bootloader treats an empty read as a bad/missing init
    // response and falls through to the 62500-baud kernel-ID probe.
    transport.QueueNoFrame();
    transport.Exchange(Framed(0x01), Framed(0x41, bytes::Bytes{'K'}));
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    ScriptCrcCompare(transport, *device, plan->ImageOrEmpty(), std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{62500}));
}

TEST(SubaruDensoMc68hc16y5_02Executor, EcutekUsesItsDistinctBootloaderAndKernelWireValues)
{
    auto plan = EcutekPlan(FlashOperation::kWrite);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    transport.QueueNoFrame(); // legacy operation.cpp:69-70 initial drain
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x4C, 0x00, 0xB4});
    transport.Exchange(bytes::Bytes{
        0x53, 0x02, 0x00, 0x00, 0x00, 0x10, 0x60, 0x63, 0x39, 0x40, 0x61, 0x61,
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0xD3,
    });
    transport.QueueNoFrame();
    transport.Exchange(Framed(0x01), Framed(0x41, bytes::Bytes{'K'}));
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    ScriptCrcCompare(transport, *device, plan->ImageOrEmpty(), std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{11700, 62500}));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ConnectFailsWithNoValidResponseAtAll)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    transport.QueueNoFrame(); // legacy operation.cpp:69-70 initial drain
    transport.Exchange(bytes::Bytes{0x4D, 0xFF, 0xB4}, bytes::Bytes{0x00, 0x00, 0x00});
    transport.Exchange(Framed(0x01));
    transport.QueueNoFrame();

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(SubaruDensoMc68hc16y5_02Executor, CancellationBeforeConnectStopsImmediately)
{
    class AlreadyCancelled : public ICancellationToken
    {
      public:
        bool Cancelled() const override
        {
            return true;
        }
    } cancellation;
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

TEST(SubaruDensoMc68hc16y5_02Executor, ShortKlineWriteFailsBeforeRead)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ShortWriteTransport transport;
    transport.QueueNoFrame(); // legacy operation.cpp:69-70 initial drain
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
}

TEST(SubaruDensoMc68hc16y5_02Executor, InitialDrainTransportErrorStopsBeforeBootloaderTraffic)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    transport.QueueError(ErrorKind::kDisconnected, "drain failed");
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

TEST(SubaruDensoMc68hc16y5_02Executor, CancellationAtInitialDrainStopsBeforeBootloaderWrite)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ToggleCancellation cancellation;
    DrainCancellingTransport transport(cancellation);
    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.write_attempts.empty());
    EXPECT_EQ(transport.read_timeouts, (std::vector<std::chrono::milliseconds>{10ms}));
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadReturnsAssembledPageBytes)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);

    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes expected;
    std::size_t logical_page = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        for (std::uint32_t offset = 0; offset < block.len; offset += 0x400)
        {
            const auto fill = static_cast<bytes::Byte>(logical_page++);
            ScriptReadPage(transport, block.start + offset, fill);
            expected.insert(expected.end(), 0x400, fill);
        }
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(*result->read_bytes, expected);
    ASSERT_EQ(result->read_bytes->size(), 0x28000U);
    // Packed output joins the final byte before the physical RAM hole to the
    // first byte read at wire address 0x28000; the 0x8000-byte hole is absent.
    EXPECT_EQ(result->read_bytes->at(0x1FFFF), 0x7FU);
    EXPECT_EQ(result->read_bytes->at(0x20000), 0x80U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, TpuReadHonorsDeclaredPackedRomSize)
{
    auto plan = TpuReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5_TPU");
    ASSERT_NE(device, nullptr);
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    for (std::uint32_t offset = 0; offset < device->romsize; offset += 0x400)
    {
        ScriptReadPage(transport, device->fblocks[0].start + offset, 0x6a);
    }
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;

    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), device->romsize);
    EXPECT_TRUE(std::all_of(result->read_bytes->begin(), result->read_bytes->end(),
                            [](bytes::Byte value) { return value == 0x6a; }));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadRejectsMalformedPageResponse)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    ScriptReadPage(transport, 0x00000000, 0xA5, 0x44);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadRejectsTruncatedValidMarkerResponse)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    transport.Exchange(Framed(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}),
                       bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x43});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadRejectsShortPageResponse)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    // Valid BEEF/0x43 envelope but one byte less than the requested page.
    transport.Exchange(Framed(0x03, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x04, 0x00}),
                       Framed(0x43, bytes::Bytes(0x3FF, 0xA5)));

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, ReadCancelsBetweenPages)
{
    auto plan = StockPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    ScriptReadPage(transport, 0x00000000, 0xA5);

    ToggleCancellation cancellation;
    MockClock clock;
    // Cancel only after the first page's 1 ms pacing sleep has completed.
    EXPECT_CALL(clock, Sleep(1ms, _))
        .WillOnce(DoAll(clock.SleepOnFake(), [&] { cancellation.Cancel(); }, Return(Status{})));
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 4U);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteSkipsWhenNoBlockDiffers)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 3U + device->numblocks);
    EXPECT_EQ(transport.programming_voltage_line_write_index, 3U + device->numblocks);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteReflashesOnlyDifferingBlocks)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    const std::size_t image_offset = PackedBlockOffset(*device, kDifferingBlock);
    for (std::size_t offset = 0; offset < device->fblocks[kDifferingBlock].len; ++offset)
    {
        image[image_offset + offset] = static_cast<bytes::Byte>(offset * 17U + 3U);
    }
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    ScriptBlockTransfer(transport, *device, image, kDifferingBlock, false);
    ScriptCrcCompare(transport, *device, image, std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.control_line_trace.back(),
              ScriptedKlineFlashTransport::ControlLineAction::kEnableProgrammingVoltageLine);
    EXPECT_EQ(clock.Elapsed(), 4050ms);
}

// The executor writes by ECU address through the ROM file's memory map, so a
// 192 KiB file and its packed 160 KiB twin send exactly the same bytes.
TEST(SubaruDensoMc68hc16y5_02Executor, AFullRomFileWritesTheSameBytesAsItsPackedTwin)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    // The last block lies in the second flash range, past the RAM hole, where
    // the two files' layouts differ.
    const unsigned differing_block = device->numblocks - 1;
    ASSERT_GE(device->fblocks[differing_block].start, 0x28000U);
    bytes::Bytes packed(device->romsize, 0x00);
    const std::size_t image_offset = PackedBlockOffset(*device, differing_block);
    for (std::size_t offset = 0; offset < device->fblocks[differing_block].len; ++offset)
    {
        packed[image_offset + offset] = static_cast<bytes::Byte>(offset * 13U + 7U);
    }
    auto plan = StockWritePlanFrom(FlashOperation::kWrite, testing::FullMc68Image(packed));
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    // Scripted from the packed file, exactly as the 160 KiB write expects.
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, packed, differing_block);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    ScriptBlockTransfer(transport, *device, packed, differing_block, false);
    ScriptCrcCompare(transport, *device, packed, std::nullopt);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, TestWriteSendsValidateNotCommit)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    const std::size_t image_offset = PackedBlockOffset(*device, kDifferingBlock);
    std::ranges::fill(std::span(image).subspan(image_offset, device->fblocks[kDifferingBlock].len), bytes::Byte{0xA5});
    auto plan = StockWritePlan(FlashOperation::kTestWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);
    ScriptFlashInit(transport, true);
    ScriptProgVolt(transport);
    ScriptBlockTransfer(transport, *device, image, kDifferingBlock, true);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kTestWrite);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteFailsOnRejectedEraseResponse)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    transport.Exchange(Framed(0x25, ComposeBe(device->fblocks[kDifferingBlock].start)), Framed(0x64));

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteCancelsMidBlockTransfer)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kDifferingBlock = 8;
    ASSERT_LT(kDifferingBlock, device->numblocks);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ToggleCancellation cancellation;
    CancelAfterEraseTransport transport(cancellation);
    ScriptStockConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kDifferingBlock);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    transport.Exchange(Framed(0x25, ComposeBe(device->fblocks[kDifferingBlock].start)), Framed(0x65));

    FakeClock clock;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.flash_buffer_write_attempts, 0U);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteAcceptsFragmentedBlockCrcAndDrainsIt)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    std::size_t image_offset = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        // The trailing four bytes stay byte literals: production spells the
        // same field 0x00_b followed by u24(block.length), so this expectation
        // keeps its own derivation of the length encoding.
        transport.Exchange(Framed(0x02, ComposeBe(block.start, 0x00_b, 0x00_b, 0x40_b, 0x00_b)));
        const std::uint32_t crc = fastecu::checksum::Crc32(image.data() + image_offset, block.len);
        bytes::Bytes response = Framed(0x42, ComposeBe(crc));
        if (block_no == 0)
        {
            transport.QueueRead(bytes::ByteView(response).first(6));
            transport.QueueRead(bytes::ByteView(response).subspan(6));
        }
        else
        {
            transport.QueueRead(response);
        }
        transport.QueueNoFrame();
        image_offset += block.len;
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 50ms), 1);
    EXPECT_EQ(clock.Elapsed(), 2250ms);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteAcceptsBlockCrcAfterEmptyInitialRead)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    std::size_t image_offset = 0;
    for (unsigned block_no = 0; block_no < device->numblocks; ++block_no)
    {
        const auto& block = device->fblocks[block_no];
        transport.Exchange(Framed(0x02, ComposeBe(block.start, 0x00_b, 0x00_b, 0x40_b, 0x00_b)));
        const std::uint32_t crc = fastecu::checksum::Crc32(image.data() + image_offset, block.len);
        if (block_no == 0)
        {
            transport.QueueNoFrame();
            transport.QueueRead(Framed(0x42, ComposeBe(crc)));
        }
        else
        {
            transport.QueueRead(Framed(0x42, ComposeBe(crc)));
        }
        transport.QueueNoFrame();
        image_offset += block.len;
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 50ms), 1);
    EXPECT_EQ(clock.Elapsed(), 2250ms);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteRejectsTruncatedBlockCrcAfterBoundedReads)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    transport.Exchange(Framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00}),
                       bytes::Bytes{0xBE, 0xEF, 0x00, 0x05, 0x42});
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transport.QueueNoFrame();
    }

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(std::count(transport.read_timeouts.begin(), transport.read_timeouts.end(), 50ms), 20);
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteRejectsNegativeBlockCrcResponse)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    transport.Exchange(Framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00}),
                       Framed(0x7F, bytes::Bytes{0x00, 0x00, 0x00, 0x00}));

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WritePropagatesBlockCrcDrainError)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    const std::uint32_t crc = fastecu::checksum::Crc32(image.data(), 0x4000);
    transport.Exchange(Framed(0x02, bytes::Bytes{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00}),
                       Framed(0x42, ComposeBe(crc)));
    transport.QueueError(ErrorKind::kDisconnected, "CRC drain failed");

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, "CRC drain failed"));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteFailsOnRejectedFlashBufferResponse)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptWritePrefix(transport, *device, image, kBlock, false);
    transport.Exchange(WriteChunkRequest(*device, image, kBlock, 0), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x62});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteFailsOnRejectedCommitResponse)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptWritePrefix(transport, *device, image, kBlock, false);
    for (std::uint32_t offset = 0; offset < 0x1000; offset += 0x200)
    {
        transport.Exchange(WriteChunkRequest(*device, image, kBlock, offset), Framed(0x62));
    }
    const std::uint32_t start = device->fblocks[kBlock].start;
    const std::uint32_t crc = fastecu::checksum::Crc32(image.data() + PackedBlockOffset(*device, kBlock), 0x1000);
    transport.Exchange(Framed(0x24, ComposeBe(start, 0x10_b, 0x00_b, crc)), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x64});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, TestWriteFailsOnRejectedValidateResponse)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kTestWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptWritePrefix(transport, *device, image, kBlock, true);
    for (std::uint32_t offset = 0; offset < 0x1000; offset += 0x200)
    {
        transport.Exchange(WriteChunkRequest(*device, image, kBlock, offset), Framed(0x62));
    }
    const std::uint32_t start = device->fblocks[kBlock].start;
    const std::uint32_t crc = fastecu::checksum::Crc32(image.data() + PackedBlockOffset(*device, kBlock), 0x1000);
    transport.Exchange(Framed(0x23, ComposeBe(start, 0x10_b, 0x00_b, crc)), bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0x63});

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruDensoMc68hc16y5_02Executor, WriteLogsRemainingMismatchAfterVerification)
{
    const FlashDevice *device = FindFlashDevice("MC68HC16Y5");
    ASSERT_NE(device, nullptr);
    constexpr unsigned kBlock = 8;
    bytes::Bytes image(device->romsize, 0x00);
    auto plan = StockWritePlan(FlashOperation::kWrite, image);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    OpenScriptedKlineFlashTransport transport;
    ScriptStockConnectAndUpload(transport);
    ScriptCrcCompare(transport, *device, image, kBlock);
    ScriptFlashInit(transport, false);
    ScriptProgVolt(transport);
    ScriptBlockTransfer(transport, *device, image, kBlock, false);
    ScriptCrcCompare(transport, *device, image, kBlock);

    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    SubaruDensoMc68hc16y5_02Executor executor;
    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_TRUE(
        std::find(
            events.logs.begin(), events.logs.end(),
            std::pair{LogLevel::kError,
                      std::string{"Flash verification differs; do not power off, the kernel is still running"}}) !=
        events.logs.end());
}

} // namespace
} // namespace fastecu::flash
