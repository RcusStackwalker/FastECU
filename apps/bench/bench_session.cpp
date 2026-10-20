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

Status ValidateEcho(bytes::ByteView reply, bytes::Byte expected, std::size_t minimum_size, std::string_view subject)
{
    const bytes::ByteView payload = uds::Payload(reply);
    if (payload.size() < minimum_size)
    {
        return Fail(ErrorKind::kBadResponse, std::format("{} reply too short", subject));
    }
    if (payload[0] != expected)
    {
        return Fail(ErrorKind::kBadResponse,
                    std::format("{} echoed 0x{:02x}, expected 0x{:02x}", subject, payload[0], expected));
    }
    return {};
}
Status CheckEcho(const Result<bytes::Bytes>& reply, bytes::Byte expected, std::size_t minimum_size,
                 std::string_view subject)
{
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    return ValidateEcho(*reply, expected, minimum_size, subject);
}

// The vendor challenge: a seed whose inverse transform is the key.
template <typename Request> Status PassVendorChallenge(Request& request)
{
    const Result<bytes::Bytes> vendor_seed_reply = request(mitsu_colt_can_vendor_ext::BuildChallengeSeedRequest());
    if (!vendor_seed_reply.has_value())
    {
        return std::unexpected(vendor_seed_reply.error());
    }
    // [selector][subfunction][4-byte seed].
    const bytes::ByteView vendor_seed_payload = uds::Payload(*vendor_seed_reply);
    if (vendor_seed_payload.size() < 6)
    {
        return Fail(ErrorKind::kBadResponse, "vendor challenge seed reply too short");
    }
    if (vendor_seed_payload[0] != mitsu_colt_can_vendor_ext::kVendorChallengeSelector ||
        vendor_seed_payload[1] != mitsu_colt_can_vendor_ext::kVendorChallengeSeedSubfunction)
    {
        return Fail(ErrorKind::kBadResponse, std::format("vendor challenge seed reply carried 0x{:02x} 0x{:02x}",
                                                         vendor_seed_payload[0], vendor_seed_payload[1]));
    }

    const std::uint32_t vendor_key = mitsu_colt_can_vendor_ext::ChallengeInverseTransform(
        mitsu_colt_can_vendor_ext::BytesToSeed(vendor_seed_payload.subspan(2, 4)));

    const Result<bytes::Bytes> vendor_key_reply = request(mitsu_colt_can_vendor_ext::BuildChallengeKey(vendor_key));
    if (!vendor_key_reply.has_value())
    {
        return std::unexpected(vendor_key_reply.error());
    }
    const bytes::ByteView vendor_key_payload = uds::Payload(*vendor_key_reply);
    if (vendor_key_payload.size() < 2)
    {
        return Fail(ErrorKind::kBadResponse, "vendor challenge key reply too short");
    }
    // Mirrors connect_bootloader's fatal_query prefix check: the reply must
    // carry both the echoed selector and kVendorChallengeAccepted, not just
    // one or the other.
    if (vendor_key_payload[0] != mitsu_colt_can_vendor_ext::kVendorChallengeSelector ||
        vendor_key_payload[1] != mitsu_colt_can_vendor_ext::kVendorChallengeAccepted)
    {
        return Fail(ErrorKind::kBadResponse, std::format("vendor challenge key rejected: reply 0x{:02x} 0x{:02x}",
                                                         vendor_key_payload[0], vendor_key_payload[1]));
    }
    return {};
}
} // namespace

BenchSession::RecordingChannel::RecordingChannel(uds::IUdsChannel& inner) : inner_(inner)
{
}

Status BenchSession::RecordingChannel::Send(bytes::ByteView pdu, const ICancellationToken& cancellation)
{
    return inner_.Send(pdu, cancellation);
}

Result<std::optional<bytes::Bytes>> BenchSession::RecordingChannel::Receive(std::chrono::milliseconds timeout,
                                                                            const ICancellationToken& cancellation)
{
    Result<std::optional<bytes::Bytes>> result = inner_.Receive(timeout, cancellation);
    if (result.has_value() && result->has_value())
    {
        last_rx_ = **result;
    }
    return result;
}

void BenchSession::RecordingChannel::Reset()
{
    last_rx_.clear();
}

const bytes::Bytes& BenchSession::RecordingChannel::LastRx() const
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

Result<bytes::Bytes> BenchSession::RequestOnce(bytes::ByteView pdu, const uds::ExchangePolicy& policy)
{
    recording_channel_.Reset();
    const auto started = clock_.Now();
    Result<bytes::Bytes> result = client_.Request(pdu, policy, cancellation_);
    const auto finished = clock_.Now();
    const bytes::Bytes tx(pdu.begin(), pdu.end());
    last_traffic_ =
        TrafficEvidence{.exchange_count = 1,
                        .tx = tx,
                        .rx = recording_channel_.LastRx(),
                        .last_tx = tx,
                        .last_rx = recording_channel_.LastRx(),
                        .elapsed_ms = static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count())};
    return result;
}

Status BenchSession::Connect()
{
    TrafficEvidence total;
    const auto request = [&](bytes::ByteView pdu) -> Result<bytes::Bytes>
    {
        Result<bytes::Bytes> result = RequestOnce(pdu, kConnectPolicy);
        AppendTraffic(total, last_traffic_);
        last_traffic_ = total;
        return result;
    };
    if (vendor_challenge_)
    {
        if (const Status basic =
                CheckEcho(request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)),
                          mitsu_colt_can::kSessionBasic, 1, "basic diagnostic session");
            !basic.has_value())
        {
            return basic;
        }
        if (const Status passed = PassVendorChallenge(request); !passed.has_value())
        {
            return passed;
        }
    }
    if (const Status session =
            CheckEcho(request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)),
                      mitsu_colt_can::kSessionBootload, 1, "diagnostic session");
        !session.has_value())
    {
        return session;
    }
    const Result<bytes::Bytes> seed_reply = request(mitsu_colt_can::BuildSecurityAccessSeedRequest());
    if (const Status valid = CheckEcho(seed_reply, 0x05, 5, "security access seed"); !valid.has_value())
    {
        return valid;
    }
    const bytes::ByteView seed = uds::Payload(*seed_reply).subspan(1, 4);
    return CheckEcho(request(mitsu_colt_can::BuildSecurityAccessKey(mitsu_colt_can::SeedKey(seed))), 0x06, 1,
                     "security access key");
}

Result<bytes::Bytes> BenchSession::Exchange(bytes::ByteView pdu, const uds::ExchangePolicy& policy)
{
    return RequestOnce(pdu, policy);
}

Result<bytes::Bytes> BenchSession::ExchangeRaw(bytes::ByteView pdu, int timeout_ms)
{
    recording_channel_.Reset();
    const auto started = clock_.Now();
    const auto finish = [&]
    {
        const auto finished = clock_.Now();
        const bytes::Bytes tx(pdu.begin(), pdu.end());
        last_traffic_ =
            TrafficEvidence{.exchange_count = 1,
                            .tx = tx,
                            .rx = recording_channel_.LastRx(),
                            .last_tx = tx,
                            .last_rx = recording_channel_.LastRx(),
                            .elapsed_ms = static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count())};
    };
    if (const Status sent = recording_channel_.Send(pdu, cancellation_); !sent.has_value())
    {
        finish();
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> received =
        recording_channel_.Receive(std::chrono::milliseconds{timeout_ms}, cancellation_);
    finish();
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response within the read timeout");
    }
    return std::move(**received);
}

const TrafficEvidence& BenchSession::LastTraffic() const
{
    return last_traffic_;
}

Result<double> BenchSession::Vbatt()
{
    // ICanFlashTransport exposes no read_vbatt(); CommandOutcome::vbatt is
    // optional, so this degrades cleanly rather than widening the transport
    // interface to reach it. See Task 8's bench checklist.
    return Fail(ErrorKind::kUnsupported, "battery voltage needs the serial layer");
}

} // namespace fastecu::bench
