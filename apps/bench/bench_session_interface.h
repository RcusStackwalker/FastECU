#pragma once
#include <string_view>

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
    virtual Status connect() = 0;

    // One round trip through UdsClient: SID echo checked, NRC 0x78 absorbed.
    // Returns the positive-response PDU, service byte first.
    virtual Result<bytes::Bytes> exchange(bytes::ByteView pdu, const uds::ExchangePolicy& policy) = 0;

    // Bypasses UdsClient's echo and NRC handling; returns whatever arrives.
    virtual Result<bytes::Bytes> exchange_raw(bytes::ByteView pdu, int timeout_ms) = 0;

    virtual Result<double> vbatt() = 0;
};

// File access, kept behind an interface for the same reason as the session.
class IBenchFiles
{
  public:
    virtual ~IBenchFiles() = default;
    virtual Result<bytes::Bytes> load(std::string_view path) = 0;
    virtual Status save(std::string_view path, bytes::ByteView data) = 0;
};

} // namespace fastecu::bench
