#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/uds/iuds_channel.h"

namespace fastecu::flash
{

// Adds and strips the 4-byte big-endian CAN arbitration id that ISO-15765
// flash traffic carries in front of every UDS PDU.
//
// This is the only place in the CAN flash path that knows the envelope
// exists. Executors above it work in PDUs starting at the service id, which
// is why they no longer carry a `kServiceOffset = 4` constant.
//
// Non-owning: the caller keeps the transport alive for at least this
// channel's lifetime, and remains responsible for configure/open/close.
class CanFlashUdsChannel final : public uds::IUdsChannel
{
  public:
    // Number of envelope bytes in front of every PDU.
    static constexpr std::size_t kEnvelopeSize = 4;

    CanFlashUdsChannel(ICanFlashTransport& transport, std::uint32_t request_id, std::uint32_t response_id);

    Status send(bytes::ByteView pdu, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> receive(int timeout_ms, const ICancellationToken& cancellation) override;

  private:
    ICanFlashTransport& transport_;
    std::uint32_t request_id_;
    std::uint32_t response_id_;
};

} // namespace fastecu::flash
