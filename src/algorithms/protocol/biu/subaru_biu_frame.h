#pragma once

#include "src/algorithms/protocol/bytes.h"

namespace biu_subaru
{

// Frame layout: [0x80 | payload length] 0x40 0xF0 <payload...> <checksum>,
// where the checksum is the sum of all preceding bytes modulo 256.

// Sum of the bytes of `data` modulo 256, ignoring the last byte when
// `excludeLast` is set (to validate a received frame's trailing checksum).
bytes::Byte checksum(bytes::ByteView data, bool excludeLast);

// Wraps `payload` (service id and arguments) in a request frame.
bytes::Bytes buildRequest(bytes::ByteView payload);

// The tester-present frame sent every second while connected.
bytes::Bytes keepAliveRequest();

} // namespace biu_subaru
