#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>

#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/duration_cast.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

namespace fastecu::desktop::detail
{
struct ReadErrors
{
    const char *disconnected_before;
    const char *disconnected_during;
    const char *cancelled;
    const char *exception;
};

inline constexpr ReadErrors kKlineReadErrors{"K-Line adapter disconnected before read",
                                             "K-Line adapter disconnected during read", "K-Line read cancelled",
                                             "K-Line driver read exception"};
inline constexpr ReadErrors kSsmReadErrors{"SSM adapter disconnected before read",
                                           "SSM adapter disconnected during read", "SSM read cancelled",
                                           "SSM driver read exception"};
inline constexpr ReadErrors kCanReadErrors{"CAN adapter disconnected before read",
                                           "CAN adapter disconnected during read", "CAN read cancelled",
                                           "CAN driver read exception"};
inline constexpr ReadErrors kMixedCanReadErrors{"mixed CAN adapter disconnected before read",
                                                "mixed CAN adapter disconnected during read",
                                                "mixed CAN read cancelled", "mixed CAN driver read exception"};

// The caller keeps its pre-call cancellation/unblock, close and mode checks.
// Only cancellation is observed after the driver call: an unblock suppresses
// subsequent I/O and must never discard an already in-flight result.
// The reader explicitly selects ordinary versus raw OBD serial reads.
template <typename Reader>
Result<std::optional<bytes::Bytes>> read_serial(SerialPortActions *serial, std::chrono::milliseconds timeout,
                                                const ICancellationToken& cancellation, const ReadErrors& errors,
                                                Reader reader)
{
    try
    {
        if (!serial || !serial->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, errors.disconnected_before);
        }
        const QByteArray raw = reader(saturating_ms<std::uint16_t>(timeout));
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, errors.cancelled);
        }
        if (!serial->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, errors.disconnected_during);
        }
        if (raw.isEmpty())
        {
            return std::optional<bytes::Bytes>{};
        }
        return std::optional<bytes::Bytes>{bytes::fromQByteArray(raw)};
    }
    catch (const std::exception& error)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, errors.cancelled);
        }
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, errors.cancelled);
        }
        return fail(ErrorKind::Internal, errors.exception);
    }
}
} // namespace fastecu::desktop::detail
