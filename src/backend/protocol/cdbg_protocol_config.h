#pragma once

#include <cstdint>

#include "src/backend/ports/result.h"

namespace cdbg
{
bool valid_cdbg_sampling_interval(std::uint32_t interval_ms);

class CdbgProtocolConfig
{
  public:
    std::uint32_t request_id() const;
    std::uint32_t reply_id() const;
    std::uint8_t stream_instance() const;
    std::uint32_t sampling_interval_ms() const;

  private:
    CdbgProtocolConfig(std::uint32_t request_id, std::uint32_t reply_id, std::uint8_t stream_instance,
                       std::uint32_t sampling_interval_ms);

    std::uint32_t request_id_;
    std::uint32_t reply_id_;
    std::uint8_t stream_instance_;
    std::uint32_t sampling_interval_ms_;

    friend fastecu::Result<CdbgProtocolConfig> make_cdbg_protocol_config(std::uint32_t, std::uint32_t, std::uint8_t,
                                                                         std::uint32_t);
};

fastecu::Result<CdbgProtocolConfig> make_cdbg_protocol_config(std::uint32_t request_id, std::uint32_t reply_id,
                                                              std::uint8_t stream_instance,
                                                              std::uint32_t sampling_interval_ms);
fastecu::Result<CdbgProtocolConfig> make_colt_cdbg_protocol_config();
} // namespace cdbg
