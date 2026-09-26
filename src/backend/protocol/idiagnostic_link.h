#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace fastecu::diagnostics
{

// Which K-Line header the adapter adds to outgoing frames.
enum class KlineHeader
{
    None,
    Ssm,
    Iso9141,
    Iso14230,
};

constexpr std::string_view to_string(KlineHeader header) noexcept
{
    switch (header)
    {
    case KlineHeader::None:
        return "None";
    case KlineHeader::Ssm:
        return "Ssm";
    case KlineHeader::Iso9141:
        return "Iso9141";
    case KlineHeader::Iso14230:
        return "Iso14230";
    }
    return "None";
}

struct KlineLinkConfig
{
    KlineHeader header = KlineHeader::None;
    bool iso14230_connection = false;
    int baud = 10400;
    std::uint8_t start_byte = 0;
    std::uint8_t tester_id = 0;
    std::uint8_t target_id = 0;
};

struct CanLinkConfig
{
    bool iso15765 = true; // false: raw CAN
    int bitrate = 500000;
    bool extended_id = false; // 29-bit identifiers
    std::uint32_t source_id = 0;
    std::uint32_t destination_id = 0;
};

// The diagnostic tools' view of the adapter: byte-faithful and thin. Bytes
// pass through exactly as the adapter expects them -- CAN writes carry their
// 4-byte ID prefix, headers are added only when set_header() asked for one.
class IDiagnosticLink
{
  public:
    using OptionalBytes = std::optional<bytes::Bytes>;

    virtual ~IDiagnosticLink() = default;

    // Reset, apply every field of the config, open. Disconnected if the
    // adapter reports no opened port.
    virtual Status open(const KlineLinkConfig& config) = 0;
    virtual Status open(const CanLinkConfig& config) = 0;
    virtual Status reset() = 0;

    virtual Status set_header(KlineHeader header) = 0;
    virtual Status set_p1_max(std::chrono::milliseconds p1_max) = 0;
    // The raw adapter response, uninterpreted; empty when nothing came back.
    virtual Result<bytes::Bytes> five_baud_init(std::uint8_t address) = 0;
    virtual Status fast_init(bytes::ByteView wakeup) = 0;

    // Echo-checked write; returns what the adapter returned.
    virtual Result<bytes::Bytes> write(bytes::ByteView data) = 0;
    // A deadline is a successful empty optional.
    virtual Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) = 0;
    // The adapter's OBD-framed read (direct serial K-Line).
    virtual Result<OptionalBytes> read_obd(std::chrono::milliseconds timeout,
                                           const ICancellationToken& cancellation) = 0;

    virtual bool uses_j2534() const = 0;
};

} // namespace fastecu::diagnostics
