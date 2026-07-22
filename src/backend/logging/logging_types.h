#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class LoggingProtocolId
{
    Ssm,
    MutDma,
    Cdbg,
};

enum class RawAssembly
{
    DecimalBytesConcatenated,
    UnsignedIntegerDecimal,
};

struct LoggingChannel
{
    std::string id;
    std::uint32_t address;
    std::size_t length;
    RawAssembly raw_assembly;
    std::string from_byte_expression;
    std::string unit;
    std::uint8_t decimal_precision;
};

struct LoggingPolicy
{
    int poll_timeout_ms;
    int car_silence_miss_threshold;
    int reconnect_attempt_threshold;
    int reconnect_retry_period;
};

struct ProtocolSample
{
    std::string channel_id;
    std::string raw_value;
};

struct LogSample
{
    std::string channel_id;
    double numeric_value;
    std::string raw_value;
    std::string unit;
};

enum class LoggingState
{
    Running,
    CarNotResponding,
};
