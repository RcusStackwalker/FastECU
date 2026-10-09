#pragma once
#include <string>

namespace fastecu
{

enum class ErrorKind
{
    kInvalidConfig, // invalid configuration or definition
    kTimeout,       // bounded read/operation exceeded its deadline
    kDisconnected,  // adapter/transport not open or dropped
    kBadResponse,   // malformed or negatively-acknowledged ECU response
    kCancelled,     // cooperative cancellation observed
    kUnsupported,   // operation not available for this target
    kInternal,      // invariant violation / unexpected state
};

struct Error
{
    ErrorKind kind{ErrorKind::kInternal};
    std::string detail; // human-readable context; never the sole control signal

    bool operator==(const Error&) const = default;
};

inline const char *ToString(ErrorKind k)
{
    switch (k)
    {
    case ErrorKind::kInvalidConfig:
        return "InvalidConfig";
    case ErrorKind::kTimeout:
        return "Timeout";
    case ErrorKind::kDisconnected:
        return "Disconnected";
    case ErrorKind::kBadResponse:
        return "BadResponse";
    case ErrorKind::kCancelled:
        return "Cancelled";
    case ErrorKind::kUnsupported:
        return "Unsupported";
    case ErrorKind::kInternal:
        return "Internal";
    }
    return "Internal";
}

} // namespace fastecu
