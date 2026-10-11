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
// which use a generic "{operation} rejected: " prefix instead. The
// operator-cancellation message has no legacy counterpart at all -- the legacy
// code logged nothing there.
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
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
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

constexpr std::string_view kProtocol384 = "mitsu_ecu_m32r_can";
constexpr std::string_view kVendorProtocol384 = "mitsu_ecu_m32r_can_vendor_ext";
constexpr std::string_view kProtocol512 = "mitsu_ecu_m32r_can_512kb";
constexpr std::string_view kMcu384 = "M32R_384KB_1block";
constexpr std::string_view kMcu512 = "M32R_512KB_1block";

class ScriptedCanFlashTransport : public fastecu::flash::ScriptedCanFlashTransport
{
  public:
    ScriptedCanFlashTransport()
        : fastecu::flash::ScriptedCanFlashTransport(fastecu::flash::ScriptedTransportInitialState::kOpen)
    {
    }
};

std::string_view McuFor(std::string_view protocol)
{
    return protocol == kProtocol512 ? kMcu512 : kMcu384;
}

// Every request on this bus carries a 4-byte big-endian 0x7E0 prefix
// (legacy build_request, flash_ecu_mitsu_m32r_can_operation.cpp:58-64).
bytes::Bytes Request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, 0x7e0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Responses carry the 4-byte 0x7E8 reply id; the legacy code indexes
// received.at(4) for the service byte throughout.
bytes::Bytes Response(bytes::ByteView tail)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, 0x7e8);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

bytes::Bytes Response(std::initializer_list<bytes::Byte> tail)
{
    return Response(bytes::ByteView{tail.begin(), tail.end()});
}

fastecu::Result<fastecu::flash::FlashPlan> ReadPlan(std::string_view protocol = kProtocol384)
{
    return BuildMitsuColtM32rCanPlan(FlashOperation::kRead, protocol, McuFor(protocol), std::nullopt);
}

// The two checksum bytes the executor must commit, in order, for the
// userspace slice of the ROM writeRom() builds. Written out rather than
// recomputed with checksum(): the scripted CRC frame is the only place the
// executor's own running sum and byte split are observable, so deriving the
// expectation the way the implementation does would let an inversion pass in
// both.
const bytes::Bytes kUserspaceChecksumBytes{0x12, 0x34};

// The ROM image every write test writes: kTopRegionEnd bytes, 0x00 below
// kTopRegionStart and 0xEE above it. The distinctive top-region fill is what
// makes the bootstrap comparison observable -- an ECU that reports 0xEE
// matches, one that reports 0xFF (erased flash) does not.
//
// The userspace window additionally opens with 18 * 0xFF followed by 0x46,
// which sums to exactly 0x1234 (18 * 255 + 70) over the otherwise-zero
// slice -- a checksum whose two halves differ, so the order they go on the
// wire in is pinned too (kUserspaceChecksumBytes).
bytes::Bytes WriteRom()
{
    bytes::Bytes rom(mitsu_colt_can::kTopRegionEnd, 0xA5);
    std::fill(rom.begin() + mitsu_colt_can::kUserspaceStart, rom.begin() + mitsu_colt_can::kTopRegionStart, 0x00);
    std::fill(rom.begin() + mitsu_colt_can::kTopRegionStart, rom.end(), 0xEE);
    std::fill_n(rom.begin() + mitsu_colt_can::kUserspaceStart, 18, 0xFF);
    rom[mitsu_colt_can::kUserspaceStart + 18] = 0x46;
    return rom;
}

bytes::Bytes WriteRom384()
{
    bytes::Bytes rom(0x60000, 0xA5);
    std::fill(rom.begin() + mitsu_colt_can::kUserspaceStart, rom.end(), 0x00);
    std::fill_n(rom.begin() + mitsu_colt_can::kUserspaceStart, 18, 0xFF);
    rom[mitsu_colt_can::kUserspaceStart + 18] = 0x46;
    return rom;
}

// A Write plan is what selects kSessionBootload, and kSessionBootload is the
// only thing that reaches connect_bootloader()'s factory SecurityAccess arm.
// This helper defaults to the 512 KiB protocol, whose builder declares both
// ConfirmationSpecs, so a plan from here is fully gated.
fastecu::Result<fastecu::flash::FlashPlan> WritePlan(bytes::Bytes rom, std::string_view protocol = kProtocol512)
{
    return BuildMitsuColtM32rCanPlan(FlashOperation::kWrite, protocol, McuFor(protocol), std::move(rom));
}

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

// The 4 seed bytes the ECU returns in the tests below. Deliberately all
// distinct and distinct from the surrounding framing bytes, so that reading
// the seed from any offset other than the legacy `received.mid(6, 4)` yields
// a different seed -- and therefore a different key on the wire, which the
// scripted transport rejects.
constexpr auto kSeed = std::to_array<bytes::Byte>({0x11, 0x22, 0x33, 0x44});

// Scripts the chunked ReadMemoryByAddress sweep over [start, start+length),
// filling every payload with `fill`.
void ScriptFlashRead(ScriptedCanFlashTransport& transport, std::uint32_t start, std::uint32_t length, bytes::Byte fill)
{
    const auto section = transport.Section("flash read");
    for (std::uint32_t addr = start; addr < start + length; addr += mitsu_colt_can::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = start + length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < mitsu_colt_can::kFlashReadBlockSize ? remaining : mitsu_colt_can::kFlashReadBlockSize);
        bytes::Bytes reply = Response({0x63});
        reply.insert(reply.end(), chunk, fill);
        transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(addr, chunk)), reply);
    }
}

// Scripts a single ReadMemoryByAddress chunk at `addr`, replying with
// `chunk_len` bytes of `fill`. Used where the caller wants exactly one
// chunk on the wire instead of scriptFlashRead's full-range sweep -- e.g.
// to prove top_region_matches() stops issuing reads at the first mismatch.
void ScriptFlashReadChunk(ScriptedCanFlashTransport& transport, std::uint32_t addr, bytes::Byte chunk_len,
                          bytes::Byte fill)
{
    const auto section = transport.Section("flash read chunk");
    bytes::Bytes reply = Response({0x63});
    reply.insert(reply.end(), chunk_len, fill);
    transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(addr, chunk_len)), reply);
}

void ScriptFlashReadData(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data)
{
    const auto section = transport.Section("flash read data");
    for (std::uint32_t offset = 0; offset < data.size(); offset += mitsu_colt_can::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = static_cast<std::uint32_t>(data.size()) - offset;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < mitsu_colt_can::kFlashReadBlockSize ? remaining : mitsu_colt_can::kFlashReadBlockSize);
        bytes::Bytes reply = Response({0x63});
        reply.insert(reply.end(), data.begin() + offset, data.begin() + offset + chunk);
        transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(start + offset, chunk)), reply);
    }
}

// Scripts the full sweep the executor must perform over the plan's transfer
// region.
void ScriptFullRead(ScriptedCanFlashTransport& transport, const fastecu::flash::FlashPlan& plan, bytes::Byte fill)
{
    const auto section = transport.Section("full read");
    ScriptFlashRead(transport, plan.TransferRegion().start, plan.TransferRegion().length, fill);
}

// Scripts a complete zero-based ROM read with address-derived data. The
// literal endpoints in the tests below then catch either a shifted first
// request or a truncated final request without duplicating thousands of
// chunk expectations.
void ScriptAddressMarkedRead(ScriptedCanFlashTransport& transport, std::uint32_t length)
{
    const auto section = transport.Section("address marked read");
    for (std::uint32_t addr = 0; addr < length; addr += mitsu_colt_can::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < mitsu_colt_can::kFlashReadBlockSize ? remaining : mitsu_colt_can::kFlashReadBlockSize);
        transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(addr, chunk)));
        bytes::Bytes reply = Response({0x63});
        for (std::uint32_t offset = 0; offset < chunk; ++offset)
        {
            reply.push_back(static_cast<bytes::Byte>(addr + offset));
        }
        transport.QueueRead(reply);
    }
}

// Scripts the bootload handshake every operation drives: session 0x85 then
// the factory SecurityAccess seed/key pair.
void ScriptBootloadHandshake(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("bootload handshake");
    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)),
                       Response({0x50, 0x85}));

    transport.Exchange(Request(mitsu_colt_can::BuildSecurityAccessSeedRequest()),
                       Response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    transport.Exchange(Request(mitsu_colt_can::BuildSecurityAccessKey(mitsu_colt_can::SeedKey(kSeed))),
                       Response({0x67, 0x06}));
}

// Scripts the payload half of one upload_and_commit(start, data): the
// RequestDownload and every accepted TransferData chunk, stopping before the
// checksum exchanges.
void ScriptUploadFrames(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data)
{
    const auto section = transport.Section("upload frames");
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(start, static_cast<std::uint32_t>(data.size()))),
                       Response({0x74}));

    for (const bytes::Bytes& chunk : mitsu_colt_can::BuildTransferDataFrames(data))
    {
        transport.Exchange(Request(chunk), Response({0x76}));
    }
}

// Scripts the checksum half of one upload_and_commit: the CRC RequestDownload,
// the single CRC TransferData frame, and the RoutineControl CRC check.
// `crcBytes`, when set, is the exact payload the test demands on the wire --
// both the value and the order of its two halves -- instead of one recomputed
// the way the implementation does.
void ScriptCrcCommit(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data,
                     std::optional<bytes::Bytes> crc_bytes = std::nullopt)
{
    const auto section = transport.Section("crc commit");
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(mitsu_colt_can::kCrcTransferAddress,
                                                                    mitsu_colt_can::kCrcTransferSize)),
                       Response({0x74}));

    const std::uint16_t crc = mitsu_colt_can::Checksum(data);
    const bytes::Bytes crc_data = crc_bytes.value_or(bytes::ComposeBe(crc));
    transport.Exchange(Request(mitsu_colt_can::BuildTransferDataFrames(crc_data).front()), Response({0x76}));

    // [echo][status=0], the routine-id echo plus the CRC-match status byte
    // (colt_commented.S ~0x5aa0-0x5ad4); the executor now checks the latter.
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineCheckCrc(start)), Response({0x71, 0xe1, 0x00}));
}

// Scripts one upload_and_commit(start, data): RequestDownload, the
// TransferData chunks, the CRC RequestDownload + TransferData, and the
// RoutineControl CRC check.
void ScriptUploadAndCommit(ScriptedCanFlashTransport& transport, std::uint32_t start, bytes::ByteView data,
                           std::optional<bytes::Bytes> crc_bytes = std::nullopt)
{
    const auto section = transport.Section("upload and commit");
    ScriptUploadFrames(transport, start, data);
    ScriptCrcCommit(transport, start, data, std::move(crc_bytes));
}

// Scripts the unlock + erase-trigger pair.
void ScriptUnlockAndErase(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("unlock and erase");
    transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()), Response({0x7b}));
    // [echo][status=0], the routine-id echo plus the erase-succeeded status
    // byte (colt_commented.S ~0x59c8-0x5a38); the executor now checks the
    // latter -- this is the exact wire shape a real ECU sends.
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineErase()), Response({0x71, 0xe0, 0x00}));
}

void ScriptWriteThroughEraseTrigger(ScriptedCanFlashTransport& transport)
{
    const auto section = transport.Section("write through erase trigger");
    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
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

// The userspace slice of `rom` the write path must transfer.
bytes::ByteView UserspaceOf(const bytes::Bytes& rom)
{
    return bytes::ByteView(rom.data() + mitsu_colt_can::kUserspaceStart,
                           mitsu_colt_can::kUserspaceEnd - mitsu_colt_can::kUserspaceStart);
}

// The top-region slice of `rom` the bootstrap must write and verify.
bytes::ByteView TopRegionOf(const bytes::Bytes& rom)
{
    return bytes::ByteView(rom.data() + mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength);
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

TEST(MitsuColtM32rCanExecutor, ReadReturnsEachProtocolCapacityFromAddressZero)
{
    for (const auto [protocol, size] : std::to_array<std::pair<std::string_view, std::uint32_t>>({
             {kProtocol384, 0x60000},
             {kProtocol512, 0x80000},
         }))
    {
        ScriptedCanFlashTransport transport;
        FakeClock clock;
        RecordingEventSink events;
        fastecu::ManualCancellationToken cancellation;
        MitsuColtM32rCanExecutor executor;
        const auto plan = ReadPlan(protocol);
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        ScriptBootloadHandshake(transport);
        ScriptAddressMarkedRead(transport, size);

        const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

        ASSERT_THAT(result, fastecu::testing::IsOk()) << protocol;
        ASSERT_TRUE(result->read_bytes.has_value());
        ASSERT_EQ(result->read_bytes->size(), size);
        EXPECT_EQ(result->read_bytes->front(), 0x00);
        EXPECT_EQ(result->read_bytes->back(), 0xff);
        EXPECT_TRUE(transport.ScriptConsumed());
    }
}

TEST(MitsuColtM32rCanExecutor, ReadReportsAnEmptyReplyAsTimeout)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)));
    transport.QueueNoFrame();

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
}

TEST(MitsuColtM32rCanExecutor, ReadPropagatesADisconnectedTransport)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)));
    transport.QueueError(ErrorKind::kDisconnected, "adapter gone");

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kDisconnected));
}

TEST(MitsuColtM32rCanExecutor, ReadTimeoutPropagates)
{
    // A genuine transport-level timeout (distinct from queue_no_frame's empty
    // reply, which UdsClient itself maps to Timeout) at the very first
    // exchange must surface as ErrorKind::kTimeout unmodified.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)));
    transport.QueueError(ErrorKind::kTimeout, "no reply");

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::kTimeout));
}

TEST(MitsuColtM32rCanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)),
                       Response({0x50, 0x85}));
    cancellation.Cancel();

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

// Cancels the token as soon as the first chunk's progress is reported, so what
// stops the read is the loop's own top-of-chunk cancellation check (legacy
// readFlashRange) rather than a token that was already cancelled before
// execute() was called.
class CancelAfterFirstChunkSink final : public RecordingEventSink
{
  public:
    explicit CancelAfterFirstChunkSink(fastecu::ManualCancellationToken& source) : source_(source)
    {
    }
    void PhaseProgress(const fastecu::PhaseProgressEvent& event) override
    {
        RecordingEventSink::PhaseProgress(event);
        if (event.phase_name == "Read ROM" && event.done > 0)
        {
            source_.Cancel();
        }
    }

  private:
    fastecu::ManualCancellationToken& source_;
};

TEST(MitsuColtM32rCanExecutor, ReadStopsAtTheNextChunkWhenCancelledMidRead)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    fastecu::ManualCancellationToken cancellation;
    CancelAfterFirstChunkSink events{cancellation};
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);

    // Exactly one read chunk is scripted; the executor is cancelled while it
    // is being served, so the loop must stop at the top of the next chunk.
    const std::uint32_t start = plan->TransferRegion().start;
    bytes::Bytes reply = Response({0x63});
    reply.insert(reply.end(), mitsu_colt_can::kFlashReadBlockSize, 0x5A);
    transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(
                           start, static_cast<bytes::Byte>(mitsu_colt_can::kFlashReadBlockSize))),
                       reply);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    const fastecu::RecordedPhaseProgress *last = nullptr;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.phase_name == "Read ROM")
        {
            last = &event;
        }
    }
    ASSERT_NE(last, nullptr);
    EXPECT_LT(last->done, last->total);
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

TEST(MitsuColtM32rCanExecutor, WriteDrivesTheBootloadSessionThenFactorySecurityAccess)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = WritePlan(WriteRom());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    // a write selects kSessionBootload (0x85), not kSessionBasic.
    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)),
                       Response({0x50, 0x85}));

    // SID 0x27/5, answered with (0x27+0x40, 0x05) and a 4-byte seed at
    // received.mid(6, 4).
    transport.Exchange(Request(mitsu_colt_can::BuildSecurityAccessSeedRequest()),
                       Response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    // the key is seedKey(seed), and the request carries it verbatim. Scripting
    // the exact bytes means a wrong seed offset or a wrong key derivation is
    // rejected by the transport, not silently accepted.
    transport.Exchange(Request(mitsu_colt_can::BuildSecurityAccessKey(mitsu_colt_can::SeedKey(kSeed))),
                       Response({0x67, 0x06}));

    // The handshake is what this test pins, so the run is stopped one request
    // into the write path: the first chunk of the top-region check goes
    // unanswered. That the request is a top-region read at all is the proof
    // the write branch now proceeds instead of refusing.
    transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(
        mitsu_colt_can::kTopRegionStart, static_cast<bytes::Byte>(mitsu_colt_can::kFlashReadBlockSize))));
    transport.QueueNoFrame();

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Checking top 128KB (0x60000-0x80000)...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Diagnostic session ok")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Requesting security seed...")));
    // Pins both the mid(6, 4) offset and bytes::toHex's exact format.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Received seed: 11 22 33 44 ")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, HasSubstr("Calculated seed key: "))));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Sending seed key to ECU...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Security access ok")));
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

    ScriptBootloadHandshake(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom), kUserspaceChecksumBytes);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));

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

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
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

TEST(MitsuColtM32rCanExecutor, WriteRunsTheBootstrapWhenTheTopRegionDiffers)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch, so the
    // bootstrap runs. top_region_matches() stops at that first chunk rather
    // than reading the full 128KB before comparing.
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWriteRedirectRoutine);
    ScriptUnlockAndErase(transport);
    // The carrier address is kUserspaceStart, not kTopRegionStart: the redirect
    // routines add the +0x058000 offset themselves.
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    // Verify read-back returns what was written.
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    // Then the ordinary write proceeds.
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kInfo, "Top 128KB mismatch, bootstrapping via redirect routines...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Uploading erase redirect routine to RAM 0x805568...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Uploading write redirect routine to RAM 0x8054ac...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Carrier window erased")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Top 128KB written via redirect")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Top 128KB verified")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Userspace flash written")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Userspace flash verified")));
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesRedirectBootstrapWhenTheCarrierDoesNotMatchTheDesiredTopPayload)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

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

TEST(MitsuColtM32rCanExecutor, WritePropagatesACarrierReadFailureBeforeRedirectHelpers)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

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
    // Unlike WriteRunsTheBootstrapWhenTheTopRegionDiffers (whose ECU
    // mismatches from byte 0), this proves top_region_matches() keeps
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

TEST(MitsuColtM32rCanExecutor, WriteFailsWhenTheUserspaceVerifyMismatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    ScriptUnlockAndErase(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kUserspaceStart, UserspaceOf(rom));
    ScriptFlashRead(transport, mitsu_colt_can::kUserspaceStart,
                    mitsu_colt_can::kUserspaceEnd - mitsu_colt_can::kUserspaceStart, 0xFF);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Userspace verify failed after write")));
}

TEST(MitsuColtM32rCanExecutor, WriteFailsWhenTheTopRegionVerifyMismatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch. See
    // WriteRunsTheBootstrapWhenTheTopRegionDiffers for why only one chunk is
    // scripted for this initial check (the second scriptFlashRead below, after
    // the write, is the full post-write verify read and is unaffected).
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

TEST(MitsuColtM32rCanExecutor, WriteStopsWhenTheReflashUnlockIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()), Response({0x7f, 0x3b, 0x33}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    // The erase trigger never goes out.
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "the reflash unlock request rejected: Security access denied")));
}

TEST(MitsuColtM32rCanExecutor, WriteStopsWhenTheEraseTriggerIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()), Response({0x7b}));
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineErase()), Response({0x7f, 0x31, 0x22}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    // No RequestDownload for the ROM userspace follows a refused erase.
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Erase trigger rejected: Conditions not correct")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Userspace flash erased"))));
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

TEST(MitsuColtM32rCanExecutor, HandshakeRejectsAReplyTooShortToHoldAServiceByte)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    // Two bytes: shorter than the 4-byte reply id every frame on this bus
    // starts with, so there is no service byte to read. The legacy
    // `received.mid(4, ...)` clamped and the frame decoded as the useless
    // "Not a valid answer"; CanFlashUdsChannel now rejects the frame before
    // any PDU is parsed, and says what was wrong with it.
    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)),
                       bytes::Bytes{0x07, 0xe8});

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "Wrong response from ECU: CAN frame of 2 bytes is shorter than "
                                                "its 4-byte id envelope")));
}

TEST(MitsuColtM32rCanExecutor, ReadRejectsAChunkAnsweredWithTheWrongService)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);

    // The first chunk comes back as a negative response. Accepting it would
    // append 192 bytes of framing garbage to the ROM image at offset 0.
    const std::uint32_t start = plan->TransferRegion().start;
    transport.Exchange(Request(mitsu_colt_can::BuildReadMemoryByAddress(
                           start, static_cast<bytes::Byte>(mitsu_colt_can::kFlashReadBlockSize))),
                       Response({0x7f, 0x23, 0x22}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    // Nothing is read after the rejected chunk.
    EXPECT_TRUE(transport.ScriptConsumed());
    // The failing address is part of the message, so a chunk rejected halfway
    // through a 384KB sweep is locatable.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "the flash read at 0x0 rejected: Conditions not correct")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "ROM read complete"))));
}

// Cancels the token when a chosen log line is emitted, so the
// stop lands between two exchanges of the write path rather than at
// readFlashRange()'s own top-of-chunk checkpoint.
class CancelOnLogSink final : public RecordingEventSink
{
  public:
    CancelOnLogSink(fastecu::ManualCancellationToken& source, std::string trigger)
        : source_(source), trigger_(std::move(trigger))
    {
    }
    void Log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::Log(level, message);
        if (message == trigger_)
        {
            source_.Cancel();
        }
    }

  private:
    fastecu::ManualCancellationToken& source_;
    std::string trigger_;
};

TEST(MitsuColtM32rCanExecutor, WriteStopsAtTheNextExchangeWhenCancelledMidWrite)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    fastecu::ManualCancellationToken cancellation;
    // Cancelled the instant the erase-page upload reports success: the run
    // must stop before the write-page upload's first request reaches the bus.
    // The write path has no loop of its own to poll a token, so what has to
    // hold here is UdsClient::request()'s own pre-request check -- the
    // executor-local exchange() this used to name is gone.
    CancelOnLogSink events{cancellation, "Erase page uploaded"};
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, "cancelled before request"));
    // Nothing beyond the erase-page upload was scripted, so this is the
    // assertion that no further request went out.
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Write page uploaded"))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Userspace flash erased"))));
    // The stop is the operator's, not the ECU's. No exchange may report a
    // cancellation as a rejection -- here the request was never transmitted
    // at all, so there is nothing for the ECU to have refused.
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kError, HasSubstr("rejected: cancelled")))));
}

// Cancels the token from inside write() when one chosen request
// goes out, so the stop lands after the ECU has been asked to act and before
// its reply is read.
class CancelOnRequestTransport final : public ScriptedCanFlashTransport
{
  public:
    CancelOnRequestTransport(fastecu::ManualCancellationToken& source, bytes::Bytes trigger)
        : source_(source), trigger_(std::move(trigger))
    {
    }
    fastecu::Status Write(bytes::ByteView data, const fastecu::ICancellationToken& cancellation) override
    {
        fastecu::Status result = ScriptedCanFlashTransport::Write(data, cancellation);
        if (std::ranges::equal(data, trigger_))
        {
            source_.Cancel();
        }
        return result;
    }

  private:
    fastecu::ManualCancellationToken& source_;
    bytes::Bytes trigger_;
};

TEST(MitsuColtM32rCanExecutor, ACancelledEraseTriggerIsNotReportedAsAnEcuRejection)
{
    // The most dangerous log line on this path. If the cancel lands after the
    // erase trigger is sent and before its reply, the erase is RUNNING in the
    // ECU. Bench checklist step 6 asks the operator to judge exactly that and
    // decide whether to recover the unit, so the log must not tell them the
    // ECU refused the request -- nor leave a dangling "rejected: " carrying no
    // reason at all, which is what an empty cancellation detail produced.
    fastecu::ManualCancellationToken cancellation;
    CancelOnRequestTransport transport{cancellation, Request(mitsu_colt_can::BuildRoutineErase())};
    FakeClock clock;
    RecordingEventSink events;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom384();
    auto plan = WritePlan(rom, kProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()), Response({0x7b}));
    // The erase trigger reaches the bus and the operator cancels. No reply is
    // scripted because none is ever read.
    transport.Exchange(Request(mitsu_colt_can::BuildRoutineErase()));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kError, HasSubstr("Erase trigger rejected")))));
    // The replacement line names the operator as the cause and does not claim
    // the ECU is idle.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kWarning, HasSubstr("Cancelled by operator during the erase trigger"))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Userspace flash erased"))));
}

// Fails every write outright, the way a yanked adapter does.
class WriteFailingTransport final : public ScriptedCanFlashTransport
{
  public:
    fastecu::Status Write(bytes::ByteView, const fastecu::ICancellationToken&) override
    {
        return fastecu::Fail(ErrorKind::kDisconnected, "adapter write failed");
    }
};

TEST(MitsuColtM32rCanExecutor, AFailedTransportWriteIsReportedWithoutWaitingForAReply)
{
    WriteFailingTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    // A perfectly good session reply is waiting. If the write failure were
    // swallowed, the run would consume it and carry on into the read sweep;
    // the transport's own error is what must come back instead.
    transport.QueueRead(Response({0x50, 0x85}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, "adapter write failed"));
    EXPECT_FALSE(transport.ScriptConsumed()); // the queued reply was never read
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Diagnostic session ok"))));
}

// Cancels the token from inside write(), so the request does
// reach the bus and the cancellation is first observable at the delay that
// follows it.
class CancelOnWriteTransport final : public ScriptedCanFlashTransport
{
  public:
    explicit CancelOnWriteTransport(fastecu::ManualCancellationToken& source) : source_(source)
    {
    }
    fastecu::Status Write(bytes::ByteView data, const fastecu::ICancellationToken& cancellation) override
    {
        fastecu::Status result = ScriptedCanFlashTransport::Write(data, cancellation);
        source_.Cancel();
        return result;
    }

  private:
    fastecu::ManualCancellationToken& source_;
};

TEST(MitsuColtM32rCanExecutor, ACancellationThatArrivesAfterTheRequestStopsBeforeTheReply)
{
    fastecu::ManualCancellationToken cancellation;
    CancelOnWriteTransport transport{cancellation};
    FakeClock clock;
    RecordingEventSink events;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)),
                       Response({0x50, 0x85}));

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
    // No pre-read delay separates the write from the read on this path (see
    // kRoutineExchangePolicy/kSlowExchangePolicy), so a cancel() call
    // right after the write is caught by the read call itself rather than by
    // an inter-exchange sleep -- hence the transport's own detail string.
    EXPECT_EQ(result.error().detail, "scripted CAN read cancelled");
    EXPECT_FALSE(transport.ScriptConsumed()); // the reply was never read
}

TEST(MitsuColtM32rCanExecutor, WriteAbortsWhenTheEraseRoutineRequestDownloadIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    // The bootloader refuses to open the RAM window for the erase-page
    // routine. Continuing would erase flash with whatever happens to be at
    // kEraseRoutineRamAddr.
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(
                           mitsu_colt_can::kEraseRoutineRamAddr,
                           static_cast<std::uint32_t>(std::size(mitsu_colt_can::kErasePageRoutine)))),
                       Response({0x7f, 0x34, 0x33}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "RequestDownload to 0x805568 rejected: Security access denied")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Erase-page routine upload failed")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Erase page uploaded"))));
}

TEST(MitsuColtM32rCanExecutor, WriteAbortsWhenTheWriteRoutineTransferDataIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    // The window opens but the payload frame is refused: a half-uploaded
    // write-page routine must never be handed the erase trigger.
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(
                           mitsu_colt_can::kWriteRoutineRamAddr,
                           static_cast<std::uint32_t>(std::size(mitsu_colt_can::kWritePageRoutine)))),
                       Response({0x74}));
    transport.Exchange(Request(mitsu_colt_can::BuildTransferDataFrames(mitsu_colt_can::kWritePageRoutine).front()),
                       Response({0x7f, 0x36, 0x31}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "TransferData to 0x8054ac rejected: Request out of range")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Write-page routine upload failed")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Userspace flash erased"))));
}

TEST(MitsuColtM32rCanExecutor, WriteFailsWhenTheUserspaceCrcCheckIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);
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

TEST(MitsuColtM32rCanExecutor, BootstrapAbortsWhenTheChecksumRequestDownloadIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch. top_region_matches()
    // stops there instead of reading the full 128KB before comparing.
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    // The erase redirect routine uploads, then the ECU refuses to open the
    // checksum window -- so the routine is in RAM but unverified, and the
    // bootstrap must not go on to erase the carrier window with it.
    ScriptUploadFrames(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(mitsu_colt_can::kCrcTransferAddress,
                                                                    mitsu_colt_can::kCrcTransferSize)),
                       Response({0x7f, 0x34, 0x22}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "RequestDownload for the checksum rejected: "
                                                             "Conditions not correct")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Erase redirect routine upload failed")));
}

TEST(MitsuColtM32rCanExecutor, BootstrapAbortsWhenTheChecksumTransferDataIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch. top_region_matches()
    // stops there instead of reading the full 128KB before comparing.
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    ScriptUploadFrames(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWriteRedirectRoutine);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestDownload(mitsu_colt_can::kCrcTransferAddress,
                                                                    mitsu_colt_can::kCrcTransferSize)),
                       Response({0x74}));
    {
        const std::uint16_t crc = mitsu_colt_can::Checksum(mitsu_colt_can::kWriteRedirectRoutine);
        const bytes::Bytes crc_data = bytes::ComposeBe(crc);
        transport.Exchange(Request(mitsu_colt_can::BuildTransferDataFrames(crc_data).front()));
    }
    transport.QueueRead(Response({0x7f, 0x36, 0x22}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "TransferData for the checksum rejected: "
                                                             "Conditions not correct")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kError, "Write redirect routine upload failed")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Carrier window erased"))));
}

TEST(MitsuColtM32rCanExecutor, BootstrapReportsItsOwnReflashUnlockRejection)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = WriteRom();
    auto plan = WritePlan(rom);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    ScriptBootloadHandshake(transport);
    // ECU reports 0xFF from the very first chunk: mismatch. top_region_matches()
    // stops there instead of reading the full 128KB before comparing.
    ScriptFlashReadChunk(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kFlashReadBlockSize, 0xFF);
    ScriptFlashReadData(transport, mitsu_colt_can::kUserspaceStart, TopRegionOf(rom));
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kEraseRedirectRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWriteRedirectRoutine);
    transport.Exchange(Request(mitsu_colt_can::BuildRequestReflashUnlock()), Response({0x7f, 0x3b, 0x22}));

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
    // The bootstrap's own message, distinct from the main write's: the two
    // erase stages are otherwise identical on the wire, so `stage` is the
    // only thing that says which one failed.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::kError, "the reflash unlock request (top 128KB bootstrap) rejected: "
                                                "Conditions not correct")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kError, "the reflash unlock request rejected: "
                                                                 "Conditions not correct"))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Carrier window erased"))));
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

    ScriptBootloadHandshake(transport);
    ScriptFlashRead(transport, mitsu_colt_can::kTopRegionStart, mitsu_colt_can::kTopRegionLength, 0xEE);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kEraseRoutineRamAddr, mitsu_colt_can::kErasePageRoutine);
    ScriptUploadAndCommit(transport, mitsu_colt_can::kWriteRoutineRamAddr, mitsu_colt_can::kWritePageRoutine);

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    // The reflash-unlock payload is never scripted, so scriptConsumed() here is
    // the assertion that it never reached the bus.
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Erase trigger canceled by user")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::kInfo, "Userspace flash erased"))));
}

// The four below cover what UdsClient and CanFlashUdsChannel now contribute to
// every exchange this executor makes. They use the vendor read plan because its
// first exchange is the basic diagnostic session -- the shortest script that
// reaches a real exchange.

TEST(MitsuColtM32rCanExecutor, AbsorbsResponsePendingWithoutResending)
{
    // The safety property, end to end: 0x78 is absorbed by re-READING. If the
    // client re-sent instead, the scripted transport would see a third write
    // it has no expectation for and fail it.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan(kVendorProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)),
                       Response({0x7f, 0x10, 0x78}));
    transport.QueueRead(Response({0x50, 0x81}));

    transport.Exchange(Request(mitsu_colt_can_vendor_ext::BuildChallengeSeedRequest()));
    transport.QueueNoFrame(); // stop here: pending absorption is what this pins

    ASSERT_THAT(executor.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
    EXPECT_TRUE(transport.ScriptConsumed());
    // The session was accepted on the second read, not abandoned on the first.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::kInfo, "Basic diagnostic session ok")));
}

TEST(MitsuColtM32rCanExecutor, FailsWhenTheEcuPendsPastTheRepeatLimit)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan(kVendorProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)));
    // One normal read plus the default max_pending_repeats of 10.
    for (int i = 0; i < 11; ++i)
    {
        transport.QueueRead(Response({0x7f, 0x10, 0x78}));
    }

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kTimeout, HasSubstr("responsePending")));
    // An ECU that pends forever is waited out, never re-sent to.
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(MitsuColtM32rCanExecutor, RejectsAResponseToADifferentService)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::ManualCancellationToken cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = ReadPlan(kVendorProtocol384);
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    // A SecurityAccess reply (0x67) to a DiagnosticSession request (0x10).
    transport.Exchange(Request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)),
                       Response({0x67, 0x05}));

    const auto result = executor.Execute(*plan, transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, HasSubstr("0x10")));
    EXPECT_THAT(result.error().detail, HasSubstr("0x27"));
    EXPECT_TRUE(transport.ScriptConsumed());
}

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
