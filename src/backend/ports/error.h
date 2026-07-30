#pragma once
#include <string>

namespace fastecu
{

enum class ErrorKind
{
    InvalidConfig, // invalid configuration or definition
    Timeout,       // bounded read/operation exceeded its deadline
    Disconnected,  // adapter/transport not open or dropped
    BadResponse,   // malformed or negatively-acknowledged ECU response
    Cancelled,     // cooperative cancellation observed
    Unsupported,   // operation not available for this target
    Internal,      // invariant violation / unexpected state
};

struct Error
{
    ErrorKind kind;
    std::string detail; // human-readable context; never the sole control signal

    bool operator==(const Error&) const = default;
};

inline const char *to_string(ErrorKind k)
{
    switch (k)
    {
    case ErrorKind::InvalidConfig:
        return "InvalidConfig";
    case ErrorKind::Timeout:
        return "Timeout";
    case ErrorKind::Disconnected:
        return "Disconnected";
    case ErrorKind::BadResponse:
        return "BadResponse";
    case ErrorKind::Cancelled:
        return "Cancelled";
    case ErrorKind::Unsupported:
        return "Unsupported";
    case ErrorKind::Internal:
        return "Internal";
    }
    return "Internal";
}

} // namespace fastecu
