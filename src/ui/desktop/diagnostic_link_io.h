#pragma once
#include <QByteArray>

#include <chrono>
#include <cstdint>
#include <tuple>

#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/idiagnostic_link.h"

// QByteArray boundary for the synchronous diagnostic dialogs (BIU,
// DataTerminal), which keep today's facade semantics: a failed or empty read
// is an empty array, and write results are ignored.
namespace diagnostic_link_io
{

inline QByteArray readOrEmpty(fastecu::diagnostics::IDiagnosticLink& link, std::uint16_t timeoutMs)
{
    static const fastecu::ManualCancellationToken kNeverCancelled; // flag is never flipped
    const auto frame = link.Read(std::chrono::milliseconds{timeoutMs}, kNeverCancelled);
    if (!frame.has_value() || !frame->has_value())
    {
        return {};
    }
    return bytes::ToQByteArray(**frame);
}

inline void write(fastecu::diagnostics::IDiagnosticLink& link, const QByteArray& data)
{
    std::ignore = link.Write(bytes::View(data));
}

} // namespace diagnostic_link_io
