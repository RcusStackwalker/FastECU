#pragma once

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/result.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace fastecu::flash
{

struct WireTranscriptStep
{
    enum class Kind
    {
        kWrite,
        kRead,
        kNoFrame,
        kReadTimeout,
        kSleep,
        kSection
    };

    Kind kind;
    bytes::Bytes frame;                   // kWrite, kRead
    std::chrono::milliseconds duration{}; // kReadTimeout, kSleep
    std::string label;                    // kSection

    bool operator==(const WireTranscriptStep&) const = default;
};

using WireTranscript = std::vector<WireTranscriptStep>;

// Parses the line-oriented transcript text; {image}/{fill} references expand into frames.
Result<WireTranscript> ParseWireTranscript(std::string_view text, bytes::ByteView image);

// Emits one step per line, collapsing long fills and image copies into references.
std::string FormatWireTranscript(const WireTranscript& transcript, bytes::ByteView image);

} // namespace fastecu::flash
