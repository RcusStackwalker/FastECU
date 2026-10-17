#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash::testing
{

// Every primary request carries the 4-byte big-endian 0x7E0 envelope; every
// primary response the 0x7E8 reply id (legacy connect_bootloader).
inline bytes::Bytes RequestTo(std::uint32_t id, bytes::ByteView payload)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
inline bytes::Bytes RequestTo(std::uint32_t id, std::initializer_list<bytes::Byte> payload)
{
    return RequestTo(id, bytes::ByteView(payload.begin(), payload.size()));
}
inline bytes::Bytes Request(bytes::ByteView payload)
{
    return RequestTo(0x7e0, payload);
}
inline bytes::Bytes Request(std::initializer_list<bytes::Byte> payload)
{
    return RequestTo(0x7e0, payload);
}
inline bytes::Bytes ResponseFrom(std::uint32_t id, std::initializer_list<bytes::Byte> tail)
{
    return RequestTo(id, tail);
}
inline bytes::Bytes Response(std::initializer_list<bytes::Byte> tail)
{
    return RequestTo(0x7e8, tail);
}

// The six 0x37 close attempts shared by the Denso ISO 15765 write flows:
// attempt 5 is acknowledged only when asked; the earlier ones fail in
// different ways so each retry path is exercised.
inline void ScriptCloseRetries(ScriptedCanFlashTransport& transport, bool acknowledged)
{
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        transport.Exchange(Request({0x37}));
        if (attempt == 5 && acknowledged)
        {
            transport.QueueRead(Response({0x77}));
        }
        else if (attempt == 1)
        {
            transport.QueueError(ErrorKind::kDisconnected, "close ignored");
        }
        else if (attempt == 2)
        {
            transport.QueueRead(Response({0x7F, 0x37, 0x31}));
        }
        else if (attempt == 3)
        {
            transport.QueueError(ErrorKind::kCancelled, "close cancellation ignored");
        }
        else
        {
            transport.QueueNoFrame();
        }
    }
}

// Cancels the moment the executor logs a successful close.
class CancelOnCloseSuccessSink final : public RecordingEventSink
{
  public:
    explicit CancelOnCloseSuccessSink(ManualCancellationToken& token) : token_(token)
    {
    }
    void Log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::Log(level, message);
        if (message == "Closed succesfully: 77 ")
        {
            token_.Cancel();
        }
    }

  private:
    ManualCancellationToken& token_;
};

} // namespace fastecu::flash::testing
