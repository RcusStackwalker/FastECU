#include "src/backend/protocol/uds/uds_client.h"

#include <format>
#include <optional>
#include <utility>

#include "src/algorithms/protocol/uds/uds_pdu.h"
#include "src/algorithms/protocol/uds/uds_response.h"

namespace uds
{
namespace
{
using fastecu::ErrorKind;
using fastecu::fail;
using fastecu::LogLevel;
using namespace std::chrono_literals;
} // namespace

UdsClient::UdsClient(IUdsChannel& channel, fastecu::IClock& clock, fastecu::IEventSink& events)
    : channel_(channel), clock_(clock), events_(events)
{
}

fastecu::Result<bytes::Bytes> UdsClient::request(bytes::ByteView pdu, const ExchangePolicy& policy,
                                                 const fastecu::ICancellationToken& cancellation)
{
    if (pdu.empty())
    {
        return fail(ErrorKind::Internal, "UDS request PDU is empty");
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before request");
    }

    const bytes::Byte expected_service = pdu[0];

    if (const fastecu::Status sent = channel_.send(pdu, cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }

    auto delay = policy.pre_read_delay;
    auto timeout = policy.read_timeout;

    // One normal read, then up to max_pending_repeats further reads while the
    // ECU holds us on 0x78.
    for (int attempt = 0; attempt <= policy.max_pending_repeats; ++attempt)
    {
        if (delay > 0ms)
        {
            const fastecu::Status slept = clock_.sleep(delay, cancellation);
            if (!slept.has_value())
            {
                return std::unexpected(slept.error());
            }
        }

        fastecu::Result<std::optional<bytes::Bytes>> received = channel_.receive(timeout, cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            return fail(ErrorKind::Timeout, "no response within the read timeout");
        }

        bytes::Bytes frame = std::move(**received);
        const Response parsed = parseResponse(frame);

        if (parsed.isPending())
        {
            events_.log(LogLevel::Debug,
                        std::format("ECU reported responsePending for SID 0x{:02x}; waiting", expected_service));
            // Only the first read observes the caller's pre-read delay; a
            // pending re-read waits inside the (longer) receive timeout.
            delay = 0ms;
            timeout = policy.pending_timeout;
            continue;
        }

        switch (parsed.kind)
        {
        case ResponseKind::Malformed:
            return fail(ErrorKind::BadResponse, std::format("malformed UDS response: {}", bytes::toHex(frame)));
        case ResponseKind::Negative:
            return fail(ErrorKind::BadResponse, describe(frame));
        case ResponseKind::Positive:
            if (!parsed.matches(expected_service))
            {
                return fail(ErrorKind::BadResponse, std::format("expected response to SID 0x{:02x}, got 0x{:02x}",
                                                                expected_service, parsed.service));
            }
            return frame;
        }
    }

    return fail(ErrorKind::Timeout,
                std::format("ECU still reporting responsePending after {} repeats", policy.max_pending_repeats));
}

} // namespace uds
