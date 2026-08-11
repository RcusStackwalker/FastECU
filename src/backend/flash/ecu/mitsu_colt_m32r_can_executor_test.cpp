// Equivalence tests for MitsuColtM32rCanExecutor, the portable replacement for
// FlashEcuMitsuM32rCanOperation's connect_bootloader(), readFlashRange(),
// read_mem(), upload_and_commit(), ensureTopRegionWritten() and write_mem().
// Every expected request below is built with the same
// MitsuColtCan/MitsuColtCanVendorExt builders the legacy class calls (through
// its qt_colt.h *Frame shims), and every expected log string is copied
// character-for-character from
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_mitsu_m32r_can_operation.cpp.
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/flash_cancellation.h"
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
using fastecu::flash::build_mitsu_colt_m32r_can_plan;
using fastecu::flash::FlashOperation;
using fastecu::flash::MitsuColtM32rCanExecutor;
using fastecu::flash::ScriptedCanFlashTransport;
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

std::string_view mcuFor(std::string_view protocol)
{
    return protocol == kProtocol512 ? kMcu512 : kMcu384;
}

// Every request on this bus carries a 4-byte big-endian 0x7E0 prefix
// (legacy build_request, flash_ecu_mitsu_m32r_can_operation.cpp:58-64).
bytes::Bytes request(bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Responses carry the 4-byte 0x7E8 reply id; the legacy code indexes
// received.at(4) for the service byte throughout.
bytes::Bytes response(std::initializer_list<bytes::Byte> tail)
{
    bytes::Bytes out;
    bytes::appendU32Be(out, 0x7e8);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

fastecu::flash::FlashPlan readPlan(std::string_view protocol = kProtocol384)
{
    auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, protocol,
                                               mcuFor(protocol), std::nullopt);
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
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
bytes::Bytes writeRom()
{
    bytes::Bytes rom(MitsuColtCan::kTopRegionEnd, 0xA5);
    std::fill(rom.begin() + MitsuColtCan::kUserspaceStart,
              rom.begin() + MitsuColtCan::kTopRegionStart, 0x00);
    std::fill(rom.begin() + MitsuColtCan::kTopRegionStart, rom.end(), 0xEE);
    std::fill_n(rom.begin() + MitsuColtCan::kUserspaceStart, 18, 0xFF);
    rom[MitsuColtCan::kUserspaceStart + 18] = 0x46;
    return rom;
}

bytes::Bytes writeRom384()
{
    bytes::Bytes rom(0x60000, 0xA5);
    std::fill(rom.begin() + MitsuColtCan::kUserspaceStart, rom.end(), 0x00);
    std::fill_n(rom.begin() + MitsuColtCan::kUserspaceStart, 18, 0xFF);
    rom[MitsuColtCan::kUserspaceStart + 18] = 0x46;
    return rom;
}

// A Write plan is what selects kSessionBootload, and kSessionBootload is the
// only thing that reaches connect_bootloader()'s factory SecurityAccess arm
// (legacy lines 131-165). This helper defaults to the 512 KiB protocol, whose
// builder declares both ConfirmationSpecs, so a plan from here is fully gated.
fastecu::flash::FlashPlan writePlan(bytes::Bytes rom,
                                    std::string_view protocol = kProtocol512)
{
    auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, protocol,
                                               mcuFor(protocol),
                                               std::move(rom));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// Hand-built rather than produced by build_mitsu_colt_m32r_can_plan: the
// default 512 KiB plan declares both confirmations, and validate_and_build
// does not require them, so this is the only way to reach the executor with a
// Write plan whose high-risk step was never granted.
fastecu::flash::FlashPlan writePlanGranting(
    std::initializer_list<fastecu::flash::ConfirmationSpec::Id> granted,
    bytes::Bytes rom = writeRom(), FlashOperation operation = FlashOperation::Write,
    std::uint32_t rom_size = MitsuColtCan::kFullRomSize)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = operation;
    fields.family = fastecu::flash::FlashFamily::MitsuColtM32rCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = std::string(rom_size == 0x60000 ? kProtocol384 : kProtocol512);
    fields.mcu_name = std::string(rom_size == 0x60000 ? kMcu384 : kMcu512);
    fields.transfer_region = fastecu::flash::MemoryRegion{
        MitsuColtCan::kUserspaceStart, rom_size - MitsuColtCan::kUserspaceStart};
    fields.image = std::move(rom);
    fields.family_plan = fastecu::flash::MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = false,
        .session_id = MitsuColtCan::kSessionBootload,
    };
    for (const fastecu::flash::ConfirmationSpec::Id id : granted)
    {
        fields.confirmations.push_back(fastecu::flash::ConfirmationSpec{id, {}});
    }
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

fastecu::flash::FlashPlan handBuiltWritePlan(
    std::string_view target, std::string_view mcu, bool vendor,
    fastecu::flash::MemoryRegion region, std::size_t image_size)
{
    fastecu::flash::FlashPlanFields fields;
    fields.operation = FlashOperation::Write;
    fields.family = fastecu::flash::FlashFamily::MitsuColtM32rCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
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
        .session_id = MitsuColtCan::kSessionBootload,
    };
    auto plan = fastecu::flash::validate_and_build(std::move(fields));
    EXPECT_TRUE(plan.has_value()) << plan.error().detail;
    return std::move(*plan);
}

// The 4 seed bytes the ECU returns in the tests below. Deliberately all
// distinct and distinct from the surrounding framing bytes, so that reading
// the seed from any offset other than the legacy `received.mid(6, 4)` yields
// a different seed -- and therefore a different key on the wire, which the
// scripted transport rejects.
constexpr bytes::Byte kSeed[] = {0x11, 0x22, 0x33, 0x44};

// Scripts the chunked ReadMemoryByAddress sweep over [start, start+length),
// filling every payload with `fill`.
void scriptFlashRead(ScriptedCanFlashTransport& transport, std::uint32_t start,
                     std::uint32_t length, bytes::Byte fill)
{
    for (std::uint32_t addr = start; addr < start + length;
         addr += MitsuColtCan::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = start + length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < MitsuColtCan::kFlashReadBlockSize ? remaining
                                                          : MitsuColtCan::kFlashReadBlockSize);
        transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(addr, chunk)));
        bytes::Bytes reply = response({0x63});
        reply.insert(reply.end(), chunk, fill);
        transport.queueRead(reply);
    }
}

void scriptFlashReadData(ScriptedCanFlashTransport& transport, std::uint32_t start,
                         bytes::ByteView data)
{
    for (std::uint32_t offset = 0; offset < data.size();
         offset += MitsuColtCan::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = static_cast<std::uint32_t>(data.size()) - offset;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < MitsuColtCan::kFlashReadBlockSize ? remaining
                                                          : MitsuColtCan::kFlashReadBlockSize);
        transport.expectWrite(
            request(MitsuColtCan::buildReadMemoryByAddress(start + offset, chunk)));
        bytes::Bytes reply = response({0x63});
        reply.insert(reply.end(), data.begin() + offset, data.begin() + offset + chunk);
        transport.queueRead(reply);
    }
}

// Scripts the full sweep the executor must perform over the plan's transfer
// region.
void scriptFullRead(ScriptedCanFlashTransport& transport, const fastecu::flash::FlashPlan& plan,
                    bytes::Byte fill)
{
    scriptFlashRead(transport, plan.transfer_region().start, plan.transfer_region().length, fill);
}

// Scripts a complete zero-based ROM read with address-derived data. The
// literal endpoints in the tests below then catch either a shifted first
// request or a truncated final request without duplicating thousands of
// chunk expectations.
void scriptAddressMarkedRead(ScriptedCanFlashTransport& transport, std::uint32_t length)
{
    for (std::uint32_t addr = 0; addr < length;
         addr += MitsuColtCan::kFlashReadBlockSize)
    {
        const std::uint32_t remaining = length - addr;
        const auto chunk = static_cast<bytes::Byte>(
            remaining < MitsuColtCan::kFlashReadBlockSize ? remaining
                                                          : MitsuColtCan::kFlashReadBlockSize);
        transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(addr, chunk)));
        bytes::Bytes reply = response({0x63});
        for (std::uint32_t offset = 0; offset < chunk; ++offset)
        {
            reply.push_back(static_cast<bytes::Byte>((addr + offset) & 0xff));
        }
        transport.queueRead(reply);
    }
}

// Scripts the bootload handshake every operation drives: session 0x85 then
// the factory SecurityAccess seed/key pair.
void scriptBootloadHandshake(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));

    transport.expectWrite(request(MitsuColtCan::buildSecurityAccessSeedRequest()));
    transport.queueRead(response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    transport.expectWrite(
        request(MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(kSeed))));
    transport.queueRead(response({0x67, 0x06}));
}

// Scripts the payload half of one upload_and_commit(start, data): the
// RequestDownload and every accepted TransferData chunk (legacy lines
// 238-260), stopping before the checksum exchanges.
void scriptUploadFrames(ScriptedCanFlashTransport& transport, std::uint32_t start,
                        bytes::ByteView data)
{
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        start, static_cast<std::uint32_t>(data.size()))));
    transport.queueRead(response({0x74}));

    for (const bytes::Bytes& chunk : MitsuColtCan::buildTransferDataFrames(data))
    {
        transport.expectWrite(request(chunk));
        transport.queueRead(response({0x76}));
    }
}

// Scripts the checksum half of one upload_and_commit (legacy lines 262-294):
// the CRC RequestDownload, the single CRC TransferData frame, and the
// RoutineControl CRC check. `crcBytes`, when set, is the exact payload the
// test demands on the wire -- both the value and the order of its two
// halves -- instead of one recomputed the way the implementation does.
void scriptCrcCommit(ScriptedCanFlashTransport& transport, std::uint32_t start,
                     bytes::ByteView data,
                     std::optional<bytes::Bytes> crcBytes = std::nullopt)
{
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize)));
    transport.queueRead(response({0x74}));

    const std::uint16_t crc = MitsuColtCan::checksum(data);
    const bytes::Bytes crcData =
        crcBytes.value_or(bytes::Bytes{static_cast<bytes::Byte>((crc >> 8) & 0xff),
                                       static_cast<bytes::Byte>(crc & 0xff)});
    transport.expectWrite(request(MitsuColtCan::buildTransferDataFrames(crcData).front()));
    transport.queueRead(response({0x76}));

    transport.expectWrite(request(MitsuColtCan::buildRoutineCheckCrc(start)));
    transport.queueRead(response({0x71}));
}

// Scripts one upload_and_commit(start, data): RequestDownload, the
// TransferData chunks, the CRC RequestDownload + TransferData, and the
// RoutineControl CRC check (legacy lines 231-297).
void scriptUploadAndCommit(ScriptedCanFlashTransport& transport, std::uint32_t start,
                           bytes::ByteView data,
                           std::optional<bytes::Bytes> crcBytes = std::nullopt)
{
    scriptUploadFrames(transport, start, data);
    scriptCrcCommit(transport, start, data, std::move(crcBytes));
}

// Scripts the unlock + erase-trigger pair (legacy lines 349-368 / 445-464).
void scriptUnlockAndErase(ScriptedCanFlashTransport& transport)
{
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7b}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineErase()));
    transport.queueRead(response({0x71}));
}

// The userspace slice of `rom` the write path must transfer.
bytes::ByteView userspaceOf(const bytes::Bytes& rom)
{
    return bytes::ByteView(rom.data() + MitsuColtCan::kUserspaceStart,
                           MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart);
}

// The top-region slice of `rom` the bootstrap must write and verify.
bytes::ByteView topRegionOf(const bytes::Bytes& rom)
{
    return bytes::ByteView(rom.data() + MitsuColtCan::kTopRegionStart,
                           MitsuColtCan::kTopRegionLength);
}

TEST(MitsuColtM32rCanExecutor, RejectsAPlanFromAnotherFamilyBeforeAnyIo)
{
    // A plan built for another family must be rejected by
    // check_family_transport_match before configure()/open() or any write --
    // the scripted transport is left completely untouched, which is the
    // assertion that matters here.
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // Hand-built rather than produced by a builder: the point is a plan this
    // executor must refuse, and only validate_and_build can make a FlashPlan.
    fastecu::flash::FlashPlanFields fields;
    fields.operation = FlashOperation::Read;
    fields.family = fastecu::flash::FlashFamily::DensoSh705xEepromCan;
    fields.transport = fastecu::flash::TransportKind::CanIso15765;
    fields.target_id = "sub_ecu_denso_sh705x_eeprom_can";
    fields.mcu_name = "SH7058";
    fields.transfer_region = fastecu::flash::MemoryRegion{.start = 0x0, .length = 0x100};
    fields.kernel = fastecu::flash::KernelImage{
        .id = "k", .load_address = 0xffff6004, .bytes = {0x01, 0x02}};
    fields.family_plan = fastecu::flash::DensoSh705xEepromCanPlan{
        .mode = fastecu::flash::EepromReadMode::Mode2,
        .security = fastecu::flash::DensoSecurityVariant::Stock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };
    auto foreign = fastecu::flash::validate_and_build(std::move(fields));
    ASSERT_TRUE(foreign.has_value()) << foreign.error().detail;

    const auto result =
        executor.execute(*foreign, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("does not match this executor"));
    EXPECT_THAT(events.logs, IsEmpty());
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
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
    const auto cases = std::to_array<Case>({
        {"target", "mitsu_ecu_m32r_can_typo", kMcu384, false, {0x8000, 0x58000}, 0x60000},
        {"mcu", kProtocol384, kMcu512, false, {0x8000, 0x58000}, 0x60000},
        {"vendor", kProtocol384, kMcu384, true, {0x8000, 0x58000}, 0x60000},
        {"region", kProtocol384, kMcu384, false, {0x8000, 0x78000}, 0x60000},
        {"image", kProtocol384, kMcu384, false, {0x8000, 0x58000}, 0x80000},
    });

    for (const Case& test : cases)
    {
        ScriptedCanFlashTransport transport;
        FakeClock clock;
        RecordingEventSink events;
        fastecu::flash::CancellationSource cancellation;
        MitsuColtM32rCanExecutor executor;
        const auto plan = handBuiltWritePlan(test.target, test.mcu, test.vendor, test.region,
                                             test.image_size);

        const auto result =
            executor.execute(plan, transport, clock, cancellation.token(), events);

        ASSERT_FALSE(result.has_value()) << test.name;
        EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig) << test.name;
        EXPECT_FALSE(transport.last_config_.has_value()) << test.name;
        EXPECT_EQ(transport.writesConsumed(), 0u) << test.name;
    }
}

TEST(MitsuColtM32rCanExecutor, ReadReturnsEachProtocolCapacityFromAddressZero)
{
    for (const auto [protocol, size] :
         std::to_array<std::pair<std::string_view, std::uint32_t>>({
             {kProtocol384, 0x60000},
             {kProtocol512, 0x80000},
         }))
    {
        ScriptedCanFlashTransport transport;
        FakeClock clock;
        RecordingEventSink events;
        fastecu::flash::CancellationSource cancellation;
        MitsuColtM32rCanExecutor executor;
        const auto plan = readPlan(protocol);
        scriptBootloadHandshake(transport);
        scriptAddressMarkedRead(transport, size);

        const auto result =
            executor.execute(plan, transport, clock, cancellation.token(), events);

        ASSERT_TRUE(result.has_value()) << protocol << ": " << result.error().detail;
        ASSERT_TRUE(result->read_bytes.has_value());
        EXPECT_EQ(result->read_bytes->size(), size);
        EXPECT_EQ(result->read_bytes->front(), 0x00);
        EXPECT_EQ(result->read_bytes->back(), 0xff);
        EXPECT_TRUE(transport.scriptConsumed());
    }
}

TEST(MitsuColtM32rCanExecutor, ReadReportsAnEmptyReplyAsTimeout)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queue_no_frame();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
}

TEST(MitsuColtM32rCanExecutor, ReadPropagatesADisconnectedTransport)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queue_error(ErrorKind::Disconnected, "adapter gone");

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
}

TEST(MitsuColtM32rCanExecutor, ReadStopsWhenCancelled)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));
    cancellation.trip();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(transport.writesConsumed(), 0u);
}

// Trips a cancellation source as soon as the first chunk's progress is
// reported, so what stops the read is the loop's own top-of-chunk
// cancellation check (legacy readFlashRange line 183) rather than a token
// that was already cancelled before execute() was called.
class CancelAfterFirstChunkSink final : public RecordingEventSink
{
  public:
    explicit CancelAfterFirstChunkSink(fastecu::flash::CancellationSource& source)
        : source_(source)
    {
    }
    void phase_progress(const fastecu::PhaseProgressEvent& event) override
    {
        RecordingEventSink::phase_progress(event);
        if (event.phase_name == "Read ROM" && event.done > 0)
        {
            source_.trip();
        }
    }

  private:
    fastecu::flash::CancellationSource& source_;
};

TEST(MitsuColtM32rCanExecutor, ReadStopsAtTheNextChunkWhenCancelledMidRead)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    fastecu::flash::CancellationSource cancellation;
    CancelAfterFirstChunkSink events{cancellation};
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    scriptBootloadHandshake(transport);

    // Exactly one read chunk is scripted; the executor is cancelled while it
    // is being served, so the loop must stop at the top of the next chunk.
    const std::uint32_t start = plan.transfer_region().start;
    transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(
        start, static_cast<bytes::Byte>(MitsuColtCan::kFlashReadBlockSize))));
    bytes::Bytes reply = response({0x63});
    reply.insert(reply.end(), MitsuColtCan::kFlashReadBlockSize, 0x5A);
    transport.queueRead(reply);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
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
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(kVendorProtocol384);

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));

    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    transport.queueRead(response({0x63, 0x27, 0x41, 0x12, 0x34, 0x56, 0x78}));

    const std::uint32_t key = MitsuColtCanVendorExt::challengeInverseTransform(0x12345678);
    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeKey(key)));
    transport.queueRead(response({0x63, 0x27, 0x34}));

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queue_no_frame(); // stop here: ordering is what this pins

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Vendor challenge accepted")));
    // bytes::toHex is lowercase "%02x " per byte, trailing space included.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Received vendor seed: 12 34 56 78 ")));
}

TEST(MitsuColtM32rCanExecutor, VendorChallengeRejectionStopsBeforeTheSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(kVendorProtocol384);

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));

    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    transport.queueRead(response({0x7f, 0x23, 0x33}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:93, with the NRC
    // 0x33 decoded from the untruncated context.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Wrong vendor challenge response from ECU: "
                                               "Security access denied")));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(MitsuColtM32rCanExecutor, ReadEmitsMonotonicProgress)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    scriptBootloadHandshake(transport);

    scriptFullRead(transport, plan, 0x00);

    ASSERT_TRUE(executor.execute(plan, transport, clock, cancellation.token(), events)
                    .has_value());

    ASSERT_FALSE(events.phase_progress_calls.empty());
    EXPECT_THAT(events.phase_progress_calls,
                Each(testing::Field(&fastecu::RecordedPhaseProgress::phase_count, 2)));
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
                       std::views::filter([](const auto& event)
                                          { return event.phase_name == "Read ROM"; });
    int previous = -1;
    for (const auto& event : read_events)
    {
        EXPECT_GE(event.done, previous);
        EXPECT_EQ(event.total, static_cast<int>(plan.transfer_region().length));
        previous = event.done;
    }
    EXPECT_EQ(previous, static_cast<int>(plan.transfer_region().length));
}

TEST(MitsuColtM32rCanExecutor, WriteDrivesTheBootloadSessionThenFactorySecurityAccess)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = writePlan(writeRom());

    // Legacy line 82: a write selects kSessionBootload (0x85), not kSessionBasic.
    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));

    // Legacy lines 136-145: SID 0x27/5, answered with (0x27+0x40, 0x05) and a
    // 4-byte seed at received.mid(6, 4).
    transport.expectWrite(request(MitsuColtCan::buildSecurityAccessSeedRequest()));
    transport.queueRead(response({0x67, 0x05, 0x11, 0x22, 0x33, 0x44}));

    // Legacy lines 151-156: the key is seedKey(seed), and the request carries
    // it verbatim. Scripting the exact bytes means a wrong seed offset or a
    // wrong key derivation is rejected by the transport, not silently accepted.
    transport.expectWrite(
        request(MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(kSeed))));
    transport.queueRead(response({0x67, 0x06}));

    // The handshake is what this test pins, so the run is stopped one request
    // into the write path: the first chunk of the top-region check goes
    // unanswered. That the request is a top-region read at all is the proof
    // the write branch now proceeds instead of refusing.
    transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(
        MitsuColtCan::kTopRegionStart,
        static_cast<bytes::Byte>(MitsuColtCan::kFlashReadBlockSize))));
    transport.queue_no_frame();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Timeout);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, Contains("Writing ROM, please wait..."));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Checking top 128KB (0x60000-0x80000)...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Diagnostic session ok")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Requesting security seed...")));
    // Pins both the mid(6, 4) offset and bytes::toHex's exact format.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Received seed: 11 22 33 44 ")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, HasSubstr("Calculated seed key: "))));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Sending seed key to ECU...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Security access ok")));
}

TEST(MitsuColtM32rCanExecutor, WriteBoundsTheDefaultProtocolTo384KiB)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom384();
    auto plan = writePlan(rom, kProtocol384);

    scriptBootloadHandshake(transport);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    scriptUnlockAndErase(transport);
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom),
                          kUserspaceChecksumBytes);
    scriptFlashReadData(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::Info,
                                  "Checking top 128KB (0x60000-0x80000)..."))));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::Info,
                                  "Uploading erase redirect routine to RAM 0x805568..."))));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::Info,
                                  "Uploading write redirect routine to RAM 0x8054ac..."))));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Writing ROM userspace 0x8000-0x60000...")));
    std::vector<std::string> entered;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.done == 0)
        {
            entered.push_back(event.phase_name);
        }
    }
    EXPECT_THAT(entered, testing::ElementsAre("Connect", "Prepare userspace",
                                              "Erase userspace", "Write userspace",
                                              "Verify userspace"));
}

TEST(MitsuColtM32rCanExecutor, WriteSkipsBootstrapWhenTheTopRegionAlreadyMatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // Top region in the ROM image is all 0xEE, and the ECU reports 0xEE too.
    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    scriptUnlockAndErase(transport);
    // The checksum the commit must carry is stated outright, not recomputed:
    // the scripted frame is [0x36, 0x12, 0x34], so both the running sum and
    // its big-endian split are pinned independently of the implementation.
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom),
                          kUserspaceChecksumBytes);
    scriptFlashReadData(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy-faithful port lifetime: this executor never closes the bus.
    EXPECT_EQ(transport.close_call_count_, 0);
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Top 128KB already matches, no bootstrap needed")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Erase page uploaded")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Write page uploaded")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Userspace flash erased")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Writing ROM userspace 0x8000-0x60000...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Userspace flash written")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Userspace flash verified")));
    EXPECT_THAT(events.phase_progress_calls,
                Each(testing::Field(&fastecu::RecordedPhaseProgress::phase_count, 6)));
    // Nothing from the bootstrap arm ran.
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::Info, "Top 128KB written via redirect"))));
    EXPECT_EQ(result->operation, FlashOperation::Write);
    EXPECT_FALSE(result->read_bytes.has_value());
    std::vector<std::string> entered;
    for (const auto& event : events.phase_progress_calls)
    {
        if (event.done == 0)
        {
            entered.push_back(event.phase_name);
        }
    }
    EXPECT_THAT(entered, testing::ElementsAre("Connect", "Ensure top region",
                                              "Prepare userspace", "Erase userspace",
                                              "Write userspace", "Verify userspace"));
    EXPECT_THAT(events.phase_progress_calls,
                Each(testing::Field(&fastecu::RecordedPhaseProgress::phase_count, 6)));
}

TEST(MitsuColtM32rCanExecutor, WriteRunsTheBootstrapWhenTheTopRegionDiffers)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    // ECU reports 0xFF: mismatch, so the bootstrap runs.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kEraseRedirectRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWriteRedirectRoutine);
    scriptUnlockAndErase(transport);
    // The carrier address is kUserspaceStart, not kTopRegionStart: the redirect
    // routines add the +0x058000 offset themselves.
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, topRegionOf(rom));
    // Verify read-back returns what was written.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    // Then the ordinary write proceeds.
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    scriptUnlockAndErase(transport);
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom));
    scriptFlashReadData(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_TRUE(result.has_value()) << result.error().detail;
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info,
                              "Top 128KB mismatch, bootstrapping via redirect routines...")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info,
                              "Uploading erase redirect routine to RAM 0x805568...")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info,
                              "Uploading write redirect routine to RAM 0x8054ac...")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Carrier window erased")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Top 128KB written via redirect")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Top 128KB verified")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Userspace flash written")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Userspace flash verified")));
}

TEST(MitsuColtM32rCanExecutor, WriteFailsWhenTheUserspaceVerifyMismatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    scriptUnlockAndErase(transport);
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom));
    scriptFlashRead(transport, MitsuColtCan::kUserspaceStart,
                    MitsuColtCan::kUserspaceEnd - MitsuColtCan::kUserspaceStart, 0xFF);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Userspace verify failed after write")));
}

TEST(MitsuColtM32rCanExecutor, WriteFailsWhenTheTopRegionVerifyMismatches)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kEraseRedirectRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWriteRedirectRoutine);
    scriptUnlockAndErase(transport);
    scriptUploadAndCommit(transport, MitsuColtCan::kUserspaceStart, topRegionOf(rom));
    // Verify read-back still reports 0xFF: the write did not take.
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Top 128KB verify failed after redirect write")));
    // The main write never starts.
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Erase page uploaded"))));
}

TEST(MitsuColtM32rCanExecutor, WriteStopsWhenTheReflashUnlockIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7f, 0x3b, 0x33}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // The erase trigger never goes out.
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:451.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "Reflash unlock rejected: Security access denied")));
}

TEST(MitsuColtM32rCanExecutor, WriteStopsWhenTheEraseTriggerIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7b}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineErase()));
    transport.queueRead(response({0x7f, 0x31, 0x22}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // No RequestDownload for the ROM userspace follows a refused erase.
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:461.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "Erase trigger rejected: Conditions not correct")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Userspace flash erased"))));
}

TEST(MitsuColtM32rCanExecutor, RefusesATestWritePlanRatherThanWritingForReal)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // build_mitsu_colt_m32r_can_plan refuses TestWrite, but validate_and_build
    // accepts it -- and a dry run must never reach the erase trigger.
    auto plan = writePlanGranting({fastecu::flash::ConfirmationSpec::Id::EraseTrigger,
                                   fastecu::flash::ConfirmationSpec::Id::TopRegionBootstrap},
                                  writeRom(), FlashOperation::TestWrite);

    scriptBootloadHandshake(transport);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Unsupported);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.notices, Not(Contains("Writing ROM, please wait...")));
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesAnImageThatDoesNotMatchThePlanBeforeAnyIo)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // build_mitsu_colt_m32r_can_plan rejects this image, but validate_and_build
    // does not. The executor must still reject it before it configures or
    // opens the transport, let alone reaches the ECU handshake.
    auto plan = writePlanGranting({fastecu::flash::ConfirmationSpec::Id::EraseTrigger,
                                   fastecu::flash::ConfirmationSpec::Id::TopRegionBootstrap},
                                  bytes::Bytes(MitsuColtCan::kTopRegionEnd - 1, 0x00));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("0x80000"));
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_FALSE(transport.last_config_.has_value());
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesTheBootstrapWhenItsConfirmationIsAbsent)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    // Everything but the bootstrap gate is granted, and the top region does
    // not match -- so the run must stop at the gate rather than firing the
    // redirect routines.
    auto plan = writePlanGranting({fastecu::flash::ConfirmationSpec::Id::EraseTrigger});

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:331.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Info, "Top 128KB bootstrap canceled by user")));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::Info,
                                  "Uploading erase redirect routine to RAM 0x805568..."))));
}

TEST(MitsuColtM32rCanExecutor, HandshakeRejectsAReplyTooShortToHoldAServiceByte)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    // Two bytes: shorter than the 4-byte reply id every frame on this bus
    // starts with, so there is no service byte and no NRC context to decode.
    // The legacy `received.mid(4, ...)` clamped; the portable nrc_context()
    // guards instead, and this is the frame that proves the guard is there --
    // without it the description would be taken from a subspan past the end.
    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(bytes::Bytes{0x07, 0xe8});

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Wrong response from ECU: Not a valid answer")));
}

TEST(MitsuColtM32rCanExecutor, ReadRejectsAChunkAnsweredWithTheWrongService)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    scriptBootloadHandshake(transport);

    // The first chunk comes back as a negative response. Accepting it would
    // append 192 bytes of framing garbage to the ROM image at offset 0.
    const std::uint32_t start = plan.transfer_region().start;
    transport.expectWrite(request(MitsuColtCan::buildReadMemoryByAddress(
        start, static_cast<bytes::Byte>(MitsuColtCan::kFlashReadBlockSize))));
    transport.queueRead(response({0x7f, 0x23, 0x22}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    // Nothing is read after the rejected chunk.
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:198 -- the failing
    // address is part of the message, so a chunk rejected halfway through a
    // 384KB sweep is locatable.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "Wrong response from ECU at 0x0: Conditions not correct")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "ROM read complete"))));
}

// Trips the cancellation source when a chosen log line is emitted, so the
// stop lands between two exchanges of the write path rather than at
// readFlashRange()'s own top-of-chunk checkpoint.
class CancelOnLogSink final : public RecordingEventSink
{
  public:
    CancelOnLogSink(fastecu::flash::CancellationSource& source, std::string trigger)
        : source_(source), trigger_(std::move(trigger))
    {
    }
    void log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::log(level, message);
        if (message == trigger_)
        {
            source_.trip();
        }
    }

  private:
    fastecu::flash::CancellationSource& source_;
    std::string trigger_;
};

TEST(MitsuColtM32rCanExecutor, WriteStopsAtTheNextExchangeWhenCancelledMidWrite)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    fastecu::flash::CancellationSource cancellation;
    // Cancelled the instant the erase-page upload reports success: the run
    // must stop before the write-page upload's first request reaches the bus.
    // The write path has no loop of its own to poll a token, so what has to
    // hold here is exchange()'s own pre-request check.
    CancelOnLogSink events{cancellation, "Erase page uploaded"};
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_EQ(result.error().detail, "cancelled before request");
    // Nothing beyond the erase-page upload was scripted, so this is the
    // assertion that no further request went out.
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Write page uploaded"))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Userspace flash erased"))));
}

// Fails every write outright, the way a yanked adapter does.
class WriteFailingTransport final : public ScriptedCanFlashTransport
{
  public:
    fastecu::Status write(bytes::ByteView, const fastecu::ICancellationToken&) override
    {
        return fastecu::fail(ErrorKind::Disconnected, "adapter write failed");
    }
};

TEST(MitsuColtM32rCanExecutor, AFailedTransportWriteIsReportedWithoutWaitingForAReply)
{
    WriteFailingTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    // A perfectly good session reply is waiting. If the write failure were
    // swallowed, the run would consume it and carry on into the read sweep;
    // the transport's own error is what must come back instead.
    transport.queueRead(response({0x50, 0x85}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "adapter write failed");
    EXPECT_FALSE(transport.scriptConsumed()); // the queued reply was never read
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Diagnostic session ok"))));
}

// Trips the cancellation source from inside write(), so the request does
// reach the bus and the cancellation is first observable at the delay that
// follows it.
class CancelOnWriteTransport final : public ScriptedCanFlashTransport
{
  public:
    explicit CancelOnWriteTransport(fastecu::flash::CancellationSource& source) : source_(source)
    {
    }
    fastecu::Status write(bytes::ByteView data,
                          const fastecu::ICancellationToken& cancellation) override
    {
        fastecu::Status result = ScriptedCanFlashTransport::write(data, cancellation);
        source_.trip();
        return result;
    }

  private:
    fastecu::flash::CancellationSource& source_;
};

TEST(MitsuColtM32rCanExecutor, ACancellationThatArrivesAfterTheRequestStopsBeforeTheReply)
{
    fastecu::flash::CancellationSource cancellation;
    CancelOnWriteTransport transport{cancellation};
    FakeClock clock;
    RecordingEventSink events;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBootload)));
    transport.queueRead(response({0x50, 0x85}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    // The clock's own cancellation carries no detail; the transport's read
    // path would have reported "scripted CAN read cancelled". Distinguishing
    // them is what proves the run stopped at the inter-exchange delay instead
    // of going on to wait out the read timeout.
    EXPECT_EQ(result.error().detail, "");
    EXPECT_FALSE(transport.scriptConsumed()); // the reply was never read
}

TEST(MitsuColtM32rCanExecutor, WriteAbortsWhenTheEraseRoutineRequestDownloadIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    // The bootloader refuses to open the RAM window for the erase-page
    // routine. Continuing would erase flash with whatever happens to be at
    // kEraseRoutineRamAddr.
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        MitsuColtCan::kEraseRoutineRamAddr,
        static_cast<std::uint32_t>(std::size(MitsuColtCan::kErasePageRoutine)))));
    transport.queueRead(response({0x7f, 0x34, 0x33}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:244 and 413.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "RequestDownload to 0x805568 rejected: Security access denied")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Erase-page routine upload failed")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Erase page uploaded"))));
}

TEST(MitsuColtM32rCanExecutor, WriteAbortsWhenTheWriteRoutineTransferDataIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    // The window opens but the payload frame is refused: a half-uploaded
    // write-page routine must never be handed the erase trigger.
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        MitsuColtCan::kWriteRoutineRamAddr,
        static_cast<std::uint32_t>(std::size(MitsuColtCan::kWritePageRoutine)))));
    transport.queueRead(response({0x74}));
    transport.expectWrite(
        request(MitsuColtCan::buildTransferDataFrames(MitsuColtCan::kWritePageRoutine).front()));
    transport.queueRead(response({0x7f, 0x36, 0x31}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:256 and 421.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "TransferData to 0x8054ac rejected: Request out of range")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Write-page routine upload failed")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Userspace flash erased"))));
}

TEST(MitsuColtM32rCanExecutor, WriteFailsWhenTheUserspaceCrcCheckIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);
    scriptUnlockAndErase(transport);
    // Every byte goes out and is acknowledged, and only the ECU's own
    // post-write checksum verification fails. This is the one rejection that
    // says "the flash you just wrote does not match what you sent", so
    // reporting success here would send a user off to flash a bricked ECU.
    scriptUploadFrames(transport, MitsuColtCan::kUserspaceStart, userspaceOf(rom));
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize)));
    transport.queueRead(response({0x74}));
    transport.expectWrite(request(MitsuColtCan::buildTransferDataFrames(
                                      bytes::Bytes{0x12, 0x34})
                                      .front()));
    transport.queueRead(response({0x76}));
    transport.expectWrite(request(MitsuColtCan::buildRoutineCheckCrc(
        MitsuColtCan::kUserspaceStart)));
    transport.queueRead(response({0x7f, 0x31, 0x22}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:290 and 470.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "RoutineControl CRC check for 0x8000 rejected: "
                                               "Conditions not correct")));
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Error, "ROM userspace write failed")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Userspace flash written"))));
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
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    // The erase redirect routine uploads, then the ECU refuses to open the
    // checksum window -- so the routine is in RAM but unverified, and the
    // bootstrap must not go on to erase the carrier window with it.
    scriptUploadFrames(transport, MitsuColtCan::kEraseRoutineRamAddr,
                       MitsuColtCan::kEraseRedirectRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize)));
    transport.queueRead(response({0x7f, 0x34, 0x22}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:266 and 339.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "RequestDownload for checksum rejected: "
                                               "Conditions not correct")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Erase redirect routine upload failed")));
}

TEST(MitsuColtM32rCanExecutor, BootstrapAbortsWhenTheChecksumTransferDataIsRejected)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kEraseRedirectRoutine);
    scriptUploadFrames(transport, MitsuColtCan::kWriteRoutineRamAddr,
                       MitsuColtCan::kWriteRedirectRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestDownload(
        MitsuColtCan::kCrcTransferAddress, MitsuColtCan::kCrcTransferSize)));
    transport.queueRead(response({0x74}));
    {
        const std::uint16_t crc = MitsuColtCan::checksum(MitsuColtCan::kWriteRedirectRoutine);
        const bytes::Bytes crcData{static_cast<bytes::Byte>((crc >> 8) & 0xff),
                                   static_cast<bytes::Byte>(crc & 0xff)};
        transport.expectWrite(request(MitsuColtCan::buildTransferDataFrames(crcData).front()));
    }
    transport.queueRead(response({0x7f, 0x36, 0x22}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:280 and 346.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "TransferData for checksum rejected: "
                                               "Conditions not correct")));
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Write redirect routine upload failed")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Carrier window erased"))));
}

TEST(MitsuColtM32rCanExecutor, BootstrapReportsItsOwnReflashUnlockRejection)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    const bytes::Bytes rom = writeRom();
    auto plan = writePlan(rom);

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xFF);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kEraseRedirectRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWriteRedirectRoutine);
    transport.expectWrite(request(MitsuColtCan::buildRequestReflashUnlock()));
    transport.queueRead(response({0x7f, 0x3b, 0x22}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    // The bootstrap's own message prefix, distinct from the main write's
    // (legacy lines 355 vs 451): the two erase stages are otherwise identical
    // on the wire, so the prefix is the only thing that says which one failed.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error, "Reflash unlock (top 128KB bootstrap) rejected: "
                                               "Conditions not correct")));
    EXPECT_THAT(events.logs,
                Not(Contains(Pair(LogLevel::Error, "Reflash unlock rejected: "
                                                   "Conditions not correct"))));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Carrier window erased"))));
}

TEST(MitsuColtM32rCanExecutor, VendorChallengeKeyRejectionStopsBeforeTheSession)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan(kVendorProtocol384);

    transport.expectWrite(request(MitsuColtCan::buildDiagnosticSession(
        MitsuColtCan::kSessionBasic)));
    transport.queueRead(response({0x50, 0x81}));

    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    transport.queueRead(response({0x63, 0x27, 0x41, 0x12, 0x34, 0x56, 0x78}));

    // Echoing the key subfunction is not the vendor extension's success
    // signal. Only response byte 0x34 grants access, so no session may be
    // started on the strength of this reply.
    const std::uint32_t key = MitsuColtCanVendorExt::challengeInverseTransform(0x12345678);
    transport.expectWrite(request(MitsuColtCanVendorExt::buildChallengeKey(key)));
    transport.queueRead(response({0x63, 0x27, 0x42}));

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:113.
    EXPECT_THAT(events.logs,
                Contains(Pair(LogLevel::Error,
                              "Vendor challenge key rejected: Not a valid answer")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Vendor challenge accepted"))));
}

// An IFlashTransport that is not an ICanFlashTransport -- what a K-Line
// adapter would look like to this executor.
class NotACanTransport final : public fastecu::flash::IFlashTransport
{
  public:
    void request_unblock() noexcept override
    {
    }
};

TEST(MitsuColtM32rCanExecutor, RefusesATransportThatIsNotACanTransport)
{
    NotACanTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    // A checked downcast, not a static_cast: the wrong adapter is a typed
    // refusal rather than undefined behaviour on the first configure().
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(result.error().detail, HasSubstr("does not implement ICanFlashTransport"));
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(MitsuColtM32rCanExecutor, PropagatesAConfigureFailureBeforeAnyRequest)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.configure_result_ = fastecu::fail(ErrorKind::InvalidConfig, "bad bitrate");

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(result.error().detail, "bad bitrate");
    // A bus that could not be configured never gets a request.
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(MitsuColtM32rCanExecutor, PropagatesAnOpenFailureBeforeAnyRequest)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;
    auto plan = readPlan();

    transport.open_result_ = fastecu::fail(ErrorKind::Disconnected, "no adapter");

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "no adapter");
    // Configuration happens first and is not rolled back, but nothing is sent.
    EXPECT_TRUE(transport.last_config_.has_value());
    EXPECT_EQ(transport.writesConsumed(), 0u);
    EXPECT_THAT(events.logs, IsEmpty());
}

TEST(MitsuColtM32rCanExecutor, WriteRefusesTheEraseTriggerWhenItsConfirmationIsAbsent)
{
    ScriptedCanFlashTransport transport;
    FakeClock clock;
    RecordingEventSink events;
    fastecu::flash::CancellationSource cancellation;
    MitsuColtM32rCanExecutor executor;

    auto plan = writePlanGranting({fastecu::flash::ConfirmationSpec::Id::TopRegionBootstrap});

    scriptBootloadHandshake(transport);
    scriptFlashRead(transport, MitsuColtCan::kTopRegionStart, MitsuColtCan::kTopRegionLength,
                    0xEE);
    scriptUploadAndCommit(transport, MitsuColtCan::kEraseRoutineRamAddr,
                          MitsuColtCan::kErasePageRoutine);
    scriptUploadAndCommit(transport, MitsuColtCan::kWriteRoutineRamAddr,
                          MitsuColtCan::kWritePageRoutine);

    const auto result =
        executor.execute(plan, transport, clock, cancellation.token(), events);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    // The reflash-unlock payload is never scripted, so scriptConsumed() here is
    // the assertion that it never reached the bus.
    EXPECT_TRUE(transport.scriptConsumed());
    // Legacy text, flash_ecu_mitsu_m32r_can_operation.cpp:441.
    EXPECT_THAT(events.logs, Contains(Pair(LogLevel::Info, "Erase trigger canceled by user")));
    EXPECT_THAT(events.logs, Not(Contains(Pair(LogLevel::Info, "Userspace flash erased"))));
}

} // namespace
