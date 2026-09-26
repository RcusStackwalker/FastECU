#pragma once
#include <QByteArray>

#include <chrono>
#include <cstdint>

#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/protocol/idiagnostic_link.h"

// QByteArray boundary for the synchronous diagnostic dialogs (BIU,
// DataTerminal), which keep today's facade semantics: a failed or empty read
// is an empty array, and write results are ignored.
namespace diagnostic_link_io
{

inline QByteArray read_or_empty(fastecu::diagnostics::IDiagnosticLink& link, std::uint16_t timeout_ms)
{
    static const fastecu::ManualCancellationToken never_cancelled; // flag is never flipped
    const auto frame = link.read(std::chrono::milliseconds{timeout_ms}, never_cancelled);
    if (!frame.has_value() || !frame->has_value())
    {
        return {};
    }
    return bytes::toQByteArray(**frame);
}

inline void write(fastecu::diagnostics::IDiagnosticLink& link, const QByteArray& data)
{
    static_cast<void>(link.write(bytes::view(data)));
}

} // namespace diagnostic_link_io
