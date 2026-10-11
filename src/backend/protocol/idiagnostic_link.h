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
    kNone,
    kSsm,
    kIso9141,
    kIso14230,
};

constexpr std::string_view ToString(KlineHeader header) noexcept
{
    switch (header)
    {
    case KlineHeader::kNone:
        return "None";
    case KlineHeader::kSsm:
        return "Ssm";
    case KlineHeader::kIso9141:
        return "Iso9141";
    case KlineHeader::kIso14230:
        return "Iso14230";
    }
    return "None";
}

enum class Parity
{
    kNone,
    kEven,
};

constexpr std::string_view ToString(Parity parity) noexcept
{
    return parity == Parity::kEven ? "Even" : "None";
}

struct KlineLinkConfig
{
    KlineHeader header = KlineHeader::kNone;
    bool iso14230_connection = false;
    int baud = 10400;
    std::uint8_t start_byte = 0;
    std::uint8_t tester_id = 0;
    std::uint8_t target_id = 0;
    Parity parity = Parity::kNone; // SSM1 runs 1953 8E1
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
    virtual Status Open(const KlineLinkConfig& config) = 0;
    virtual Status Open(const CanLinkConfig& config) = 0;
    virtual Status Reset() = 0;

    virtual Status SetHeader(KlineHeader header) = 0;
    virtual Status SetP1Max(std::chrono::milliseconds p1_max) = 0;
    // The raw adapter response, uninterpreted; empty when nothing came back.
    virtual Result<bytes::Bytes> FiveBaudInit(std::uint8_t address) = 0;
    virtual Status FastInit(bytes::ByteView wakeup) = 0;

    // Echo-checked write; returns what the adapter returned.
    virtual Result<bytes::Bytes> Write(bytes::ByteView data) = 0;
    // A deadline is a successful empty optional.
    virtual Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) = 0;
    // The adapter's OBD-framed read (direct serial K-Line).
    virtual Result<OptionalBytes> ReadObd(std::chrono::milliseconds timeout,
                                          const ICancellationToken& cancellation) = 0;

    virtual bool UsesJ2534() const = 0;
};

} // namespace fastecu::diagnostics
