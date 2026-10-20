#pragma once
#include <string_view>

#include "apps/bench/bench_types.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/uds/uds_client.h"

namespace fastecu::bench
{

// One ECU session's worth of round trips. An interface so bench_commands --
// PDU construction and reply decoding, the parts worth testing -- runs against
// a fake with no hardware present.
class IBenchSession
{
  public:
    virtual ~IBenchSession() = default;

    // 0x10 diagnostic session plus 0x27 security access.
    virtual Status Connect() = 0;

    // One round trip through UdsClient: SID echo checked, NRC 0x78 absorbed.
    // Returns the positive-response PDU, service byte first.
    virtual Result<bytes::Bytes> Exchange(bytes::ByteView pdu, const uds::ExchangePolicy& policy) = 0;

    // Bypasses UdsClient's echo and NRC handling; returns whatever arrives.
    virtual Result<bytes::Bytes> ExchangeRaw(bytes::ByteView pdu, int timeout_ms) = 0;

    // Evidence from the most recent public operation. `connect()` represents
    // all three handshake exchanges; exchange/exchange_raw represent one.
    virtual const TrafficEvidence& LastTraffic() const = 0;

    virtual Result<double> Vbatt() = 0;
};

// File access, kept behind an interface for the same reason as the session.
class IBenchFiles
{
  public:
    virtual ~IBenchFiles() = default;
    virtual Result<bytes::Bytes> Load(std::string_view path) = 0;
    virtual Status Save(std::string_view path, bytes::ByteView data) = 0;
};

} // namespace fastecu::bench
