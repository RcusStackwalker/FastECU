#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_executor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

constexpr std::uint32_t kDiagnosticRequestId = 0x7E0;
// Legacy serial_read_timeout, used by every connect_bootloader() read.
constexpr auto kConnectTimeout = 2000ms;
constexpr auto kShortDelay = 50ms;
constexpr auto kJumpDelay = 200ms;

// Directly compared against legacy generate_seed_key(): the 32-entry index
// transformation is byte-identical to SsmProtocol::kIndexTransformationStock.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0xF2CA, 0x2417, 0x21DE, 0x8475, 0x39AB, 0xF767, 0x6204, 0x6BE0,
    0xBC63, 0x5988, 0x2845, 0x9846, 0xEB97, 0x99DE, 0xC7DB, 0xEFAE,
};

bytes::Bytes framed(bytes::ByteView payload, std::uint32_t request_id)
{
    bytes::Bytes result;
    bytes::appendU32Be(result, request_id);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

Result<std::optional<bytes::Bytes>> exchange_optional(ICanFlashTransport& transport, IClock& clock,
                                                      const ICancellationToken& cancellation, bytes::ByteView payload,
                                                      std::uint32_t request_id,
                                                      std::chrono::milliseconds delay_before_read)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    if (const Status written = transport.write(framed(payload, request_id), cancellation); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after write");
    }
    if (delay_before_read > 0ms)
    {
        if (const Status slept = clock.sleep(delay_before_read, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    Result<std::optional<bytes::Bytes>> response = transport.read(kConnectTimeout, cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after read");
    }
    return std::move(*response);
}

Result<bytes::Bytes> exchange(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              bytes::ByteView payload, std::uint32_t request_id,
                              std::chrono::milliseconds delay_before_read)
{
    Result<std::optional<bytes::Bytes>> response =
        exchange_optional(transport, clock, cancellation, payload, request_id, delay_before_read);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from TCU");
    }
    return std::move(**response);
}

Status expect_prefix(bytes::ByteView response, std::initializer_list<bytes::Byte> prefix)
{
    constexpr std::size_t kCanIdPrefixSize = 4;
    if (response.size() < kCanIdPrefixSize + prefix.size())
    {
        return fail(ErrorKind::BadResponse, "response is too short");
    }
    if (!std::equal(prefix.begin(), prefix.end(), response.begin() + kCanIdPrefixSize))
    {
        return fail(ErrorKind::BadResponse, "wrong response from TCU");
    }
    return {};
}

Result<bytes::Bytes> request_prefix(ICanFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation, bytes::ByteView request,
                                    std::uint32_t request_id, std::chrono::milliseconds delay_before_read,
                                    std::initializer_list<bytes::Byte> expected)
{
    Result<bytes::Bytes> response = exchange(transport, clock, cancellation, request, request_id, delay_before_read);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (const Status matched = expect_prefix(*response, expected); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    return response;
}

Result<std::optional<bytes::Bytes>> non_fatal_prefix(ICanFlashTransport& transport, IClock& clock,
                                                     const ICancellationToken& cancellation, IEventSink& events,
                                                     bytes::ByteView request, std::uint32_t request_id,
                                                     std::chrono::milliseconds delay_before_read,
                                                     std::initializer_list<bytes::Byte> expected,
                                                     std::string_view label)
{
    Result<std::optional<bytes::Bytes>> response =
        exchange_optional(transport, clock, cancellation, request, request_id, delay_before_read);
    if (!response.has_value())
    {
        // A port error or cancellation is not an ECU content mismatch and
        // cannot safely be swallowed by a legacy-tolerance branch.
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        events.log(LogLevel::Error, std::format("No valid response from TCU for {}", label));
        return std::optional<bytes::Bytes>{};
    }
    if (const Status matched = expect_prefix(**response, expected); !matched.has_value())
    {
        events.log(LogLevel::Error, std::format("Wrong response from TCU for {}: {}", label, bytes::toHex(**response)));
        return std::optional<bytes::Bytes>{};
    }
    return std::move(**response);
}

bytes::Bytes seed_key(bytes::ByteView seed)
{
    return SsmProtocol::calculateSeedKey(seed, kSeedKeyTable, SsmProtocol::kIndexTransformationStock);
}

Status connect_bootloader(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                          IEventSink& events, const SubaruTcuHitachiM32rCanPlan& plan)
{
    // Step 1: a matching response means the resident kernel is already live
    // and the remaining seven exchanges must not be sent. A missing or
    // non-matching frame means normal initialization should continue.
    Result<std::optional<bytes::Bytes>> alive =
        exchange_optional(transport, clock, cancellation, bytes::Bytes{0x31, 0x02, 0x02, 0x01}, plan.request_id, 0ms);
    if (!alive.has_value())
    {
        return std::unexpected(alive.error());
    }
    if (alive->has_value())
    {
        if (const Status matched = expect_prefix(**alive, {0x71, 0x02, 0x02, 0x03}); matched.has_value())
        {
            events.log(LogLevel::Info, "Kernel already running");
            return {};
        }
        events.log(LogLevel::Error, std::format("Wrong response from TCU: {}", bytes::toHex(**alive)));
    }
    else
    {
        events.log(LogLevel::Error, "No valid response from TCU");
    }

    // Steps 2 and 3: identity mismatches are diagnostic only in legacy.
    Result<std::optional<bytes::Bytes>> tcu_id =
        non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0xAA}, kDiagnosticRequestId, kShortDelay,
                         {0xEA}, "TCU ID");
    if (!tcu_id.has_value())
    {
        return std::unexpected(tcu_id.error());
    }
    if (tcu_id->has_value())
    {
        constexpr std::size_t kTcuIdOffset = 8;
        constexpr std::size_t kTcuIdSize = 5;
        const bytes::ByteView frame{**tcu_id};
        if (frame.size() < kTcuIdOffset + kTcuIdSize)
        {
            events.log(LogLevel::Error, "TCU ID response is too short");
        }
        else
        {
            std::string decoded;
            decoded.reserve(kTcuIdSize * 2);
            for (const bytes::Byte value : frame.subspan(kTcuIdOffset, kTcuIdSize))
            {
                decoded += std::format("{:02X}", value);
            }
            events.log(LogLevel::Info, std::format("TCU ID: {}", decoded));
        }
    }

    Result<std::optional<bytes::Bytes>> cal_id =
        non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0x09, 0x04}, kDiagnosticRequestId,
                         kShortDelay, {0x49, 0x04}, "CAL ID");
    if (!cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }
    if (cal_id->has_value())
    {
        constexpr std::size_t kCalIdOffset = 7;
        const bytes::ByteView frame{**cal_id};
        if (frame.size() < kCalIdOffset + 1)
        {
            events.log(LogLevel::Error, "CAL ID response is too short");
        }
        else
        {
            const std::string decoded(frame.begin() + static_cast<std::ptrdiff_t>(kCalIdOffset), frame.end());
            events.log(LogLevel::Info, std::format("CAL ID: {}", decoded));
        }
    }

    // Step 4: enter the extended diagnostic session; mismatch is fatal.
    if (Result<bytes::Bytes> session = request_prefix(transport, clock, cancellation, bytes::Bytes{0x10, 0x03},
                                                      kDiagnosticRequestId, kShortDelay, {0x50, 0x03});
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Step 5: request the four-byte seed. The prefix alone only guarantees
    // six framed bytes, so guard the indices 6..9 explicitly.
    Result<bytes::Bytes> seed_response = request_prefix(transport, clock, cancellation, bytes::Bytes{0x27, 0x01},
                                                        kDiagnosticRequestId, kShortDelay, {0x67, 0x01});
    if (!seed_response.has_value())
    {
        return std::unexpected(seed_response.error());
    }
    if (seed_response->size() < 10)
    {
        return fail(ErrorKind::BadResponse, "seed response is too short");
    }

    // Step 6: derive and send the key.
    bytes::Bytes key_request{0x27, 0x02};
    const bytes::Bytes key = seed_key(bytes::ByteView{*seed_response}.subspan(6, 4));
    key_request.insert(key_request.end(), key.begin(), key.end());
    if (Result<bytes::Bytes> key_response = request_prefix(transport, clock, cancellation, key_request,
                                                           kDiagnosticRequestId, kShortDelay, {0x67, 0x02});
        !key_response.has_value())
    {
        return std::unexpected(key_response.error());
    }

    // Step 7: legacy logs a bad jump response and continues (its return is
    // commented out), but preserves the 200 ms delay before the read.
    Result<std::optional<bytes::Bytes>> jump =
        non_fatal_prefix(transport, clock, cancellation, events, bytes::Bytes{0x10, 0x02}, plan.request_id, kJumpDelay,
                         {0x50, 0x02}, "kernel jump");
    if (!jump.has_value())
    {
        return std::unexpected(jump.error());
    }
    if (jump->has_value())
    {
        events.log(LogLevel::Info, std::format("kernel jump response: {}", bytes::toHex(**jump)));
    }

    // Step 8, deliberate divergence 3: build the full four-byte payload in
    // bounds. Legacy reused a six-byte frame and wrote positions 6 and 7
    // beyond its QByteArray, which does not extend under Qt 6.
    if (Result<bytes::Bytes> recheck =
            request_prefix(transport, clock, cancellation, bytes::Bytes{0x31, 0x02, 0x02, 0x01}, plan.request_id, 0ms,
                           {0x71, 0x02, 0x02, 0x03});
        !recheck.has_value())
    {
        return std::unexpected(recheck.error());
    }
    return {};
}

Result<FlashExecutionResult> execute_transfer(const FlashPlan&)
{
    // Task 3 replaces the read arm and Task 4 adds the write arm. Keeping a
    // hard failure here lets Task 2 exercise connect without pretending a
    // transfer succeeded before either transfer path exists.
    return fail(ErrorKind::Internal, "transfer lands in tasks 3 and 4");
}

} // namespace

Result<Iso15765Config> SubaruTcuHitachiM32rCanExecutor::transport_setup(const FlashPlan& plan) const
{
    if (const Status family = check_family(plan, FlashFamily::SubaruTcuHitachiM32rCan); !family.has_value())
    {
        return std::unexpected(family.error());
    }
    if (const Status valid = validate_subaru_tcu_hitachi_m32r_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& parameters = std::get<SubaruTcuHitachiM32rCanPlan>(plan.family_plan());
    return iso15765_config_from(parameters);
}

Result<FlashExecutionResult> SubaruTcuHitachiM32rCanExecutor::execute(const FlashPlan& plan,
                                                                      ICanFlashTransport& transport, IClock& clock,
                                                                      const ICancellationToken& cancellation,
                                                                      IEventSink& events)
{
    if (const Status family = check_family(plan, FlashFamily::SubaruTcuHitachiM32rCan); !family.has_value())
    {
        return std::unexpected(family.error());
    }
    if (const Status valid = validate_subaru_tcu_hitachi_m32r_can_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }
    const auto& parameters = std::get<SubaruTcuHitachiM32rCanPlan>(plan.family_plan());
    if (const Status connected = connect_bootloader(transport, clock, cancellation, events, parameters);
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    return execute_transfer(plan);
}

} // namespace fastecu::flash
