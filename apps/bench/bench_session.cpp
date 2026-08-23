#include "apps/bench/bench_session.h"

#include <format>
#include <optional>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/uds/uds_response.h"

namespace fastecu::bench
{
namespace
{
constexpr uds::ExchangePolicy kConnectPolicy{};

void appendTraffic(TrafficEvidence& total, const TrafficEvidence& next)
{
    if (next.exchange_count == 0)
    {
        return;
    }
    if (total.exchange_count == 0)
    {
        total.tx = next.tx;
        total.rx = next.rx;
    }
    total.last_tx = next.last_tx;
    total.last_rx = next.last_rx;
    total.exchange_count += next.exchange_count;
    total.elapsed_ms += next.elapsed_ms;
}

Status validateEcho(bytes::ByteView reply, bytes::Byte expected, std::size_t minimum_size, std::string_view subject)
{
    const bytes::ByteView payload = uds::payload(reply);
    if (payload.size() < minimum_size)
    {
        return fail(ErrorKind::BadResponse, std::format("{} reply too short", subject));
    }
    if (payload[0] != expected)
    {
        return fail(ErrorKind::BadResponse,
                    std::format("{} echoed 0x{:02x}, expected 0x{:02x}", subject, payload[0], expected));
    }
    return {};
}
} // namespace

BenchSession::RecordingChannel::RecordingChannel(uds::IUdsChannel& inner) : inner_(inner)
{
}

Status BenchSession::RecordingChannel::send(bytes::ByteView pdu, const ICancellationToken& cancellation)
{
    return inner_.send(pdu, cancellation);
}

Result<std::optional<bytes::Bytes>> BenchSession::RecordingChannel::receive(int timeout_ms,
                                                                            const ICancellationToken& cancellation)
{
    Result<std::optional<bytes::Bytes>> result = inner_.receive(timeout_ms, cancellation);
    if (result.has_value() && result->has_value())
    {
        last_rx_ = **result;
    }
    return result;
}

void BenchSession::RecordingChannel::reset()
{
    last_rx_.clear();
}

const bytes::Bytes& BenchSession::RecordingChannel::last_rx() const
{
    return last_rx_;
}

BenchSession::BenchSession(std::unique_ptr<flash::ICanFlashTransport> transport, std::uint32_t request_id,
                           std::uint32_t response_id, IClock& clock, IEventSink& events,
                           const ICancellationToken& cancellation, bool vendor_challenge)
    : transport_(std::move(transport)), channel_(*transport_, request_id, response_id), recording_channel_(channel_),
      clock_(clock), client_(recording_channel_, clock, events), cancellation_(cancellation),
      vendor_challenge_(vendor_challenge)
{
}

Result<bytes::Bytes> BenchSession::requestOnce(bytes::ByteView pdu, const uds::ExchangePolicy& policy)
{
    recording_channel_.reset();
    const std::uint64_t started = clock_.now_ms();
    Result<bytes::Bytes> result = client_.request(pdu, policy, cancellation_);
    const std::uint64_t finished = clock_.now_ms();
    const bytes::Bytes tx(pdu.begin(), pdu.end());
    last_traffic_ = TrafficEvidence{.exchange_count = 1,
                                    .tx = tx,
                                    .rx = recording_channel_.last_rx(),
                                    .last_tx = tx,
                                    .last_rx = recording_channel_.last_rx(),
                                    .elapsed_ms = finished - started};
    return result;
}

Status BenchSession::connect()
{
    TrafficEvidence total;
    const auto request = [&](bytes::ByteView pdu) -> Result<bytes::Bytes>
    {
        Result<bytes::Bytes> result = requestOnce(pdu, kConnectPolicy);
        appendTraffic(total, last_traffic_);
        last_traffic_ = total;
        return result;
    };

    if (vendor_challenge_)
    {
        const Result<bytes::Bytes> basic_reply =
            request(MitsuColtCan::buildDiagnosticSession(MitsuColtCan::kSessionBasic));
        if (!basic_reply.has_value())
        {
            return std::unexpected(basic_reply.error());
        }
        if (const Status valid = validateEcho(*basic_reply, MitsuColtCan::kSessionBasic, 1, "basic diagnostic session");
            !valid.has_value())
        {
            return valid;
        }

        const Result<bytes::Bytes> vendor_seed_reply = request(MitsuColtCanVendorExt::buildChallengeSeedRequest());
        if (!vendor_seed_reply.has_value())
        {
            return std::unexpected(vendor_seed_reply.error());
        }
        // [selector][subfunction][4-byte seed].
        const bytes::ByteView vendor_seed_payload = uds::payload(*vendor_seed_reply);
        if (vendor_seed_payload.size() < 6)
        {
            return fail(ErrorKind::BadResponse, "vendor challenge seed reply too short");
        }
        if (vendor_seed_payload[0] != MitsuColtCanVendorExt::kVendorChallengeSelector ||
            vendor_seed_payload[1] != MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction)
        {
            return fail(ErrorKind::BadResponse, std::format("vendor challenge seed reply carried 0x{:02x} 0x{:02x}",
                                                            vendor_seed_payload[0], vendor_seed_payload[1]));
        }

        const std::uint32_t vendor_key = MitsuColtCanVendorExt::challengeInverseTransform(
            MitsuColtCanVendorExt::bytesToSeed(vendor_seed_payload.subspan(2, 4)));

        const Result<bytes::Bytes> vendor_key_reply = request(MitsuColtCanVendorExt::buildChallengeKey(vendor_key));
        if (!vendor_key_reply.has_value())
        {
            return std::unexpected(vendor_key_reply.error());
        }
        const bytes::ByteView vendor_key_payload = uds::payload(*vendor_key_reply);
        if (vendor_key_payload.size() < 2)
        {
            return fail(ErrorKind::BadResponse, "vendor challenge key reply too short");
        }
        // Echoing the selector is not acceptance: only kVendorChallengeAccepted
        // grants the transition, so this stays a content check of its own.
        if (vendor_key_payload[1] != MitsuColtCanVendorExt::kVendorChallengeAccepted)
        {
            return fail(ErrorKind::BadResponse,
                        std::format("vendor challenge key rejected: reply 0x{:02x}", vendor_key_payload[1]));
        }
    }

    const Result<bytes::Bytes> session_reply =
        request(MitsuColtCan::buildDiagnosticSession(MitsuColtCan::kSessionBootload));
    if (!session_reply.has_value())
    {
        return std::unexpected(session_reply.error());
    }
    if (const Status valid = validateEcho(*session_reply, MitsuColtCan::kSessionBootload, 1, "diagnostic session");
        !valid.has_value())
    {
        return valid;
    }

    const Result<bytes::Bytes> seed_reply = request(MitsuColtCan::buildSecurityAccessSeedRequest());
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    if (const Status valid = validateEcho(*seed_reply, 0x05, 5, "security access seed"); !valid.has_value())
    {
        return valid;
    }
    const bytes::ByteView seed_payload = uds::payload(*seed_reply);
    const bytes::ByteView seed = seed_payload.subspan(1, 4);

    const Result<bytes::Bytes> key_reply = request(MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(seed)));
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    if (const Status valid = validateEcho(*key_reply, 0x06, 1, "security access key"); !valid.has_value())
    {
        return valid;
    }
    return {};
}

Result<bytes::Bytes> BenchSession::exchange(bytes::ByteView pdu, const uds::ExchangePolicy& policy)
{
    return requestOnce(pdu, policy);
}

Result<bytes::Bytes> BenchSession::exchange_raw(bytes::ByteView pdu, int timeout_ms)
{
    recording_channel_.reset();
    const std::uint64_t started = clock_.now_ms();
    const auto finish = [&]
    {
        const std::uint64_t finished = clock_.now_ms();
        const bytes::Bytes tx(pdu.begin(), pdu.end());
        last_traffic_ = TrafficEvidence{.exchange_count = 1,
                                        .tx = tx,
                                        .rx = recording_channel_.last_rx(),
                                        .last_tx = tx,
                                        .last_rx = recording_channel_.last_rx(),
                                        .elapsed_ms = finished - started};
    };
    const Status sent = recording_channel_.send(pdu, cancellation_);
    if (!sent.has_value())
    {
        finish();
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> received = recording_channel_.receive(timeout_ms, cancellation_);
    finish();
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

const TrafficEvidence& BenchSession::last_traffic() const
{
    return last_traffic_;
}

Result<double> BenchSession::vbatt()
{
    // ICanFlashTransport exposes no read_vbatt(); CommandOutcome::vbatt is
    // optional, so this degrades cleanly rather than widening the transport
    // interface to reach it. See Task 8's bench checklist.
    return fail(ErrorKind::Unsupported, "battery voltage needs the serial layer");
}

} // namespace fastecu::bench
