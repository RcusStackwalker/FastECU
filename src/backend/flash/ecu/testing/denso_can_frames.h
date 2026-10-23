#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"

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

inline constexpr std::array<std::uint16_t, 4> kEncryptTable{0xC85B, 0x32C0, 0xE282, 0x92A0};

// The executor's decrypt table is this encrypt table exactly reversed, and
// SsmProtocol::calculatePayload's Feistel structure inverts by reversing key
// order, so this single helper both (a) pre-encrypts a known plaintext into
// the wire bytes a scripted read reply must carry for the executor's decrypt
// step to recover it, and (b) computes the wire bytes a write must carry for
// a known plaintext image.
inline bytes::Bytes ToWire(bytes::ByteView plain)
{
    return ssm_protocol::CalculatePayload(plain, kEncryptTable, ssm_protocol::kStockPairTable);
}

// The chunked 0xB7 dump sweep (legacy read_memory): 0xB7 plus a 4-byte big-
// endian address, answered with 0xF7 plus one encrypted page.
inline void ScriptFlashDump(ScriptedCanFlashTransport& t, std::uint32_t start, std::uint32_t length,
                            std::uint32_t pagesize, bytes::Byte fill)
{
    const auto section = t.Section("flash dump");
    const bytes::Bytes wire_page = ToWire(bytes::Bytes(pagesize, fill));
    bytes::Bytes reply = Response({0xF7});
    reply.insert(reply.end(), wire_page.begin(), wire_page.end());
    for (std::uint32_t addr = start; addr < start + length; addr += pagesize)
    {
        t.Exchange(Request(bytes::ComposeBe(bytes::Byte(0xB7), addr)), reply);
    }
}

// The 0x37 stop command (legacy read_memory).
inline void ScriptStopCommand(ScriptedCanFlashTransport& t)
{
    const auto section = t.Section("stop command");
    t.Exchange(Request({0x37}), Response({0x77}));
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

} // namespace fastecu::flash::testing
