#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fastecu::logging
{

enum class LoggingProtocolId
{
    kSsm,
    kMutDma,
    kCdbg,
};

enum class NumericStorage
{
    kUnsignedInteger,
    kSignedInteger,
    kFloat,
};

enum class NumericByteOrder
{
    kBigEndian,
    kLittleEndian,
};

struct LoggingChannel
{
    std::string id;
    std::uint32_t address;
    std::size_t length;
    std::string from_byte_expression;
    std::string unit;
    std::uint8_t decimal_precision;
    std::vector<std::uint32_t> byte_addresses;
    std::optional<std::uint8_t> sample_bit;
    NumericStorage storage{NumericStorage::kUnsignedInteger};
    NumericByteOrder byte_order{NumericByteOrder::kBigEndian};
};

struct LoggingPolicy
{
    std::chrono::milliseconds poll_timeout;
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
    double numeric_value{};
    std::string raw_value;
    std::string unit;
};

enum class LoggingState
{
    kRunning,
    kCarNotResponding,
};

} // namespace fastecu::logging
