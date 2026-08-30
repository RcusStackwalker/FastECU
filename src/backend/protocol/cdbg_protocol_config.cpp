#include "src/backend/protocol/cdbg_protocol_config.h"

#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"

namespace cdbg
{
bool valid_cdbg_sampling_interval(std::uint32_t interval_ms)
{
    return interval_ms >= 1 && (interval_ms <= 65535 || (interval_ms <= 655350 && interval_ms % 10 == 0));
}

CdbgProtocolConfig::CdbgProtocolConfig(std::uint32_t request_id, std::uint32_t reply_id, std::uint8_t stream_instance,
                                       std::uint32_t sampling_interval_ms)
    : request_id_(request_id), reply_id_(reply_id), stream_instance_(stream_instance),
      sampling_interval_ms_(sampling_interval_ms)
{
}

std::uint32_t CdbgProtocolConfig::request_id() const
{
    return request_id_;
}

std::uint32_t CdbgProtocolConfig::reply_id() const
{
    return reply_id_;
}

std::uint8_t CdbgProtocolConfig::stream_instance() const
{
    return stream_instance_;
}

std::uint32_t CdbgProtocolConfig::sampling_interval_ms() const
{
    return sampling_interval_ms_;
}

fastecu::Result<CdbgProtocolConfig> make_cdbg_protocol_config(std::uint32_t request_id, std::uint32_t reply_id,
                                                              std::uint8_t stream_instance,
                                                              std::uint32_t sampling_interval_ms)
{
    if (request_id > 0x1fffffff)
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "request-id exceeds the 29-bit CAN identifier range");
    }
    if (reply_id > 0x1fffffff)
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "reply-id exceeds the 29-bit CAN identifier range");
    }
    if (request_id == reply_id)
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "reply-id must differ from request-id");
    }
    if (!valid_cdbg_sampling_interval(sampling_interval_ms))
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "sampling-interval-ms is not encodable by the CDBG wire format");
    }
    return CdbgProtocolConfig(request_id, reply_id, stream_instance, sampling_interval_ms);
}

fastecu::Result<CdbgProtocolConfig> make_colt_cdbg_protocol_config()
{
    return make_cdbg_protocol_config(MitsuColtCanCdbg::kRequestCanId, MitsuColtCanCdbg::kReplyCanId, 0, 10);
}
} // namespace cdbg
