#include "src/backend/flash/ecu/denso_beef_kline_common.h"

#include <format>
#include <utility>
#include "src/algorithms/protocol/bytes_compose.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
Status CheckCancelled(const ICancellationToken& cancellation, std::string detail)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::move(detail));
    }
    return {};
}

} // namespace

using bytes::ComposeBeWithChecksum;
bytes::Bytes DensoBeefKlineFrame(bytes::Byte opcode, bytes::ByteView payload)
{
    const std::uint16_t len_plus_one = static_cast<std::uint16_t>(payload.size() + 1);
    return ComposeBeWithChecksum(bytes::Sum8, kDensoBeefKlineStartComm, len_plus_one, bytes::Byte(opcode), payload);
}

bytes::Bytes DensoBeefKlineWritePayload(std::uint32_t address, bytes::ByteView chunk)
{
    return bytes::ComposeBe(address, chunk);
}

Status DensoBeefKlineEnableProgrammingVoltage(IKlineFlashTransport& transport, const ICancellationToken& cancellation)
{
    if (Status cancelled = CheckCancelled(cancellation, "cancelled before programming line state");
        !cancelled.has_value())
    {
        return cancelled;
    }
    if (Status line = transport.EnableProgrammingVoltageLine(); !line.has_value())
    {
        return line;
    }
    return CheckCancelled(cancellation, "cancelled after programming line state");
}

bool DensoBeefKlineResponseOk(bytes::ByteView received, bytes::Byte expected_opcode_with_ack)
{
    return received.size() > 5 && bytes::ReadU16Be(received, 0) == kDensoBeefKlineStartComm &&
           received[4] == expected_opcode_with_ack;
}

Result<IKlineFlashTransport::OptionalBytes>
DensoBeefKlineExchangeOptional(IKlineFlashTransport& transport, IClock *clock, const ICancellationToken& cancellation,
                               bytes::ByteView request, std::chrono::milliseconds settle,
                               std::chrono::milliseconds timeout)
{
    if (Status cancelled = CheckCancelled(cancellation, "cancelled before write"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (Status written = DensoBeefKlineWriteAll(transport, request); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (Status cancelled = CheckCancelled(cancellation, "cancelled after write"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (clock != nullptr && settle > 0ms)
    {
        if (Status slept = clock->Sleep(settle, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (Status cancelled = CheckCancelled(cancellation, "cancelled before read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    auto received = transport.Read(timeout, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (Status cancelled = CheckCancelled(cancellation, "cancelled after read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    return std::move(*received);
}

Status DensoBeefKlineWriteAll(IKlineFlashTransport& transport, bytes::ByteView request)
{
    Result<std::size_t> written = transport.Write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return Fail(ErrorKind::kDisconnected, "short K-Line write");
    }
    return {};
}

Status DensoBeefKlineCheckAck(bytes::ByteView response, bytes::Byte opcode, std::string_view phase,
                              std::size_t more_than)
{
    if (response.size() <= more_than || !DensoBeefKlineResponseOk(response, static_cast<bytes::Byte>(opcode | 0x40U)))
    {
        return Fail(ErrorKind::kBadResponse, std::format("Wrong response from ECU during {}", phase));
    }
    return {};
}

Result<bytes::Bytes> DensoBeefKlineExchangeAck(IKlineFlashTransport& transport, IClock *clock,
                                               const ICancellationToken& cancellation, bytes::ByteView request,
                                               std::chrono::milliseconds settle, std::chrono::milliseconds timeout,
                                               bytes::Byte opcode, std::string_view phase, std::size_t more_than)
{
    Result<IKlineFlashTransport::OptionalBytes> received =
        DensoBeefKlineExchangeOptional(transport, clock, cancellation, request, settle, timeout);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, std::format("no response from ECU during {}", phase));
    }
    if (Status ack = DensoBeefKlineCheckAck(**received, opcode, phase, more_than); !ack.has_value())
    {
        return std::unexpected(ack.error());
    }
    return std::move(**received);
}

Result<bytes::Bytes> DensoBeefKlineRequestBlockCrc(IKlineFlashTransport& transport,
                                                   const ICancellationToken& cancellation, const MemoryRegion& block)
{
    const bytes::Bytes request = DensoBeefKlineFrame(
        kDensoKlineOpCrc, bytes::ComposeBe(block.start, bytes::Byte{0x00}, bytes::U24{block.length}));
    return DensoBeefKlineExchangeOptional(transport, nullptr, cancellation, request, 0ms, 3000ms)
        .transform([](IKlineFlashTransport::OptionalBytes reply) { return std::move(reply).value_or(bytes::Bytes{}); });
}
} // namespace fastecu::flash
