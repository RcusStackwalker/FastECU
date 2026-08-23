#pragma once
#include <memory>
#include <optional>

#include "apps/bench/bench_session_interface.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/protocol/uds/uds_client.h"

namespace fastecu::bench
{

// Owns the transport, recording channel and UDS client. Connect mirrors the
// desktop executor's three exchanges and validates its session/security
// echoes; the recording layer preserves traffic evidence on UDS failures.
class BenchSession final : public IBenchSession
{
  public:
    BenchSession(std::unique_ptr<flash::ICanFlashTransport> transport, std::uint32_t request_id,
                 std::uint32_t response_id, IClock& clock, IEventSink& events, const ICancellationToken& cancellation,
                 bool vendor_challenge = false);

    Status connect() override;
    Result<bytes::Bytes> exchange(bytes::ByteView pdu, const uds::ExchangePolicy& policy) override;
    Result<bytes::Bytes> exchange_raw(bytes::ByteView pdu, int timeout_ms) override;
    const TrafficEvidence& last_traffic() const override;
    Result<double> vbatt() override;

  private:
    class RecordingChannel final : public uds::IUdsChannel
    {
      public:
        explicit RecordingChannel(uds::IUdsChannel& inner);
        Status send(bytes::ByteView pdu, const ICancellationToken& cancellation) override;
        Result<std::optional<bytes::Bytes>> receive(int timeout_ms, const ICancellationToken& cancellation) override;
        void reset();
        const bytes::Bytes& last_rx() const;

      private:
        uds::IUdsChannel& inner_;
        bytes::Bytes last_rx_;
    };

    Result<bytes::Bytes> requestOnce(bytes::ByteView pdu, const uds::ExchangePolicy& policy);

    std::unique_ptr<flash::ICanFlashTransport> transport_;
    flash::CanFlashUdsChannel channel_;
    RecordingChannel recording_channel_;
    IClock& clock_;
    uds::UdsClient client_;
    const ICancellationToken& cancellation_;
    TrafficEvidence last_traffic_;
    bool vendor_challenge_;
};

} // namespace fastecu::bench
