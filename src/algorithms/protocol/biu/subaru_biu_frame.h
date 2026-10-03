#pragma once

#include "src/algorithms/protocol/bytes.h"

namespace biu_subaru
{

// Frame layout: [0x80 | payload length] 0x40 0xF0 <payload...> <checksum>,
// where the checksum is bytes::sum8 of all preceding bytes.

// Wraps `payload` (service id and arguments) in a request frame.
bytes::Bytes buildRequest(bytes::ByteView payload);

// The tester-present frame sent every second while connected.
bytes::Bytes keepAliveRequest();

} // namespace biu_subaru
