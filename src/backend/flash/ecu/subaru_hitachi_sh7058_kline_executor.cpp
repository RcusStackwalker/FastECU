#include "src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor.h"

#include <chrono>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using bytes::Bytes;

Result<Bytes> exchange(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancel,
                       bytes::ByteView payload, std::chrono::milliseconds delay = 200ms)
{
    if (cancel.cancelled())
    {
        return fail(ErrorKind::Cancelled, "SH7058 read cancelled");
    }
    const Bytes frame = SsmProtocol::addHeader(payload, 0xf0, 0x10);
    auto written = transport.write(frame);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != frame.size())
    {
        return fail(ErrorKind::Disconnected, "short SH7058 K-Line write");
    }
    if (auto slept = clock.sleep(delay, cancel); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    auto reply = transport.read(delay == 0ms ? 5000ms : 500ms, cancel);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (cancel.cancelled())
    {
        return fail(ErrorKind::Cancelled, "SH7058 read cancelled");
    }
    if (!reply->has_value())
    {
        return fail(ErrorKind::Timeout, "no SH7058 K-Line response");
    }
    return std::move(**reply);
}

bool valid(bytes::ByteView frame, bytes::Byte service, std::size_t payload_size)
{
    return SsmProtocol::hasValidFrame(frame, 0xf0, 0x10) && frame[3] == payload_size && frame[4] == service;
}
} // namespace

Result<KlineConfig> SubaruHitachiSh7058KlineExecutor::transport_setup(const FlashPlan& plan) const
{
    if (auto status = validate_subaru_hitachi_sh7058_plan(plan); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (plan.operation() != FlashOperation::Read)
    {
        return fail(ErrorKind::Unsupported, "SH7058 K-Line supports read only");
    }
    return non_iso14230_kline_config_from(std::get<SubaruHitachiSh7058KlinePlan>(plan.family_plan()));
}

Result<FlashExecutionResult> SubaruHitachiSh7058KlineExecutor::execute(const FlashPlan& plan,
                                                                       IKlineFlashTransport& transport, IClock& clock,
                                                                       const ICancellationToken& cancel,
                                                                       IEventSink& events)
{
    if (auto setup = transport_setup(plan); !setup.has_value())
    {
        return std::unexpected(setup.error());
    }
    if (auto status = transport.set_add_iso14230_header(false); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (auto status = transport.setBaud(38400); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    auto initial = exchange(transport, clock, cancel, Bytes{0xbf});
    if (!initial.has_value() && initial.error().kind != ErrorKind::Timeout)
    {
        return std::unexpected(initial.error());
    }
    std::optional<std::string> rom_id;
    if (!initial.has_value() || !valid(*initial, 0xff, initial->size() >= 5 ? (*initial)[3] : 0))
    {
        if (auto status = transport.setBaud(4800); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        auto identity = exchange(transport, clock, cancel, Bytes{0xbf});
        if (!identity.has_value())
        {
            return std::unexpected(identity.error());
        }
        if (!SsmProtocol::hasValidFrame(*identity, 0xf0, 0x10) || identity->size() < 14 || (*identity)[4] != 0xff)
        {
            return fail(ErrorKind::BadResponse, "invalid SH7058 identity response");
        }
        std::string id = bytes::toHex(bytes::ByteView(*identity).subspan(8, 5), "{:02X}");
        rom_id = id + '_';
        auto switched = exchange(transport, clock, cancel, Bytes{0xb8, 0, 0, 0, 0x75}, 50ms);
        if (!switched.has_value())
        {
            return std::unexpected(switched.error());
        }
        if (!SsmProtocol::hasPayloadPrefix(*switched, Bytes{0xf8}, 0xf0, 0x10))
        {
            return fail(ErrorKind::BadResponse, "SH7058 baud switch rejected");
        }
        if (auto status = transport.setBaud(38400); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        auto resumed = exchange(transport, clock, cancel, Bytes{0xbf});
        if (!resumed.has_value())
        {
            return std::unexpected(resumed.error());
        }
        if (!SsmProtocol::hasPayloadPrefix(*resumed, Bytes{0xff}, 0xf0, 0x10))
        {
            return fail(ErrorKind::BadResponse, "SH7058 connection lost after baud switch");
        }
    }
    Bytes rom;
    rom.reserve(0x100000);
    for (std::uint32_t offset = 0; offset < 0x100000; offset += 0x80)
    {
        const std::uint32_t address = 0x100000 + offset;
        Bytes request = bytes::composeBe(bytes::Byte{0xa0}, address, bytes::Byte{0x7f});
        auto page = exchange(transport, clock, cancel, request, 0ms);
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        if (!valid(*page, 0xe0, 0x81))
        {
            return fail(ErrorKind::BadResponse, "invalid SH7058 ROM page");
        }
        rom.insert(rom.end(), page->begin() + 5, page->begin() + 133);
        events.progress(static_cast<int>(offset + 0x80), 0x100000);
    }
    return FlashExecutionResult{FlashOperation::Read, std::move(rom), std::move(rom_id)};
}
} // namespace fastecu::flash
