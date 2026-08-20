#include "apps/bench/bench_session.h"

#include <optional>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/uds/uds_response.h"

namespace fastecu::bench
{
namespace
{
constexpr uds::ExchangePolicy kConnectPolicy{};
} // namespace

BenchSession::BenchSession(std::unique_ptr<flash::ICanFlashTransport> transport, std::uint32_t request_id,
                           std::uint32_t response_id, IClock& clock, IEventSink& events,
                           const ICancellationToken& cancellation)
    : transport_(std::move(transport)), channel_(*transport_, request_id, response_id),
      client_(channel_, clock, events), cancellation_(cancellation)
{
}

Status BenchSession::connect()
{
    const Result<bytes::Bytes> session_reply = client_.request(
        MitsuColtCan::buildDiagnosticSession(MitsuColtCan::kSessionBootload), kConnectPolicy, cancellation_);
    if (!session_reply.has_value())
    {
        return std::unexpected(session_reply.error());
    }

    const Result<bytes::Bytes> seed_reply =
        client_.request(MitsuColtCan::buildSecurityAccessSeedRequest(), kConnectPolicy, cancellation_);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    const bytes::ByteView seed_payload = uds::payload(*seed_reply);
    if (seed_payload.size() < 5)
    {
        return fail(ErrorKind::BadResponse, "security access seed reply too short");
    }
    const bytes::ByteView seed = seed_payload.subspan(1, 4);

    const Result<bytes::Bytes> key_reply = client_.request(
        MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(seed)), kConnectPolicy, cancellation_);
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    return {};
}

Result<bytes::Bytes> BenchSession::exchange(bytes::ByteView pdu, const uds::ExchangePolicy& policy)
{
    return client_.request(pdu, policy, cancellation_);
}

Result<bytes::Bytes> BenchSession::exchange_raw(bytes::ByteView pdu, int timeout_ms)
{
    const Status sent = channel_.send(pdu, cancellation_);
    if (!sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> received = channel_.receive(timeout_ms, cancellation_);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return fail(ErrorKind::Timeout, "no response within the read timeout");
    }
    return std::move(**received);
}

Result<double> BenchSession::vbatt()
{
    // ICanFlashTransport exposes no read_vbatt(); CommandOutcome::vbatt is
    // optional, so this degrades cleanly rather than widening the transport
    // interface to reach it. See Task 8's bench checklist.
    return fail(ErrorKind::Unsupported, "battery voltage needs the serial layer");
}

} // namespace fastecu::bench
