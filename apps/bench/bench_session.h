#pragma once
#include <memory>

#include "apps/bench/bench_session_interface.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/protocol/uds/uds_client.h"

namespace fastecu::bench
{

// Wiring only: owns the transport, channel and client, and performs the two
// connect exchanges. Every decision worth testing lives in bench_commands,
// which is why this class has no branching beyond error propagation.
class BenchSession final : public IBenchSession
{
  public:
    BenchSession(std::unique_ptr<flash::ICanFlashTransport> transport, std::uint32_t request_id,
                 std::uint32_t response_id, IClock& clock, IEventSink& events, const ICancellationToken& cancellation);

    Status connect() override;
    Result<bytes::Bytes> exchange(bytes::ByteView pdu, const uds::ExchangePolicy& policy) override;
    Result<bytes::Bytes> exchange_raw(bytes::ByteView pdu, int timeout_ms) override;
    Result<double> vbatt() override;

  private:
    std::unique_ptr<flash::ICanFlashTransport> transport_;
    flash::CanFlashUdsChannel channel_;
    uds::UdsClient client_;
    const ICancellationToken& cancellation_;
};

} // namespace fastecu::bench
