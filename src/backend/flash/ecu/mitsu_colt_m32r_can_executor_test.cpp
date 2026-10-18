#include "src/backend/ports/testing/result_matchers.h"
// Equivalence tests for MitsuColtM32rCanExecutor, the portable replacement for
// FlashEcuMitsuM32rCanOperation's connect_bootloader(), readFlashRange(),
// read_mem(), upload_and_commit(), ensureTopRegionWritten() and write_mem().
// Every expected request below is built with the same
// MitsuColtCan/MitsuColtCanVendorExt builders the legacy class calls (through
// its qt_colt.h *Frame shims). Expected log strings are copied
// character-for-character from the legacy source wherever it had a
// counterpart, with two kinds of exception: the messages the UDS layer
// replaced (a decoded NRC or envelope complaint where the legacy decoder could
// only say "Not a valid answer"), and fatal_request's rejection messages,
// which use a generic "{operation} rejected: " prefix instead.
//
// This family does NOT instantiate CanExecutorConformance
// (can_executor_conformance.h), unlike its CAN siblings. execute() below
// unconditionally runs the full connect_bootloader() handshake before it
// ever inspects plan.operation() (see the "test_write is not supported"
// guard well after the connect call) -- validate_mitsu_colt_m32r_can_plan()
// has no TestWrite-specific rejection of its own, unlike the shared
// validate_single_window_plan() every already-folded sibling's plan module
// uses, which rejects TestWrite before any connect is attempted. So a
// hand-built TestWrite plan for this family reaches the ECU handshake before
// being refused, which the shared RefusesATestWritePlanRatherThanWritingForReal
// test cannot observe -- its fixed body requires zero transport I/O and an
// untouched last_config_. TYPED_TEST_SUITE_P's registration is all-or-nothing
// per instantiation, so no subset of the suite can be composed for this
// family alone without touching the already-folded siblings' own files. See
// RefusesATestWritePlanRatherThanWritingForReal below, which pins this
// family's actual (connect-first) behavior instead.
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/ecu/testing/mitsu_colt_m32r_can_scripts.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::RecordingEventSink;
using fastecu::flash::BuildMitsuColtM32rCanPlan;
using fastecu::flash::FlashOperation;
using fastecu::flash::MitsuColtM32rCanExecutor;
using testing::Contains;
using testing::Each;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Not;
using testing::Pair;
using namespace fastecu::flash::colt_testing;

class ScriptedCanFlashTransport : public fastecu::flash::ScriptedCanFlashTransport
{
  public:
    ScriptedCanFlashTransport()
        : fastecu::flash::ScriptedCanFlashTransport(fastecu::flash::ScriptedTransportInitialState::kOpen)
    {
    }
};

// Hand-built rather than produced by build_mitsu_colt_m32r_can_plan: the
// default 512 KiB plan declares both confirmations, and validate_and_build
// does not require them, so this is the only way to reach the executor with a
// Write plan whose high-risk step was never granted.
fastecu::Result<fastecu::flash::FlashPlan>
WritePlanGranting(std::initializer_list<fastecu::flash::ConfirmationSpec::Id> granted, bytes::Bytes rom = WriteRom(),
                  FlashOperation operation = FlashOperation::kWrite,
                  std::uint32_t rom_size = mitsu_colt_can::kFullRomSize)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::kMitsuColtM32rCan;
    fields.transport = fastecu::flash::TransportKind::kCanIso15765;
    fields.target_id = std::string(rom_size == 0x60000 ? kProtocol384 : kProtocol512);
    fields.mcu_name = std::string(rom_size == 0x60000 ? kMcu384 : kMcu512);
    fields.transfer_region =
        fastecu::flash::MemoryRegion{mitsu_colt_can::kUserspaceStart, rom_size - mitsu_colt_can::kUserspaceStart};
    fields.image = std::move(rom);
    fields.family_plan = fastecu::flash::MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = false,
        .session_id = mitsu_colt_can::kSessionBootload,
    };
    for (const fastecu::flash::ConfirmationSpec::Id id : granted)
    {
        fields.confirmations.push_back(fastecu::flash::ConfirmationSpec{id, {}});
    }
    return fastecu::flash::ValidateAndBuild(std::move(fields));
}

fastecu::Result<fastecu::flash::FlashPlan> HandBuiltWritePlan(std::string_view target, std::string_view mcu,
                                                              bool vendor, fastecu::flash::MemoryRegion region,
                                                              std::size_t image_size)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = FlashOperation::kWrite;
    fields.family = fastecu::flash::FlashFamily::kMitsuColtM32rCan;
    fields.transport = fastecu::flash::TransportKind::kCanIso15765;
    fields.target_id = std::string(target);
    fields.mcu_name = std::string(mcu);
    fields.transfer_region = region;
    fields.image = bytes::Bytes(image_size, 0x00);
    fields.family_plan = fastecu::flash::MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = vendor,
        .session_id = mitsu_colt_can::kSessionBootload,
    };
    return fastecu::flash::ValidateAndBuild(std::move(fields));
}

// Scripts the full sweep the executor must perform over the plan's transfer
// region.
void ScriptFullRead(ScriptedCanFlashTransport& transport, const fastecu::flash::FlashPlan& plan, bytes::Byte fill)
{
    const auto section = transport.Section("full read");
    ScriptFlashRead(transport, plan.TransferRegion().start, plan.TransferRegion().length, fill);
}

// Scripts a write through the handshake, the top-region check and both
// routine uploads, up to the reflash unlock.
void ScriptWriteThroughRoutineUploads(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("write through routine uploads");
    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
}

void ScriptWriteThroughEraseTrigger(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("write through erase trigger");
    ScriptWriteThroughRoutineUploads(transport);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()), Response({0x7b}));
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineErase()));
}

void ScriptWriteThroughEraseRoutineCrcCheck(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("write through erase routine crc check");
    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadFrames(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(mitsu_colt_can::kCrcTransferAddress,
                                                                    mitsu_colt_can::kCrcTransferSize)),
                       Response({0x74}));
    const std::uint16_t crc = mitsu_colt_can::Checksum(mitsu_colt_can::kErasePageRoutine);
    const bytes::Bytes crc_data = bytes::ComposeBe(crc);
    transport.Exchange(Request(mitsu_colt_can::BuildTransferDataFrames(crc_data).front()), Response({0x76}));
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineCheckCrc(mitsu_colt_can::kEraseRoutineRamAddr)));
}

TEST(MitsuColtM32rCanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    // A plan built for another family must be rejected by check_family before
    // configure()/open() or any write --
    // the scripted transport is left completely untouched, which is the
    // assertion that matters here.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    // Hand-built rather than produced by a builder: the point is a plan this
    // executor must refuse, and only validate_and_build can make a FlashPlan.
    fastecu::flash::FlashPlanFields fields;
    fields.operation = FlashOperation::kRead;
    fields.family = fastecu::flash::FlashFamily::kDensoSh705xEepromCan;
    fields.transport = fastecu::flash::TransportKind::kCanIso15765;
    fields.target_id = "sub_ecu_denso_sh705x_eeprom_can";
    fields.mcu_name = "SH7058";
    fields.transfer_region = fastecu::flash::MemoryRegion{.start = 0x0, .length = 0x100};
    fields.kernel = fastecu::flash::KernelImage{.id = "k", .load_address = 0xffff6004, .bytes = {0x01, 0x02}};
    fields.family_plan = fastecu::flash::DensoSh705xEepromCanPlan{
        .mode = fastecu::flash::EepromReadMode::kMode2,
        .security = fastecu::flash::DensoSecurityVariant::kStock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };
    auto foreign = fastecu::flash::ValidateAndBuild(std::move(fields));
    ASSERT_THAT(foreign, fastecu::testing::IsOk());

    const auto result = executor.Execute(*foreign, transport, clock, cancellation, events);

    ASSERT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("does not match this executor")));
    EXPECT_THAT(events.logs, IsEmpty());
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_FALSE(transport.last_config.has_value());
}

TEST(MitsuColtM32rCanExecutor, TransportSetupReturnsThePlansWireParameters)
{
    MitsuColtM32rCanExecutor executor;
    const auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto setup = executor.TransportSetup(*plan);

    ASSERT_THAT(setup, fastecu::testing::IsOk());
    EXPECT_EQ(setup->bitrate, 500000);
    EXPECT_EQ(setup->request_id, 0x7e0U);
    EXPECT_EQ(setup->response_id, 0x7e8U);
    EXPECT_FALSE(setup->extended_id);
}

TEST(MitsuColtM32rCanExecutor, RejectsInconsistentHandBuiltPlansBeforeAnyIo)
{
    struct Case
    {
        std::string_view name;
        std::string_view target;
        std::string_view mcu;
        bool vendor;
        fastecu::flash::MemoryRegion region;
        std::size_t image_size;
    };
    static constexpr auto kCases = std::to_array<Case>({
        {"target", "mitsu_ecu_m32r_can_typo", kMcu384, false, {0x8000, 0x58000}, 0x60000},
        {"mcu", kProtocol384, kMcu512, false, {0x8000, 0x58000}, 0x60000},
        {"vendor", kProtocol384, kMcu384, true, {0x8000, 0x58000}, 0x60000},
        {"region", kProtocol384, kMcu384, false, {0x8000, 0x78000}, 0x60000},
        {"image", kProtocol384, kMcu384, false, {0x8000, 0x58000}, 0x80000},
    });

    for (const Case& test : kCases)
    {
        ScriptedCanFlashTransport transport;
        FakeClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        MitsuColtM32rCanExecutor executor;
        const auto plan = HandBuiltWritePlan(test.target, test.mcu, test.vendor, test.region, test.image_size);
        ASSERT_THAT(plan, fastecu::testing::IsOk());

        ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig))
            << test.name;
        EXPECT_FALSE(transport.last_config.has_value()) << test.name;
        EXPECT_EQ(transport.WritesConsumed(), 0U) << test.name;
    }
}

TEST(MitsuColtM32rCanExecutor, VendorChallengeRunsInBasicSessionBeforeBootloadSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan(kVendorProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)),
                       Response({0x50, 0x81}));

    transport.Exchange(Request(mitsu_colt_can_vendor_ext::BuildChallengeSeedRequest()),
                       Response({0x63, 0x27, 0x41, 0x12, 0x34, 0x56, 0x78}));

    const std::uint32_t key = mitsu_colt_can_vendor_ext::ChallengeInverseTransform(0x12345678);
    transport.Exchange(Request(mitsu_colt_can_vendor_ext::BuildChallengeKey(key)), Response({0x63, 0x27, 0x34}));

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)));
    transport.QueueNoFrame(); // stop here: ordering is what this pins

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Vendor challenge accepted")));
    // bytes::toHex is lowercase "%02x " per byte, trailing space included.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Received vendor seed: 12 34 56 78 ")));
}

TEST(MitsuColtM32rCanExecutor, VendorChallengeRejectionStopsBeforeTheSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan(kVendorProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)),
                       Response({0x50, 0x81}));

    transport.Exchange(Request(mitsu_colt_can_vendor_ext::BuildChallengeSeedRequest()), Response({0x7f, 0x23, 0x33}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    // NRC 0x33 decoded from the untruncated context.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Wrong vendor challenge response from ECU: "
                                                             "Security access denied")));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(MitsuColtM32rCanExecutor, ReadEmitsMonotonicProgress)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);

    ScriptFullRead(transport, *plan, 0x00);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());

    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_THAT(events.phase_progress_calls, Each(testing::Field(&fastecu::RecordedPhaseProgress::phase_count, 2)));
    std::vector<std::string> entered;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.done == 0)
        {
            entered.push_back(event.phase_name);
        }
    }
    EXPECT_THAT(entered, testing::ElementsAre("Connect to ECU", "Read ROM"));

    auto read_events = events.phase_progress_calls |
                       std::views::filter([](const auto& event) { return event.phase_name == "Read ROM"; });
    int previous = -1;
    for (const auto& event : read_events)
    {
        EXPECT_GE(event.done, previous);
        EXPECT_EQ(event.total, static_cast<int>(plan->TransferRegion().length));
        previous = event.done;
    }
    EXPECT_EQ(previous, static_cast<int>(plan->TransferRegion().length));
}

TEST(MitsuColtM32rCanExecutor, WriteBoundsTheDefaultProtocolTo384KiB)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom384();
    auto plan = WritePlan(rom, kProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptWrite384KiB(transport, rom);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Checking top 128KB (0x60000-0x80000)..."))));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::kInfo, "Uploading erase redirect routine to RAM 0x805568..."))));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::kInfo, "Uploading write redirect routine to RAM 0x8054ac..."))));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Writing ROM userspace 0x8000-0x60000...")));
    std::vector<std::string> entered;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.done == 0)
        {
            entered.push_back(event.phase_name);
        }
    }
    EXPECT_THAT(entered, testing::ElementsAre("Connect", "Prepare userspace", "Erase userspace", "Write userspace",
                                              "Verify userspace"));
}

TEST(MitsuColtM32rCanExecutor, WriteSkipsBootstrapWhenTheTopRegionAlreadyMatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    // Top region in the ROM image is all 0xEE, and the ECU reports 0xEE too.
    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptWriteThroughRoutineUploads(transport);
    ScriptUnlockAndErase(transport);
    // The checksum the commit must carry is stated outright, not recomputed:
    // the scripted frame is [0x36, 0x12, 0x34], so both the running sum and
    // its big-endian split are pinned independently of the implementation.
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom), kUserspaceChecksumBytes);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    // Lifecycle is owned by BoundAttempt::run(), not this executor body.
    EXPECT_EQ(transport.close_call_count, 0);
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Top 128KB already matches, no bootstrap needed")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Erase page uploaded")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Write page uploaded")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Userspace flash erased")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Writing ROM userspace 0x8000-0x60000...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Userspace flash written")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Userspace flash verified")));
    EXPECT_THAT(events.phase_progress_calls, Each(testing::Field(&fastecu::RecordedPhaseProgress::phase_count, 6)));
    // Nothing from the bootstrap arm ran.
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Top 128KB written via redirect"))));
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_FALSE(result->read_bytes.has_value());
    std::vector<std::string> entered;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.done == 0)
        {
            entered.push_back(event.phase_name);
        }
    }
    EXPECT_THAT(entered, testing::ElementsAre("Connect", "Ensure top region", "Prepare userspace", "Erase userspace",
                                              "Write userspace", "Verify userspace"));
    EXPECT_THAT(events.phase_progress_calls, Each(testing::Field(&fastecu::RecordedPhaseProgress::phase_count, 6)));
}

// A 512 KB write against a fresh scripted transport; `plan` is checked in SetUp.
struct MitsuColtM32rCanWriteExecutor : ::testing::Test
{
    void SetUp() override
    {
        ASSERT_THAT(plan, fastecu::testing::IsOk());
    }

    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    const bytes::Bytes rom = WriteRom();
    fastecu::Result<fastecu::flash::FlashPlan> plan = WritePlan(rom);
};

TEST_F(MitsuColtM32rCanWriteExecutor, WriteRefusesRedirectBootstrapWhenTheCarrierDoesNotMatchTheDesiredTopPayload)
{
    ScriptBootloadHandshake(transport);
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashRead(transport, mitsu_colt_can::kUserspaceStart, 3 * mitsu_colt_can::kFlashReadBlockSize, 0xEE);
    ScriptFlashReadChunk(transport, mitsu_colt_can::kUserspaceStart + 3 * mitsu_colt_can::kFlashReadBlockSize,
                         mitsu_colt_can::kFlashReadBlockSize, 0xFF);

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                                    HasSubstr("carrier window does not match desired top payload")));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "Carrier window 0x8000-0x27fff does not match desired top payload; "
                                                "refusing redirect bootstrap")));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::kInfo, "Uploading erase redirect routine to RAM 0x805568..."))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Carrier window erased"))));
}

TEST_F(MitsuColtM32rCanWriteExecutor, WritePropagatesACarrierReadFailureBeforeRedirectHelpers)
{
    ScriptBootloadHandshake(transport);
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(
        mitsu_colt_can::kUserspaceStart, static_cast<bytes::Byte>(mitsu_colt_can::kFlashReadBlockSize))));
    transport.QueueError(ErrorKind::kDisconnected, "carrier read disconnected");

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_EQ(result.error(), (fastecu::Error{ErrorKind::kDisconnected, "carrier read disconnected"}));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "the flash read at 0x8000 rejected: carrier read disconnected")));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::kInfo, "Uploading erase redirect routine to RAM 0x805568..."))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Carrier window erased"))));
}

TEST(MitsuColtM32rCanExecutor, WriteStopsReadingTheTopRegionAtTheFirstMismatchedChunk)
{
    // Unlike the bootstrap cases below (whose ECU mismatches from byte 0),
    // this proves top_region_matches() keeps
    // comparing chunk by chunk -- several chunks in a row can match before
    // the divergence is found, and reading still stops there rather than
    // continuing to the end of the 128KB region.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    // First 3 chunks match the ROM's 0xEE top-region fill; the 4th diverges.
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, 3 * mitsu_colt_can::kFlashReadBlockSize, 0xEE);
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart + 3 * mitsu_colt_can::kFlashReadBlockSize,
                         mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    // A further top-region read past the 4th chunk here would fail the
    // transport's own unexpected-write assertion -- that is what proves the
    // early exit. The next scripted read is the carrier precondition check at
    // kUserspaceStart, a different address.
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWriteRedirectRoutine);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kInfo, "Top 128KB mismatch, bootstrapping via redirect routines...")));
}

TEST_F(MitsuColtM32rCanWriteExecutor, WriteFailsWhenTheUserspaceVerifyMismatches)
{
    ScriptWriteThroughRoutineUploads(transport);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));
    ScriptFlashRead(transport, mitsu_colt_can::kUserspaceStart,
                    mitsu_colt_can::kUserspaceEnd - mitsu_colt_can::kUserspaceStart, 0xFF);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Userspace verify failed after write")));
}

TEST_F(MitsuColtM32rCanWriteExecutor, WriteFailsWhenTheTopRegionVerifyMismatches)
{
    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch, so
    // top_region_matches() stops there and only one chunk is scripted for
    // this initial check (the second scriptFlashRead below, after the write,
    // is the full post-write verify read and is unaffected).
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWriteRedirectRoutine);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    // Verify read-back still reports 0xFF: the write did not take.
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xFF);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Top 128KB verify failed after redirect write")));
    // The main write never starts.
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Erase page uploaded"))));
}

TEST(MitsuColtM32rCanExecutor, WriteStopsWhenTheCarrierEraseTriggerReportsANonZeroStatus)
{
    // Same fatal_query wiring as the MitsuColtM32rCanExecutorRoutineStatusTest
    // cases below, exercised through the top-128KB bootstrap's copy of
    // unlock_and_erase() -- the `stage` suffix must appear in the log too.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch. flash_range_matches()
    // stops there instead of reading the full 128KB before comparing.
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWriteRedirectRoutine);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()), Response({0x7b}));
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineErase()), Response({0x71, 0xe0, 0x01}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "Erase trigger (top 128KB bootstrap) rejected: unexpected erase "
                                                "trigger (top 128KB bootstrap) response")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Carrier window erased"))));
    // The main write never starts.
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Erase page uploaded"))));
}

// The erase trigger (RoutineControl 224) and the CRC check (RoutineControl
// 225) both reply [routine-id echo][status] past the SID UdsClient already
// validated: a matching SID echo alone does not mean the routine itself
// succeeded, so both exchanges go through fatal_query with expected_prefix
// = {routine, 0x00} (colt_commented.S ~0x59c8-0x5a38 for the erase trigger,
// ~0x5aa0-0x5ad4 for the CRC check -- the latter is where cobd_data[2]
// carries whether can_flasher_current_block_calculated_crc matched the
// reference). fatal_query's own mismatch handling -- too-short payload,
// wrong prefix content -- is already exercised generically by
// FatalQueryTest in uds_client_exchange_common_test.cpp, so these two cases
// only need to confirm each call site is wired to the right expected_prefix;
// WriteStopsWhenTheCarrierEraseTriggerReportsANonZeroStatus above covers the
// same wiring through unlock_and_erase's other caller.
struct RoutineStatusCase
{
    std::string name;
    void (*script)(ScriptedCanFlashTransport&);
    bytes::Bytes reply;
    std::string expected_log;
    std::string_view not_yet_logged;
};

class MitsuColtM32rCanExecutorRoutineStatusTest : public ::testing::TestWithParam<RoutineStatusCase>
{
};

TEST_P(MitsuColtM32rCanExecutorRoutineStatusTest, StopsTheWrite)
{
    const RoutineStatusCase& test_case = GetParam();
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = WritePlan(WriteRom());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    test_case.script(transport);
    transport.QueueRead(Response(test_case.reply));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, test_case.expected_log)));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, test_case.not_yet_logged))));
}

INSTANTIATE_TEST_SUITE_P(
    MitsuColtM32rCanExecutor, MitsuColtM32rCanExecutorRoutineStatusTest,
    ::testing::Values(RoutineStatusCase{"EraseTriggerReportsANonZeroStatus",
                                        ScriptWriteThroughEraseTrigger,
                                        {0x71, 0xe0, 0x01},
                                        "Erase trigger rejected: unexpected erase trigger response",
                                        "Userspace flash erased"},
                      RoutineStatusCase{"CrcCheckReportsANonZeroStatus",
                                        ScriptWriteThroughEraseRoutineCrcCheck,
                                        {0x71, 0xe1, 0x01},
                                        std::format("RoutineControl CRC check for 0x{:x} rejected: unexpected CRC "
                                                    "check for 0x{:x} response",
                                                    mitsu_colt_can::kEraseRoutineRamAddr,
                                                    mitsu_colt_can::kEraseRoutineRamAddr),
                                        "Erase page uploaded"}),
    [](const ::testing::TestParamInfo<RoutineStatusCase>& info) { return info.param.name; });

// Which failures abort is part of the wire contract, and the abort happens at
// this family's own call sites, not inside FatalRequest. Each case scripts a
// 512 KiB write up to one request, which the ECU refuses with an NRC, and
// nothing after it: any further request fails the scripted transport. Where a
// site sits inside UploadAndCommit, the case also covers the call site that
// invoked it (named in the comment).
struct SiteRejectionCase
{
    std::string name;
    void (*script)(ScriptedCanFlashTransport&);
    bytes::Byte service;
};

// Handshake, a matching top region, then the main write's routine uploads.
void ScriptWriteToMainUploads(ScriptedCanFlashTransport& transport)
{
    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
}

// Handshake, a mismatched top region and the carrier check, up to the
// bootstrap's redirect uploads.
void ScriptWriteToBootstrapUploads(ScriptedCanFlashTransport& transport)
{
    const bytes::Bytes rom = WriteRom();
    ScriptBootloadHandshake(transport);
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
}

void ScriptBootstrapThroughRedirectUploads(ScriptedCanFlashTransport& transport)
{
    ScriptWriteToBootstrapUploads(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWriteRedirectRoutine);
}

class MitsuColtM32rCanExecutorSiteRejectionTest : public ::testing::TestWithParam<SiteRejectionCase>
{
};

TEST_P(MitsuColtM32rCanExecutorSiteRejectionTest, StopsBeforeTheNextRequest)
{
    const SiteRejectionCase& test_case = GetParam();
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = WritePlan(WriteRom());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    test_case.script(transport);
    transport.QueueRead(Response({0x7f, test_case.service, 0x22}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

INSTANTIATE_TEST_SUITE_P(
    MitsuColtM32rCanExecutor, MitsuColtM32rCanExecutorSiteRejectionTest,
    ::testing::Values(
        // UploadAndCommit's RequestDownload, via WriteMem's erase-page upload.
        SiteRejectionCase{"EraseRoutineRequestDownload",
                          [](ScriptedCanFlashTransport& transport)
                          {
                              ScriptWriteToMainUploads(transport);
                              transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(
                                  mitsu_colt_can::kEraseRoutineRamAddr,
                                  static_cast<std::uint32_t>(std::size(mitsu_colt_can::kErasePageRoutine)))));
                          },
                          0x34},
        // UploadAndCommit's TransferData, via WriteMem's write-page upload.
        SiteRejectionCase{"WriteRoutineTransferData",
                          [](ScriptedCanFlashTransport& transport)
                          {
                              ScriptWriteToMainUploads(transport);
                              ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr,
                                                    mitsu_colt_can::kErasePageRoutine);
                              transport.Exchange(
                                  Request(mitsu_colt_can::BuildRequestDownload(
                                      mitsu_colt_can::kWriteRoutineRamAddr,
                                      static_cast<std::uint32_t>(std::size(mitsu_colt_can::kWritePageRoutine)))),
                                  Response({0x74}));
                              transport.Exchange(Request(
                                  mitsu_colt_can::BuildTransferDataFrames(mitsu_colt_can::kWritePageRoutine).front()));
                          },
                          0x36},
        // UploadAndCommit's checksum RequestDownload, via the bootstrap's erase
        // redirect upload.
        SiteRejectionCase{"BootstrapChecksumRequestDownload",
                          [](ScriptedCanFlashTransport& transport)
                          {
                              ScriptWriteToBootstrapUploads(transport);
                              ScriptUploadFrames(transport, mitsu_colt_can::kEraseRoutineRamAddr,
                                                 mitsu_colt_can::kEraseRedirectRoutine);
                              transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(
                                  mitsu_colt_can::kCrcTransferAddress, mitsu_colt_can::kCrcTransferSize)));
                          },
                          0x34},
        // UploadAndCommit's checksum TransferData, via the bootstrap's write
        // redirect upload.
        SiteRejectionCase{
            "BootstrapChecksumTransferData",
            [](ScriptedCanFlashTransport& transport)
            {
                ScriptWriteToBootstrapUploads(transport);
                ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr,
                                      mitsu_colt_can::kEraseRedirectRoutine);
                ScriptUploadFrames(transport, mitsu_colt_can::kWriteRoutineRamAddr,
                                   mitsu_colt_can::kWriteRedirectRoutine);
                transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(mitsu_colt_can::kCrcTransferAddress,
                                                                                mitsu_colt_can::kCrcTransferSize)),
                                   Response({0x74}));
                const std::uint16_t crc = mitsu_colt_can::Checksum(mitsu_colt_can::kWriteRedirectRoutine);
                transport.Exchange(Request(mitsu_colt_can::BuildTransferDataFrames(bytes::ComposeBe(crc)).front()));
            },
            0x36},
        // UnlockAndErase's reflash unlock, via WriteMem.
        SiteRejectionCase{"ReflashUnlock",
                          [](ScriptedCanFlashTransport& transport)
                          {
                              ScriptWriteThroughRoutineUploads(transport);
                              transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()));
                          },
                          0x3b},
        // UnlockAndErase's reflash unlock, via the bootstrap.
        SiteRejectionCase{"BootstrapReflashUnlock",
                          [](ScriptedCanFlashTransport& transport)
                          {
                              ScriptBootstrapThroughRedirectUploads(transport);
                              transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()));
                          },
                          0x3b},
        // UploadAndCommit's RequestDownload, via the bootstrap's top-region
        // redirect write.
        SiteRejectionCase{"BootstrapRedirectWriteRequestDownload",
                          [](ScriptedCanFlashTransport& transport)
                          {
                              ScriptBootstrapThroughRedirectUploads(transport);
                              ScriptUnlockAndErase(transport);
                              transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(
                                  mitsu_colt_can::kUserspaceStart, mitsu_colt_can::kTopRegionLength)));
                          },
                          0x34}),
    [](const ::testing::TestParamInfo<SiteRejectionCase>& info) { return info.param.name; });

TEST(MitsuColtM32rCanExecutor, RefusesATestWritePlanRatherThanWritingForReal)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    // build_mitsu_colt_m32r_can_plan refuses TestWrite, but validate_and_build
    // accepts it -- and a dry run must never reach the erase trigger.
    auto plan = WritePlanGranting({fastecu::flash::ConfirmationSpec::Id::kEraseTrigger,
                                   fastecu::flash::ConfirmationSpec::Id::kTopRegionBootstrap},
                                  WriteRom(), FlashOperation::kTestWrite);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kUnsupported));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, Not(Contains("Writing ROM, please wait...")));
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    // build_mitsu_colt_m32r_can_plan rejects this image, but validate_and_build
    // does not. The executor must still reject it before it configures or
    // opens the transport, let alone reaches the ECU handshake.
    auto plan = WritePlanGranting({fastecu::flash::ConfirmationSpec::Id::kEraseTrigger,
                                   fastecu::flash::ConfirmationSpec::Id::kTopRegionBootstrap},
                                  bytes::Bytes(mitsu_colt_can::kTopRegionEnd - 1, 0x00));
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig, HasSubstr("0x80000")));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_FALSE(transport.last_config.has_value());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesTheBootstrapWhenItsConfirmationIsAbsent)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    // Everything but the bootstrap gate is granted, and the top region does
    // not match -- so the run must stop at the gate rather than firing the
    // redirect routines.
    auto plan = WritePlanGranting({fastecu::flash::ConfirmationSpec::Id::kEraseTrigger});
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch. top_region_matches()
    // stops there instead of reading the full 128KB before comparing.
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Top 128KB bootstrap canceled by user")));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::kInfo, "Uploading erase redirect routine to RAM 0x805568..."))));
}

TEST_F(MitsuColtM32rCanWriteExecutor, WriteFailsWhenTheUserspaceCrcCheckIsRejected)
{
    ScriptWriteThroughRoutineUploads(transport);
    ScriptUnlockAndErase(transport);
    // Every byte goes out and is acknowledged, and only the ECU's own
    // post-write checksum verification fails. This is the one rejection that
    // says "the flash you just wrote does not match what you sent", so
    // reporting success here would send a user off to flash a bricked ECU.
    ScriptUploadFrames(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(mitsu_colt_can::kCrcTransferAddress,
                                                                    mitsu_colt_can::kCrcTransferSize)),
                       Response({0x74}));
    transport.Exchange(Request(mitsu_colt_can::BuildTransferDataFrames(bytes::Bytes{0x12, 0x34}).front()),
                       Response({0x76}));
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineCheckCrc(mitsu_colt_can::kUserspaceStart)),
                       Response({0x7f, 0x31, 0x22}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "RoutineControl CRC check for 0x8000 rejected: "
                                                             "Conditions not correct")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "ROM userspace write failed")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Userspace flash written"))));
    const fastecu::RecordedPhaseProgress *last = nullptr;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.phase_name == "Write userspace")
        {
            last = &event;
        }
    }
    ASSERT_NE(last, nullptr);
    EXPECT_LT(last->done, last->total);
    EXPECT_EQ(std::ranges::count(events.phase_progress_calls, "Verify userspace",
                                 &fastecu::RecordedPhaseProgress::phase_name),
              0);
}

TEST(MitsuColtM32rCanExecutor, VendorChallengeKeyRejectionStopsBeforeTheSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan(kVendorProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)),
                       Response({0x50, 0x81}));

    transport.Exchange(Request(mitsu_colt_can_vendor_ext::BuildChallengeSeedRequest()),
                       Response({0x63, 0x27, 0x41, 0x12, 0x34, 0x56, 0x78}));

    // Echoing the key subfunction is not the vendor extension's success
    // signal. Only response byte 0x34 grants access, so no session may be
    // started on the strength of this reply.
    const std::uint32_t key = mitsu_colt_can_vendor_ext::ChallengeInverseTransform(0x12345678);
    transport.Exchange(Request(mitsu_colt_can_vendor_ext::BuildChallengeKey(key)), Response({0x63, 0x27, 0x42}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    // fatal_query's generic mismatch wording (uds_client_exchange_common.h),
    // not the legacy text: the reply is a well-formed positive response to
    // the service that was sent, so what actually went wrong is the response
    // byte, not something an NRC decoder would explain.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "Vendor challenge key rejected: unexpected vendor challenge "
                                                "key response")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Vendor challenge accepted"))));
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesTheEraseTriggerWhenItsConfirmationIsAbsent)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    auto plan = WritePlanGranting({fastecu::flash::ConfirmationSpec::Id::kTopRegionBootstrap});

    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptWriteThroughRoutineUploads(transport);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    // The reflash-unlock payload is never scripted, so scriptConsumed() here is
    // the assertion that it never reached the bus.
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Erase trigger canceled by user")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Userspace flash erased"))));
}

// Pins the reply CAN id this family's plan wires into CanFlashUdsChannel. It
// uses the vendor read plan because its first exchange is the basic diagnostic
// session -- the shortest script that reaches a real exchange.

TEST(MitsuColtM32rCanExecutor, RejectsAFrameFromTheWrongReplyId)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan(kVendorProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    // Built inline: the response() helper hardcodes the plan's 0x7e8.
    bytes::Bytes wrong_id;
    bytes::AppendU32Be(wrong_id, 0x7e9);
    wrong_id.insert(wrong_id.end(), {0x50, 0x81});
    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)), wrong_id);

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("7e9")));
    EXPECT_TRUE(transport.ScriptConsumed());
}

} // namespace
