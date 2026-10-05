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
// The caller keeps its pre-call cancellation/unblock, close and mode checks.
// Only cancellation is observed after the driver call: an unblock suppresses
// subsequent I/O and must never discard an already in-flight result.
// The reader explicitly selects ordinary versus raw OBD serial reads.
template <typename Reader>
Result<std::optional<bytes::Bytes>> read_serial(SerialPortActions *serial, std::chrono::milliseconds timeout,
                                                const ICancellationToken& cancellation, Reader reader)
{
    try
    {
        if (!serial || !serial->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "serial adapter disconnected before read");
        }
        const QByteArray raw = reader(saturating_ms<std::uint16_t>(timeout));
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "serial read cancelled");
        }
        if (!serial->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "serial adapter disconnected during read");
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
            return fail(ErrorKind::Cancelled, "serial read cancelled");
        }
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "serial read cancelled");
        }
        return fail(ErrorKind::Internal, "serial driver read exception");
    }
}
} // namespace fastecu::desktop::detail
