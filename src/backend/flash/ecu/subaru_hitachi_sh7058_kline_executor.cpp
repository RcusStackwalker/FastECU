#include "src/backend/flash/ecu/subaru_hitachi_sh7058_kline_executor.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_types.h"

#include <chrono>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/ports/cancellation.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using bytes::Bytes;

Result<Bytes> Exchange(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancel,
                       bytes::ByteView payload, std::chrono::milliseconds delay = 200ms)
{
    const Bytes frame = ssm_protocol::AddHeader(payload, 0xf0, 0x10);
    return CheckCancelled(cancel, "SH7058 read cancelled")
        .and_then([&] { return transport.Write(frame); })
        .and_then(
            [&](std::size_t written) -> Status
            {
                if (written != frame.size())
                {
                    return Fail(ErrorKind::kDisconnected, "short SH7058 K-Line write");
                }
                return clock.Sleep(delay, cancel);
            })
        .and_then([&] { return transport.Read(delay == 0ms ? 5000ms : 500ms, cancel); })
        .and_then(
            [&](std::optional<Bytes> reply) -> Result<Bytes>
            {
                if (cancel.Cancelled())
                {
                    return Fail(ErrorKind::kCancelled, "SH7058 read cancelled");
                }
                if (!reply.has_value())
                {
                    return Fail(ErrorKind::kTimeout, "no SH7058 K-Line response");
                }
                return std::move(*reply);
            });
}

bool Valid(bytes::ByteView frame, bytes::Byte service, std::size_t payload_size)
{
    return ssm_protocol::HasValidFrame(frame, 0xf0, 0x10) && frame[3] == payload_size && frame[4] == service;
}

Status ExpectPrefix(const Result<Bytes>& reply, bytes::Byte prefix, std::string_view rejected)
{
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!ssm_protocol::HasPayloadPrefix(*reply, Bytes{prefix}, 0xf0, 0x10))
    {
        return Fail(ErrorKind::kBadResponse, std::string(rejected));
    }
    return {};
}

// No answer at 38400 baud: identify at 4800, ask the ECU to switch, and
// resume at 38400. Returns the ROM id prefix from the identity reply.
Result<std::string> IdentifyAndSwitchBaud(IKlineFlashTransport& transport, IClock& clock,
                                          const ICancellationToken& cancel)
{
    return transport.SetBaud(4800)
        .and_then([&] { return Exchange(transport, clock, cancel, Bytes{0xbf}); })
        .and_then(
            [&](const Bytes& identity) -> Result<std::string>
            {
                if (!ssm_protocol::HasValidFrame(identity, 0xf0, 0x10) || identity.size() < 14 || identity[4] != 0xff)
                {
                    return Fail(ErrorKind::kBadResponse, "invalid SH7058 identity response");
                }
                std::string rom_id = bytes::ToHex(bytes::ByteView(identity).subspan(8, 5), "{:02X}") + '_';
                return ExpectPrefix(Exchange(transport, clock, cancel, Bytes{0xb8, 0, 0, 0, 0x75}, 50ms), 0xf8,
                                    "SH7058 baud switch rejected")
                    .and_then([&] { return transport.SetBaud(38400); })
                    .and_then(
                        [&]
                        {
                            return ExpectPrefix(Exchange(transport, clock, cancel, Bytes{0xbf}), 0xff,
                                                "SH7058 connection lost after baud switch");
                        })
                    .transform([&] { return std::move(rom_id); });
            });
}

// The 1 MiB window above the ROM, in 0x80-byte pages.
Result<Bytes> ReadRom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancel,
                      IEventSink& events)
{
    Bytes rom;
    rom.reserve(0x100000);
    for (std::uint32_t offset = 0; offset < 0x100000; offset += 0x80)
    {
        const std::uint32_t address = 0x100000 + offset;
        auto page =
            Exchange(transport, clock, cancel, bytes::ComposeBe(bytes::Byte{0xa0}, address, bytes::Byte{0x7f}), 0ms);
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        if (!Valid(*page, 0xe0, 0x81))
        {
            return Fail(ErrorKind::kBadResponse, "invalid SH7058 ROM page");
        }
        rom.insert(rom.end(), page->begin() + 5, page->begin() + 133);
        events.Progress(static_cast<int>(offset + 0x80), 0x100000);
    }
    return rom;
}
} // namespace

Result<KlineConfig> SubaruHitachiSh7058KlineExecutor::Setup(const SubaruHitachiSh7058KlinePlan& family) const
{
    return NonIso14230KlineConfigFrom(family);
}

Result<FlashExecutionResult> SubaruHitachiSh7058KlineExecutor::Run(const SubaruHitachiSh7058KlinePlan& /*family*/,
                                                                   const FlashPlan& /*plan*/,
                                                                   IKlineFlashTransport& transport, IClock& clock,
                                                                   const ICancellationToken& cancel, IEventSink& events)
{
    if (const Status configured =
            transport.SetAddIso14230Header(false).and_then([&] { return transport.SetBaud(38400); });
        !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    auto initial = Exchange(transport, clock, cancel, Bytes{0xbf});
    if (!initial.has_value() && initial.error().kind != ErrorKind::kTimeout)
    {
        return std::unexpected(initial.error());
    }
    std::optional<std::string> rom_id;
    if (!initial.has_value() || !Valid(*initial, 0xff, initial->size() >= 5 ? (*initial)[3] : 0))
    {
        auto switched = IdentifyAndSwitchBaud(transport, clock, cancel);
        if (!switched.has_value())
        {
            return std::unexpected(switched.error());
        }
        rom_id = std::move(*switched);
    }
    auto rom = ReadRom(transport, clock, cancel, events);
    if (!rom.has_value())
    {
        return std::unexpected(rom.error());
    }
    return FlashExecutionResult{FlashOperation::kRead, std::move(*rom), std::move(rom_id)};
}
} // namespace fastecu::flash
