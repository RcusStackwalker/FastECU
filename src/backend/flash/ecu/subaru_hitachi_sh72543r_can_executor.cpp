#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"
#include "src/backend/flash/transfer_progress.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>

namespace fastecu::flash
{
namespace
{
using namespace bytes::literals;
using namespace std::chrono_literals;
using bytes::Bytes;
// Legacy citations below refer to flash_ecu_subaru_hitachi_sh72543r_can_operation.cpp
// at 5dc86672. Header lines 45-52 define 200/2000/800 ms read timeouts.
constexpr std::array<std::uint16_t, 16> kSeedTable{0x794B, 0x3CAF, 0x3019, 0x8B57, 0x52A0, 0xA77C, 0x38C9, 0xB0B5,
                                                   0x6520, 0x3B66, 0xA09D, 0x2877, 0x479F, 0xB685, 0x7568, 0x84D7};
constexpr std::array<std::uint8_t, 32> kTransform{5,  6, 7, 1, 9,  12, 13, 8, 10, 13, 2, 11, 15, 4,  0,  3,
                                                  11, 4, 6, 0, 15, 2,  13, 9, 5,  12, 1, 10, 3,  13, 14, 8};

bool HasPrefix(bytes::ByteView reply, std::initializer_list<bytes::Byte> prefix)
{
    return reply.size() >= 4 + prefix.size() && std::equal(prefix.begin(), prefix.end(), reply.begin() + 4);
}

// Per-attempt state only: a reused executor never retains read bytes or identity.
class Session
{
  public:
    Session(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancel, IEventSink& events)
        : transport_(transport), clock_(clock), cancel_(cancel), events_(events)
    {
    }

    Status Checkpoint() const
    {
        return cancel_.Cancelled() ? Fail(ErrorKind::kCancelled, "flash cancelled") : Status{};
    }
    Status Sleep(std::chrono::milliseconds delay)
    {
        if (auto s = Checkpoint(); !s.has_value())
        {
            return s;
        }
        if (delay > 0ms)
        {
            if (auto s = clock_.Sleep(delay, cancel_); !s.has_value())
            {
                return s;
            }
        }
        return Checkpoint();
    }
    Result<std::optional<Bytes>> Receive(std::chrono::milliseconds timeout)
    {
        if (auto s = Checkpoint(); !s.has_value())
        {
            return std::unexpected(s.error());
        }
        auto r = transport_.Read(timeout, cancel_);
        if (auto s = Checkpoint(); !s.has_value())
        {
            return std::unexpected(s.error());
        }
        // Both no-frame and explicit Timeout represent silence to optional/retry callers.
        if (!r.has_value())
        {
            if (r.error().kind == ErrorKind::kTimeout)
            {
                return std::optional<Bytes>{};
            }
            return std::unexpected(r.error());
        }
        if (r->has_value() && (**r).empty())
        {
            return std::optional<Bytes>{};
        }
        return r;
    }
    Status Send(bytes::ByteView payload)
    {
        if (auto s = Checkpoint(); !s.has_value())
        {
            return s;
        }
        Bytes frame;
        bytes::AppendU32Be(frame, 0x7e0);
        frame.insert(frame.end(), payload.begin(), payload.end());
        if (auto s = transport_.Write(frame, cancel_); !s.has_value())
        {
            return s;
        }
        return Checkpoint();
    }
    Result<std::optional<Bytes>> Optional(bytes::ByteView payload, std::chrono::milliseconds delay,
                                          std::chrono::milliseconds timeout = 2000ms)
    {
        if (auto s = Send(payload); !s.has_value())
        {
            return std::unexpected(s.error());
        }
        if (auto s = Sleep(delay); !s.has_value())
        {
            return std::unexpected(s.error());
        }
        return Receive(timeout);
    }
    Result<Bytes> Required(bytes::ByteView payload, std::chrono::milliseconds delay,
                           std::initializer_list<bytes::Byte> prefix)
    {
        auto r = Optional(payload, delay);
        if (!r.has_value())
        {
            return std::unexpected(r.error());
        }
        if (!*r)
        {
            return Fail(ErrorKind::kTimeout, "No valid response from ECU");
        }
        if (!HasPrefix(**r, prefix))
        {
            return Fail(ErrorKind::kBadResponse, "Wrong response from ECU");
        }
        return std::move(**r);
    }
    Status TolerantSession(bytes::Byte service)
    {
        // Legacy read_mem:345-365 (10 03), erase_mem:889-918 (10 43).
        auto r = Optional(Bytes{0x10, service}, 200ms);
        if (!r.has_value())
        {
            return std::unexpected(r.error());
        }
        if (!r->has_value() || !HasPrefix(**r, {0x50, service}))
        {
            events_.Log(LogLevel::kError, "No valid response from ECU for session request");
        }
        return {};
    }
    Status Access()
    {
        // Legacy read_mem:372-447 and erase_mem:923-997. Bounds correction:
        // prefix 67 01 alone is insufficient to read the four-byte seed.
        events_.Log(LogLevel::kInfo, "Starting seed request...");
        auto seed = Required(Bytes{0x27, 1}, 200ms, {0x67, 1});
        if (!seed.has_value())
        {
            return std::unexpected(seed.error());
        }
        if (seed->size() < 10)
        {
            return Fail(ErrorKind::kBadResponse, "Seed response is too short");
        }
        events_.Log(LogLevel::kInfo, "Seed request ok");
        Bytes request{0x27, 2};
        const auto key = ssm_protocol::CalculateSeedKey(bytes::ByteView(*seed).subspan(6, 4), kSeedTable, kTransform);
        request.insert(request.end(), key.begin(), key.end());
        events_.Log(LogLevel::kInfo, "Sending seed key...");
        auto r = Required(request, 200ms, {0x67, 2});
        if (!r.has_value())
        {
            return std::unexpected(r.error());
        }
        events_.Log(LogLevel::kInfo, "Seed key ok");
        return {};
    }
    Result<std::optional<std::string>> Connect()
    {
        // Legacy connect_bootloader:103-124. Vendor negative response is its alive marker.
        events_.Log(LogLevel::kInfo, "Checking if OBK is already running...");
        auto alive = Optional(Bytes{0xb7}, 50ms, 200ms);
        if (!alive.has_value())
        {
            return std::unexpected(alive.error());
        }
        if (alive->has_value() && HasPrefix(**alive, {0x7f, 0xb7, 0x13}))
        {
            events_.Log(LogLevel::kInfo, "OBK is active");
            return std::optional<std::string>{};
        }
        events_.Log(LogLevel::kInfo, "OBK not active, initialising ECU...");
        // Legacy 126-166: five ECU-ID bytes start at framed offset 8.
        events_.Log(LogLevel::kInfo, "Requesting ECU ID");
        auto ecu = Optional(Bytes{0xaa}, 50ms);
        if (!ecu.has_value())
        {
            return std::unexpected(ecu.error());
        }
        std::string id;
        if (ecu->has_value() && HasPrefix(**ecu, {0xea}) && (**ecu).size() >= 13)
        {
            for (auto b : bytes::ByteView(**ecu).subspan(8, 5))
            {
                id += std::format("{:02X}", b);
            }
            events_.Log(LogLevel::kInfo, "ECU ID: " + id);
            id += "_";
        }
        else
        {
            events_.Log(LogLevel::kError, "No valid response from ECU");
        }
        // Legacy 170-200, 202-237, 239-274: identity queries remain optional.
        for (bytes::Byte service : {bytes::Byte{2}, bytes::Byte{4}, bytes::Byte{6}})
        {
            events_.Log(LogLevel::kInfo, service == 2   ? "Requesting VIN"
                                         : service == 4 ? "Requesting CAL ID..."
                                                        : "Requesting CVN");
            auto r = Optional(Bytes{9, service}, 50ms);
            if (!r.has_value())
            {
                return std::unexpected(r.error());
            }
            if (!r->has_value() || !HasPrefix(**r, {0x49, service}) || (**r).size() <= 7)
            {
                events_.Log(LogLevel::kError, "No valid response from ECU");
                continue;
            }
            auto data = bytes::ByteView(**r).subspan(7);
            std::string value;
            if (service == 6)
            {
                for (auto b : data)
                {
                    value += std::format("{:02X}", b);
                }
            }
            else
            {
                value.assign(data.begin(), data.end());
            }
            events_.Log(LogLevel::kInfo, (service == 2 ? "VIN: " : service == 4 ? "CAL ID: " : "CVN: ") + value);
            if (service == 4)
            {
                id = value + "_" + id;
            }
        }
        // Legacy 276-310: both A8 replies are intentionally uninterpreted.
        events_.Log(LogLevel::kInfo, "Initializing bootloader...");
        for (auto request : {Bytes{0xa8, 0, 0, 0, 0xd7}, Bytes{0xa8, 0, 0, 1, 0x3b}})
        {
            auto r = Optional(request, 200ms);
            if (!r.has_value())
            {
                return std::unexpected(r.error());
            }
        }
        events_.Log(LogLevel::kInfo, "Test script complete");
        return id.empty() ? std::optional<std::string>{} : std::optional{std::move(id)};
    }
    Result<Bytes> Read(PhaseReporter& progress)
    {
        events_.Log(LogLevel::kInfo, "Settting dump start & length...");
        if (auto s = TolerantSession(3); !s.has_value())
        {
            return std::unexpected(s.error());
        }
        if (auto s = Access(); !s.has_value())
        {
            return std::unexpected(s.error());
        }
        Bytes image;
        image.reserve(0x200000);
        auto last = clock_.Now();
        // Legacy read_mem:449-546: literal 2 MiB sweep, 0x400 bytes per 23 24 request.
        for (std::uint32_t address = 0; address < 0x200000; address += 0x400)
        {
            const Bytes request = bytes::ComposeBe(0x23_b, 0x24_b, address, std::uint16_t{0x400});
            auto r = Required(request, 0ms, {0x63});
            if (!r.has_value())
            {
                return std::unexpected(r.error());
            }
            if (r->size() != 0x405)
            {
                return Fail(ErrorKind::kBadResponse, "Read page must contain exactly 0x400 bytes");
            }
            image.insert(image.end(), r->begin() + 5, r->end());
            const auto now = clock_.Now();
            const auto elapsed =
                std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count());
            const TransferRate rate =
                ComputeTransferRate(0x400, static_cast<std::uint64_t>(elapsed), 0x200000 - address);
            events_.Log(LogLevel::kInfo, FormatReadProgress(address, 0x400, rate));
            last = now;
            progress.Update(static_cast<int>(image.size()));
        }
        // Legacy read_mem:549-580. Stop is nonfatal, up to six requests; any response ends retry.
        events_.Log(LogLevel::kInfo, "ROM read complete");
        events_.Log(LogLevel::kInfo, "Sending stop command...");
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            auto r = Optional(Bytes{0x10, 1}, 200ms, 800ms);
            if (!r.has_value())
            {
                return std::unexpected(r.error());
            }
            if (r->has_value())
            {
                break;
            }
        }
        if (auto s = Checkpoint(); !s.has_value())
        {
            return std::unexpected(s.error());
        }
        progress.Complete();
        return image;
    }

    Status Erase()
    {
        // Legacy erase_mem:889-997: tolerated programming session, required access.
        if (auto s = TolerantSession(0x43); !s.has_value())
        {
            return s;
        }
        if (auto s = Access(); !s.has_value())
        {
            return s;
        }
        // Legacy erase_mem:999-1027.
        events_.Log(LogLevel::kInfo, "Jumping to onboad kernel...");
        auto jump = Required(Bytes{0x10, 0x42}, 200ms, {0x50, 0x42});
        if (!jump.has_value())
        {
            return std::unexpected(jump.error());
        }
        // Legacy erase_mem:1030-1066, exactly the programming window in the flash table.
        events_.Log(LogLevel::kInfo, "Settting flash start & length...");
        auto window = Required(Bytes{0x34, 4, 0x33, 0, 0x60, 0, 0x1f, 0xa0, 0}, 200ms, {0x74, 0x20});
        if (!window.has_value())
        {
            return std::unexpected(window.error());
        }
        // Legacy erase_mem:1069-1123. Correction: inspect the first read too.
        // Send erase once; never retransmit it while polling for completion.
        events_.Log(LogLevel::kInfo, "Erasing ECU ROM...");
        if (auto s = Sleep(100ms); !s.has_value())
        {
            return s;
        }
        if (auto s = Send(Bytes{0x31, 1, 2, 1, 0x0f, 0xff, 0xff, 0xff}); !s.has_value())
        {
            return s;
        }
        bool saw_response = false;
        for (int read = 0; read < 21; ++read)
        {
            auto r = Receive(2000ms);
            if (!r.has_value())
            {
                return std::unexpected(r.error());
            }
            if (r->has_value())
            {
                saw_response = true;
                if (HasPrefix(**r, {0x71, 1, 2}))
                {
                    events_.Log(LogLevel::kInfo, "Flash erased! Starting flash write, do not power off!");
                    return Checkpoint();
                }
            }
            if (read > 0)
            {
                events_.Log(LogLevel::kInfo, ".");
                if (auto s = Sleep(500ms); !s.has_value())
                {
                    return s;
                }
            }
        }
        return Fail(saw_response ? ErrorKind::kBadResponse : ErrorKind::kTimeout, "Flash area erase failed");
    }
    Status Program(bytes::ByteView encrypted, PhaseReporter& progress)
    {
        events_.Log(LogLevel::kInfo, "--- Start writing ROM file to ECU flash memory ---");
        auto last = clock_.Now();
        // Legacy write_mem:599-667 selects only block 1 (0x6000..0x200000).
        // Legacy reflash_block:720-784 constructs B6 with an absolute image offset.
        // Correction: append data instead of indexing beyond QByteArray's size.
        for (std::uint32_t address = 0x6000; address < 0x200000; address += 0x100)
        {
            Bytes request = bytes::ComposeBe(0xb6_b, bytes::U24(address));
            request.insert(request.end(), encrypted.begin() + address, encrypted.begin() + address + 0x100);
            auto r = Optional(request, 10ms);
            if (!r.has_value())
            {
                return std::unexpected(r.error());
            }
            // Legacy never interprets a data reply. Silence remains nonfatal;
            // write errors/disconnect/cancellation must still stop the attempt.
            const auto now = clock_.Now();
            const auto elapsed =
                std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count());
            const TransferRate rate =
                ComputeTransferRate(0x100, static_cast<std::uint64_t>(elapsed), 0x200000 - address - 0x100);
            events_.Log(LogLevel::kInfo,
                        std::format("Kernel write addr: 0x{:08X} length: 0x00000100, {:>6} B/s {:>6} s remain", address,
                                    rate.speed, rate.eta_s));
            last = now;
            progress.Update(static_cast<int>(address + 0x100 - 0x6000));
        }
        return Checkpoint();
    }
    Status Retry(bytes::ByteView request, std::initializer_list<bytes::Byte> prefix, std::chrono::milliseconds timeout,
                 std::string_view failure)
    {
        bool saw_response = false;
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            auto r = Optional(request, 0ms, timeout);
            if (!r.has_value())
            {
                return std::unexpected(r.error());
            }
            if (r->has_value())
            {
                saw_response = true;
                if (HasPrefix(**r, prefix))
                {
                    return Checkpoint();
                }
                events_.Log(LogLevel::kError, "Wrong response from ECU");
            }
            else
            {
                events_.Log(LogLevel::kError, "No valid response from ECU");
            }
        }
        return Fail(saw_response ? ErrorKind::kBadResponse : ErrorKind::kTimeout, std::string(failure));
    }
    Status Finish()
    {
        // Legacy reflash_block:787-829: close may be sent up to 20 times.
        events_.Log(LogLevel::kInfo, "Closing out Flashing of this block...");
        if (auto s = Retry(Bytes{0x37}, {0x77}, 800ms, "Flashing block close failed"); !s.has_value())
        {
            return s;
        }
        events_.Log(LogLevel::kInfo, "Flashing of block closed");
        if (auto s = Sleep(100ms); !s.has_value())
        {
            return s;
        }
        // Legacy reflash_block:831-869: checksum is a second independent retry sequence.
        events_.Log(LogLevel::kInfo, "Verifying checksum...");
        if (auto s = Retry(Bytes{0x31, 1, 2, 2, 1}, {0x71, 1, 2}, 2000ms, "Checksum verification failed");
            !s.has_value())
        {
            return s;
        }
        events_.Log(LogLevel::kInfo, "Checksum verified...");
        return Checkpoint();
    }

  private:
    ICanFlashTransport& transport_;
    IClock& clock_;
    const ICancellationToken& cancel_;
    IEventSink& events_;
};
} // namespace
Result<Iso15765Config> SubaruHitachiSh72543rCanExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (auto valid = ValidateSubaruHitachiSh72543rCanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return Iso15765ConfigFrom(std::get<SubaruHitachiSh72543rCanPlan>(plan.FamilyPlan()));
}
Result<FlashExecutionResult> SubaruHitachiSh72543rCanExecutor::Execute(const FlashPlan& plan,
                                                                       ICanFlashTransport& transport, IClock& clock,
                                                                       const ICancellationToken& cancellation,
                                                                       IEventSink& events)
{
    if (auto valid = ValidateSubaruHitachiSh72543rCanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    Session session(transport, clock, cancellation, events);
    PhaseSequence phases(events, plan.Operation() == FlashOperation::kRead ? 2 : 4);
    auto connecting = phases.Start("Connecting", 1);
    auto identity = session.Connect();
    if (!identity.has_value())
    {
        return std::unexpected(identity.error());
    }
    connecting.Complete();
    if (plan.Operation() == FlashOperation::kWrite)
    {
        // Legacy encrypt_payload:1165-1179. Keep the family's own schedule.
        constexpr std::array<std::uint16_t, 4> kKeys{0xb740, 0x42da, 0xa7ca, 0x5fb1};
        if (auto status = session.Checkpoint(); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        const auto encrypted = ssm_protocol::CalculatePayload(plan.ImageOrEmpty(), 0x200000, kKeys, kTransform);
        auto erasing = phases.Start("Erasing", 1);
        if (auto status = session.Erase(); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        erasing.Complete();
        auto programming = phases.Start("Programming", 0x1fa000);
        if (auto status = session.Program(encrypted, programming); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        programming.Complete();
        auto verifying = phases.Start("Verifying", 1);
        if (auto status = session.Finish(); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        verifying.Complete();
        return FlashExecutionResult{
            .operation = FlashOperation::kWrite, .read_bytes = std::nullopt, .rom_id = std::nullopt};
    }
    auto reading = phases.Start("Reading", 0x200000);
    auto image = session.Read(reading);
    if (!image.has_value())
    {
        return std::unexpected(image.error());
    }
    return FlashExecutionResult{
        .operation = plan.Operation(), .read_bytes = std::move(*image), .rom_id = std::move(*identity)};
}
} // namespace fastecu::flash
