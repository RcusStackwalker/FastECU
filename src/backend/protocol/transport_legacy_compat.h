#pragma once

#include "src/backend/protocol/ican_transport.h"
#include "src/backend/protocol/ikline_transport.h"
#include "src/backend/protocol/issm_transport.h"

#include <cstdint>
#include <utility>

// Transitional adapters for pre-portability callers. They intentionally
// collapse typed failures back to the old empty/zero shapes and must not be
// used by the portable logging implementations.
namespace fastecu::transport_legacy_compat
{
namespace detail
{
class NeverCancelled final : public ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

inline const ICancellationToken& never_cancelled()
{
    static const NeverCancelled token;
    return token;
}
} // namespace detail

inline int write(ISsmTransport& transport, bytes::ByteView data)
{
    auto result = transport.write(data);
    return result ? static_cast<int>(*result) : 0;
}

inline bytes::Bytes read(ISsmTransport& transport, int timeout_ms)
{
    auto result = transport.read(timeout_ms, detail::never_cancelled());
    if (!result || !result->has_value())
    {
        return {};
    }
    return std::move(result->value());
}

inline bool set_baud(mutdma::IKlineTransport& transport, int baud)
{
    return transport.setBaud(baud).has_value();
}

inline int write(mutdma::IKlineTransport& transport, bytes::ByteView data)
{
    auto result = transport.write(data);
    return result ? static_cast<int>(*result) : 0;
}

inline bytes::Bytes read(mutdma::IKlineTransport& transport, int timeout_ms,
                         [[maybe_unused]] int want_bytes = -1)
{
    auto result = transport.read(timeout_ms, detail::never_cancelled());
    if (!result || !result->has_value())
    {
        return {};
    }
    return std::move(result->value());
}

inline int write(cdbg::ICanTransport& transport, std::uint32_t can_id,
                 bytes::ByteView payload)
{
    auto result = transport.write(can_id, payload);
    return result ? static_cast<int>(*result) : 0;
}

inline bytes::Bytes read(cdbg::ICanTransport& transport, int timeout_ms,
                         std::uint32_t& out_id)
{
    auto result = transport.read(timeout_ms, detail::never_cancelled());
    if (!result || !result->has_value())
    {
        return {};
    }
    out_id = result->value().id;
    return std::move(result->value().payload);
}

} // namespace fastecu::transport_legacy_compat
