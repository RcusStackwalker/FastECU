#pragma once
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_family.h"
#include "src/backend/ports/result.h"

namespace fastecu::flash
{

struct MitsuColtM32rCanPlan
{
    std::uint32_t request_id;  // 0x7e0
    std::uint32_t response_id; // 0x7e8
    int bitrate;               // 500000
    bool extended_id;          // false -- build_request() hardcodes the
                               // 11-bit physical request id
    bool use_vendor_challenge; // selected by the protocol identifier
    bytes::Byte session_id;    // kSessionBootload (0x85) for Read and Write
};

template <> struct FamilyTraits<MitsuColtM32rCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kMitsuColtM32rCan;
    static constexpr TransportKind kTransport = TransportKind::kCanIso15765;
};

template <> struct PlanValidator<MitsuColtM32rCanPlan>
{
    static Status Validate(const FlashPlan& plan);
};

} // namespace fastecu::flash
