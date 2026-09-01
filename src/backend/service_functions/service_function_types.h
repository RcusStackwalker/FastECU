#pragma once

#include <cstdint>
#include <variant>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/error.h"

namespace fastecu::service_functions
{

// What the platform must apply to its serial facade before handing this
// session an ISsmTransport. ISsmTransport is a bare byte pipe with no
// configure(), so the configuration travels as data -- the same idea as
// IFlashExecutor::transport_setup returning KlineConfig / Iso15765Config.
struct SsmTransportConfig
{
    enum class Framing
    {
        Iso15765,
        Kline14230,
    };

    Framing framing{Framing::Iso15765};
    int bitrate_or_baud{500000};
    std::uint32_t request_id{0x7e1};  // ISO-15765 only
    std::uint32_t response_id{0x7e9}; // ISO-15765 only
    bytes::Byte tester_id{0x00};      // K-Line only; legacy :151
    bytes::Byte target_id{0x00};      // K-Line only; legacy :152
    bool add_iso14230_header{false};  // sessions self-frame via addHeader

    bool operator==(const SsmTransportConfig&) const = default;
};

enum class OperatorGateId
{
    RelearnStaticSetup,   // legacy :648
    RelearnEngineRunning, // legacy :735
};

enum class GateResponse
{
    Accept,
    Decline,
};

struct TcuParameterReadout
{
    // legacy :611-624 decodes these nine values from response bytes 5..14.
    bytes::Byte input_clutch{};            // 0x16c, byte 5
    bytes::Byte high_low_reverse_clutch{}; // 0x16d, byte 6
    bytes::Byte direct_clutch{};           // 0x16e, byte 7
    bytes::Byte front_brake{};             // 0x16f, byte 8
    std::uint16_t awd_clutch_torque{};     // 0x170/0x171, bytes 9-10
    bytes::Byte forward_brake{};           // 0x1bc, byte 11
    bytes::Byte four_wheel_drive{};        // 0x1bd, byte 12
    bytes::Byte line_pressure{};           // 0x1be, byte 13
    bytes::Byte temperature_basis{};       // 0x1bf, byte 14

    bool operator==(const TcuParameterReadout&) const = default;
};

struct RelearnOutcome
{
    // The poll's terminal condition is unresolved -- see the spec. The bytes
    // are surfaced for the operator and the bench, never interpreted here.
    int polls_performed{0};
    bytes::Bytes last_status_frame;

    bool operator==(const RelearnOutcome&) const = default;
};

struct SetParametersOutcome
{
    int frames_written{0};

    bool operator==(const SetParametersOutcome&) const = default;
};

using ServiceFunctionOutcome = std::variant<TcuParameterReadout, RelearnOutcome, SetParametersOutcome>;

struct GateStep
{
    OperatorGateId id;
};

struct CompletedStep
{
    ServiceFunctionOutcome outcome;
};

struct FailedStep
{
    fastecu::Error error;
};

using ServiceFunctionStep = std::variant<GateStep, CompletedStep, FailedStep>;

} // namespace fastecu::service_functions
