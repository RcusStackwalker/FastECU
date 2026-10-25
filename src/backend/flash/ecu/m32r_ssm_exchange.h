#pragma once

#include <chrono>
#include <initializer_list>
#include <optional>

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Shared by Subaru Hitachi M32R K-Line ECU, Subaru Mitsubishi M32R K-Line ECU,
// and Subaru TCU Hitachi M32R K-Line executors. Their ordinary optional SSM
// exchanges have equivalent framing, write-size checks, cancellation checkpoints
// and transport-error precedence. Replies are returned unchanged without frame
// validation; family parsing, pacing and timeout policy remain with callers.
// Composes SendM32rSsmFrame and ReceiveM32rSsmOnce with an additional
// after-write cancellation check before receiving.
Result<std::optional<bytes::Bytes>> ExchangeM32rSsmOptional(IKlineFlashTransport& transport,
                                                            const ICancellationToken& cancellation,
                                                            bytes::ByteView payload, bytes::Byte tester_id,
                                                            bytes::Byte target_id, std::chrono::milliseconds timeout);

// Which module answers an SSM exchange; it only words the failures.
enum class M32rSsmPeer
{
    kEcu,
    kTcu,
};

// The profile of one required-reply conversation: the frame ids and the peer.
struct M32rSsmEndpoint
{
    bytes::Byte tester_id;
    bytes::Byte target_id;
    M32rSsmPeer peer;
};

// A required reply: ExchangeM32rSsmOptional where an absent reply is kTimeout
// "no response from ECU|TCU". A present empty reply is still a reply. Write,
// read and cancellation errors come back exactly as the optional exchange
// reports them. The timeout is explicit because callers differ.
Result<bytes::Bytes> ExchangeM32rSsmRequired(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                             bytes::ByteView payload, const M32rSsmEndpoint& endpoint,
                                             std::chrono::milliseconds timeout);

// kBadResponse "response is too short" when the response has fewer than four
// envelope bytes plus the prefix, else kBadResponse "wrong response from
// ECU|TCU" when the payload does not start with the prefix.
Status ExpectM32rSsmPrefix(bytes::ByteView response, bytes::ByteView prefix, M32rSsmPeer peer);

inline Status ExpectM32rSsmPrefix(bytes::ByteView response, std::initializer_list<bytes::Byte> prefix, M32rSsmPeer peer)
{
    return ExpectM32rSsmPrefix(response, bytes::ByteView(prefix.begin(), prefix.size()), peer);
}

// ExchangeM32rSsmRequired followed by ExpectM32rSsmPrefix; an exchange error
// is returned without looking at the prefix.
Status RequestM32rSsmPrefix(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                            bytes::ByteView payload, const M32rSsmEndpoint& endpoint, bytes::ByteView prefix,
                            std::chrono::milliseconds timeout);

inline Status RequestM32rSsmPrefix(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                   bytes::ByteView payload, const M32rSsmEndpoint& endpoint,
                                   std::initializer_list<bytes::Byte> prefix, std::chrono::milliseconds timeout)
{
    return RequestM32rSsmPrefix(transport, cancellation, payload, endpoint,
                                bytes::ByteView(prefix.begin(), prefix.size()), timeout);
}

// Separate operations shared by Unisia Jecs M32R K-Line and bootmode program
// executors. Send checks cancellation only before writing, frames with AddHeader
// (including its one-byte length wrap), propagates write errors and rejects a
// count unequal to the full frame size. There is no after-write checkpoint.
Status SendM32rSsmFrame(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                        bytes::ByteView payload, bytes::Byte tester_id, bytes::Byte target_id);

// Reads exactly once with the caller's timeout, even if already cancelled.
// Transport errors win over the after-read cancellation check. Optional replies
// (including present empty or malformed bytes) pass through without validation.
Result<std::optional<bytes::Bytes>> ReceiveM32rSsmOnce(IKlineFlashTransport& transport,
                                                       const ICancellationToken& cancellation,
                                                       std::chrono::milliseconds timeout);

// Compares payload bytes at offset four only; does not validate the envelope.
bool M32rSsmPayloadHasPrefix(bytes::ByteView response, bytes::ByteView prefix);

} // namespace fastecu::flash
