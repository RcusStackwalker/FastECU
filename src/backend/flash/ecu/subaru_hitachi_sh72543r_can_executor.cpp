#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <algorithm>
#include <array>
#include <format>

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using bytes::Bytes;
// Legacy citations below refer to flash_ecu_subaru_hitachi_sh72543r_can_operation.cpp
// at 5dc86672. Header lines 45-52 define 200/2000/800 ms read timeouts.
constexpr std::array<std::uint16_t, 16> kSeedTable{0x794B, 0x3CAF, 0x3019, 0x8B57, 0x52A0, 0xA77C, 0x38C9, 0xB0B5,
                                                   0x6520, 0x3B66, 0xA09D, 0x2877, 0x479F, 0xB685, 0x7568, 0x84D7};
constexpr std::array<std::uint8_t, 32> kTransform{5,  6, 7, 1, 9,  12, 13, 8, 10, 13, 2, 11, 15, 4,  0,  3,
                                                  11, 4, 6, 0, 15, 2,  13, 9, 5,  12, 1, 10, 3,  13, 14, 8};

bool has_prefix(bytes::ByteView reply, std::initializer_list<bytes::Byte> prefix)
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

    Status checkpoint() const
    {
        return cancel_.cancelled() ? fail(ErrorKind::Cancelled, "flash cancelled") : Status{};
    }
    Status sleep(std::chrono::milliseconds delay)
    {
        if (auto s = checkpoint(); !s)
            return s;
        if (delay > 0ms)
        {
            if (auto s = clock_.sleep(delay, cancel_); !s)
                return s;
        }
        return checkpoint();
    }
    Result<std::optional<Bytes>> receive(std::chrono::milliseconds timeout)
    {
        if (auto s = checkpoint(); !s)
            return std::unexpected(s.error());
        auto r = transport_.read(timeout, cancel_);
        if (auto s = checkpoint(); !s)
            return std::unexpected(s.error());
        // Both no-frame and explicit Timeout represent silence to optional/retry callers.
        if (!r)
        {
            if (r.error().kind == ErrorKind::Timeout)
                return std::optional<Bytes>{};
            return std::unexpected(r.error());
        }
        if (*r && (**r).empty())
            return std::optional<Bytes>{};
        return r;
    }
    Status send(bytes::ByteView payload)
    {
        if (auto s = checkpoint(); !s)
            return s;
        Bytes frame;
        bytes::appendU32Be(frame, 0x7e0);
        frame.insert(frame.end(), payload.begin(), payload.end());
        if (auto s = transport_.write(frame, cancel_); !s)
            return s;
        return checkpoint();
    }
    Result<std::optional<Bytes>> optional(bytes::ByteView payload, std::chrono::milliseconds delay,
                                          std::chrono::milliseconds timeout = 2000ms)
    {
        if (auto s = send(payload); !s)
            return std::unexpected(s.error());
        if (auto s = sleep(delay); !s)
            return std::unexpected(s.error());
        return receive(timeout);
    }
    Result<Bytes> required(bytes::ByteView payload, std::chrono::milliseconds delay,
                           std::initializer_list<bytes::Byte> prefix)
    {
        auto r = optional(payload, delay);
        if (!r)
            return std::unexpected(r.error());
        if (!*r)
            return fail(ErrorKind::Timeout, "No valid response from ECU");
        if (!has_prefix(**r, prefix))
            return fail(ErrorKind::BadResponse, "Wrong response from ECU");
        return std::move(**r);
    }
    Status tolerant_session(bytes::Byte service)
    {
        // Legacy read_mem:345-365 (10 03), erase_mem:889-918 (10 43).
        auto r = optional(Bytes{0x10, service}, 200ms);
        if (!r)
            return std::unexpected(r.error());
        if (!*r || !has_prefix(**r, {0x50, service}))
            events_.log(LogLevel::Error, "No valid response from ECU for session request");
        return {};
    }
    Status access()
    {
        // Legacy read_mem:372-447 and erase_mem:923-997. Bounds correction:
        // prefix 67 01 alone is insufficient to read the four-byte seed.
        events_.log(LogLevel::Info, "Starting seed request...");
        auto seed = required(Bytes{0x27, 1}, 200ms, {0x67, 1});
        if (!seed)
            return std::unexpected(seed.error());
        if (seed->size() < 10)
            return fail(ErrorKind::BadResponse, "Seed response is too short");
        events_.log(LogLevel::Info, "Seed request ok");
        Bytes request{0x27, 2};
        const auto key = SsmProtocol::calculateSeedKey(bytes::ByteView(*seed).subspan(6, 4), kSeedTable, kTransform);
        request.insert(request.end(), key.begin(), key.end());
        events_.log(LogLevel::Info, "Sending seed key...");
        auto r = required(request, 200ms, {0x67, 2});
        if (!r)
            return std::unexpected(r.error());
        events_.log(LogLevel::Info, "Seed key ok");
        return {};
    }
    Result<std::optional<std::string>> connect()
    {
        // Legacy connect_bootloader:103-124. Vendor negative response is its alive marker.
        events_.log(LogLevel::Info, "Checking if OBK is already running...");
        auto alive = optional(Bytes{0xb7}, 50ms, 200ms);
        if (!alive)
            return std::unexpected(alive.error());
        if (*alive && has_prefix(**alive, {0x7f, 0xb7, 0x13}))
        {
            events_.log(LogLevel::Info, "OBK is active");
            return std::optional<std::string>{};
        }
        events_.log(LogLevel::Info, "OBK not active, initialising ECU...");
        // Legacy 126-166: five ECU-ID bytes start at framed offset 8.
        events_.log(LogLevel::Info, "Requesting ECU ID");
        auto ecu = optional(Bytes{0xaa}, 50ms);
        if (!ecu)
            return std::unexpected(ecu.error());
        std::string id;
        if (*ecu && has_prefix(**ecu, {0xea}) && (**ecu).size() >= 13)
        {
            for (auto b : bytes::ByteView(**ecu).subspan(8, 5))
                id += std::format("{:02X}", b);
            events_.log(LogLevel::Info, "ECU ID: " + id);
            id += "_";
        }
        else
            events_.log(LogLevel::Error, "No valid response from ECU");
        // Legacy 170-200, 202-237, 239-274: identity queries remain optional.
        for (bytes::Byte service : {bytes::Byte{2}, bytes::Byte{4}, bytes::Byte{6}})
        {
            events_.log(LogLevel::Info, service == 2   ? "Requesting VIN"
                                        : service == 4 ? "Requesting CAL ID..."
                                                       : "Requesting CVN");
            auto r = optional(Bytes{9, service}, 50ms);
            if (!r)
                return std::unexpected(r.error());
            if (!*r || !has_prefix(**r, {0x49, service}) || (**r).size() <= 7)
            {
                events_.log(LogLevel::Error, "No valid response from ECU");
                continue;
            }
            auto data = bytes::ByteView(**r).subspan(7);
            std::string value;
            if (service == 6)
                for (auto b : data)
                    value += std::format("{:02X}", b);
            else
                value.assign(data.begin(), data.end());
            events_.log(LogLevel::Info, (service == 2 ? "VIN: " : service == 4 ? "CAL ID: " : "CVN: ") + value);
            if (service == 4)
                id = value + "_" + id;
        }
        // Legacy 276-310: both A8 replies are intentionally uninterpreted.
        events_.log(LogLevel::Info, "Initializing bootloader...");
        for (auto request : {Bytes{0xa8, 0, 0, 0, 0xd7}, Bytes{0xa8, 0, 0, 1, 0x3b}})
        {
            auto r = optional(request, 200ms);
            if (!r)
                return std::unexpected(r.error());
        }
        events_.log(LogLevel::Info, "Test script complete");
        return id.empty() ? std::optional<std::string>{} : std::optional{std::move(id)};
    }
    Result<Bytes> read(PhaseReporter& progress)
    {
        events_.log(LogLevel::Info, "Settting dump start & length...");
        if (auto s = tolerant_session(3); !s)
            return std::unexpected(s.error());
        if (auto s = access(); !s)
            return std::unexpected(s.error());
        Bytes image;
        image.reserve(0x200000);
        auto last = clock_.now();
        // Legacy read_mem:449-546: literal 2 MiB sweep, 0x400 bytes per 23 24 request.
        for (std::uint32_t address = 0; address < 0x200000; address += 0x400)
        {
            const Bytes request{0x23,
                                0x24,
                                0,
                                static_cast<bytes::Byte>(address >> 16),
                                static_cast<bytes::Byte>(address >> 8),
                                static_cast<bytes::Byte>(address),
                                4,
                                0};
            auto r = required(request, 0ms, {0x63});
            if (!r)
                return std::unexpected(r.error());
            if (r->size() != 0x405)
                return fail(ErrorKind::BadResponse, "Read page must contain exactly 0x400 bytes");
            image.insert(image.end(), r->begin() + 5, r->end());
            const auto now = clock_.now();
            const auto elapsed =
                std::max<std::int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count());
            const auto speed = std::max<std::int64_t>(1, 0x400 * 1000 / elapsed);
            events_.log(LogLevel::Info,
                        std::format("Kernel read addr:  0x{:08X}  length:  0x00000400,  {:6}  B/s {:6} s", address,
                                    speed, (0x200000 - address) / speed + 1));
            last = now;
            progress.update(static_cast<int>(image.size()));
        }
        // Legacy read_mem:549-580. Stop is nonfatal, up to six requests; any response ends retry.
        events_.log(LogLevel::Info, "ROM read complete");
        events_.log(LogLevel::Info, "Sending stop command...");
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            auto r = optional(Bytes{0x10, 1}, 200ms, 800ms);
            if (!r)
                return std::unexpected(r.error());
            if (*r)
                break;
        }
        if (auto s = checkpoint(); !s)
            return std::unexpected(s.error());
        progress.complete();
        return image;
    }

  private:
    ICanFlashTransport& transport_;
    IClock& clock_;
    const ICancellationToken& cancel_;
    IEventSink& events_;
};
} // namespace
Result<Iso15765Config> SubaruHitachiSh72543rCanExecutor::transport_setup(const FlashPlan& plan) const
{
    if (auto valid = validate_subaru_hitachi_sh72543r_can_plan(plan); !valid)
        return std::unexpected(valid.error());
    return iso15765_config_from(std::get<SubaruHitachiSh72543rCanPlan>(plan.family_plan()));
}
Result<FlashExecutionResult> SubaruHitachiSh72543rCanExecutor::execute(const FlashPlan& plan,
                                                                       ICanFlashTransport& transport, IClock& clock,
                                                                       const ICancellationToken& cancellation,
                                                                       IEventSink& events)
{
    if (auto valid = validate_subaru_hitachi_sh72543r_can_plan(plan); !valid)
        return std::unexpected(valid.error());
    if (plan.operation() != FlashOperation::Read)
        return fail(ErrorKind::Unsupported, "Write execution not yet available");
    Session session(transport, clock, cancellation, events);
    PhaseSequence phases(events, 2);
    auto connecting = phases.start("Connecting", 1);
    auto identity = session.connect();
    if (!identity)
        return std::unexpected(identity.error());
    connecting.complete();
    auto reading = phases.start("Reading", 0x200000);
    auto image = session.read(reading);
    if (!image)
        return std::unexpected(image.error());
    return FlashExecutionResult{
        .operation = plan.operation(), .read_bytes = std::move(*image), .rom_id = std::move(*identity)};
}
} // namespace fastecu::flash
