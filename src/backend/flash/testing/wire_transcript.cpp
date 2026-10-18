#include "src/backend/flash/testing/wire_transcript.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace fastecu::flash
{
namespace
{

using Kind = WireTranscriptStep::Kind;

constexpr std::size_t kMinReferenceBytes = 32;

Error LineError(std::size_t line, std::string_view what)
{
    return Error{ErrorKind::kInvalidConfig, std::format("line {}: {}", line, what)};
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }
    return text;
}

template <typename T> std::optional<T> ParseNumber(std::string_view text, int base = 10)
{
    T value{};
    const char *begin = std::to_address(text.begin());
    const char *end = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(begin, end, value, base);
    if (ec != std::errc{} || ptr != end || text.empty())
    {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string_view> NextToken(std::string_view& text)
{
    text = Trim(text);
    if (text.empty())
    {
        return std::nullopt;
    }
    if (text.front() == '{')
    {
        const auto close = text.find('}');
        if (close == std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto token = text.substr(0, close + 1);
        text.remove_prefix(close + 1);
        return token;
    }
    const auto space = text.find_first_of(" \t");
    const auto token = text.substr(0, space);
    text.remove_prefix(space == std::string_view::npos ? text.size() : space);
    return token;
}

std::optional<bytes::Byte> ParseHexPair(std::string_view token)
{
    if (token.size() != 2)
    {
        return std::nullopt;
    }
    return ParseNumber<bytes::Byte>(token, 16);
}

std::optional<std::string> ExpandImage(std::string_view spec_text, bytes::ByteView image, bytes::Bytes& frame)
{
    constexpr std::string_view kMalformed = "malformed image reference";
    const auto plus = spec_text.find('+');
    if (!spec_text.starts_with("0x") || plus == std::string_view::npos)
    {
        return std::string(kMalformed);
    }
    const auto offset = ParseNumber<std::size_t>(spec_text.substr(2, plus - 2), 16);
    const auto length = ParseNumber<std::size_t>(spec_text.substr(plus + 1));
    if (!offset || !length)
    {
        return std::string(kMalformed);
    }
    if (*offset > image.size() || *length > image.size() - *offset)
    {
        return "image reference past the end of the image";
    }
    const auto first = image.begin() + static_cast<std::ptrdiff_t>(*offset);
    frame.insert(frame.end(), first, first + static_cast<std::ptrdiff_t>(*length));
    return std::nullopt;
}

std::optional<std::string> ExpandFill(std::string_view args, bytes::Bytes& frame)
{
    const auto value_token = NextToken(args);
    const auto count_token = NextToken(args);
    if (!value_token || !count_token || !args.empty())
    {
        return "malformed fill reference";
    }
    const auto value = ParseHexPair(*value_token);
    const auto count = ParseNumber<std::size_t>(*count_token);
    if (!value || !count)
    {
        return "malformed fill reference";
    }
    frame.insert(frame.end(), *count, *value);
    return std::nullopt;
}

// Appends the expansion of one "{...}" token; returns an error detail on failure.
std::optional<std::string> ExpandReference(std::string_view token, bytes::ByteView image, bytes::Bytes& frame)
{
    auto inner = Trim(token.substr(1, token.size() - 2));
    const auto name = NextToken(inner);
    if (name == "image")
    {
        const auto spec = NextToken(inner);
        if (!spec || !inner.empty())
        {
            return "malformed image reference";
        }
        return ExpandImage(*spec, image, frame);
    }
    if (name == "fill")
    {
        return ExpandFill(inner, frame);
    }
    return "unknown reference";
}

Result<bytes::Bytes> ParseFrame(std::string_view rest, bytes::ByteView image, std::size_t line)
{
    bytes::Bytes frame;
    for (rest = Trim(rest); !rest.empty(); rest = Trim(rest))
    {
        const auto token = NextToken(rest);
        if (!token)
        {
            return std::unexpected(LineError(line, "unterminated reference"));
        }
        if (token->front() == '{')
        {
            if (const auto error = ExpandReference(*token, image, frame))
            {
                return std::unexpected(LineError(line, *error));
            }
            continue;
        }
        const auto byte = ParseHexPair(*token);
        if (!byte)
        {
            return std::unexpected(LineError(line, std::format("bad hex byte '{}'", *token)));
        }
        frame.push_back(*byte);
    }
    return frame;
}

Result<WireTranscriptStep> ParseTiming(std::string_view rest, std::size_t line)
{
    const auto name = NextToken(rest);
    const auto ms_token = NextToken(rest);
    if (!name || !ms_token || !rest.empty())
    {
        return std::unexpected(LineError(line, "malformed timing step"));
    }
    const auto ms = ParseNumber<std::int64_t>(*ms_token);
    if (!ms || *ms < 0)
    {
        return std::unexpected(LineError(line, "bad duration"));
    }
    WireTranscriptStep step{.kind = Kind::kSleep, .duration = std::chrono::milliseconds(*ms)};
    if (*name == "read-timeout")
    {
        step.kind = Kind::kReadTimeout;
    }
    else if (*name != "sleep")
    {
        return std::unexpected(LineError(line, "unknown timing step"));
    }
    return step;
}

Result<WireTranscriptStep> ParseLine(std::string_view body, bytes::ByteView image, std::size_t line)
{
    const char tag = body.front();
    const auto rest = Trim(body.substr(1));
    if (tag == '>' || tag == '<')
    {
        if (tag == '<' && rest == "none")
        {
            return WireTranscriptStep{.kind = Kind::kNoFrame};
        }
        auto frame = ParseFrame(rest, image, line);
        if (!frame)
        {
            return std::unexpected(frame.error());
        }
        return WireTranscriptStep{.kind = tag == '>' ? Kind::kWrite : Kind::kRead, .frame = std::move(*frame)};
    }
    if (tag == '@')
    {
        return ParseTiming(rest, line);
    }
    constexpr std::string_view kSection = "section";
    if (tag == '#' && rest.starts_with(kSection) && (rest.size() == kSection.size() || rest[kSection.size()] == ' '))
    {
        return WireTranscriptStep{.kind = Kind::kSection, .label = std::string(Trim(rest.substr(kSection.size())))};
    }
    return std::unexpected(LineError(line, "unrecognized line"));
}

std::string HexByte(bytes::Byte byte)
{
    return std::format("{:02x}", byte);
}

// Longest-suffix search state, so sequential frames reference sequential image offsets.
class ReferenceFormatter
{
  public:
    explicit ReferenceFormatter(bytes::ByteView image) : image_(image)
    {
    }

    std::string FormatFrame(bytes::ByteView frame)
    {
        std::string out;
        std::size_t pos = 0;
        while (pos < frame.size())
        {
            const std::size_t run = RunLength(frame, pos);
            if (run >= kMinReferenceBytes)
            {
                Append(out, std::format("{{fill {} {}}}", HexByte(frame[pos]), run));
                pos += run;
                continue;
            }
            std::size_t segment_end = pos;
            while (segment_end < frame.size() && RunLength(frame, segment_end) < kMinReferenceBytes)
            {
                ++segment_end;
            }
            FormatSegment(out, frame.subspan(pos, segment_end - pos), pos == 0);
            pos = segment_end;
        }
        return out;
    }

  private:
    static std::size_t RunLength(bytes::ByteView frame, std::size_t pos)
    {
        std::size_t end = pos;
        while (end < frame.size() && frame[end] == frame[pos])
        {
            ++end;
        }
        return end - pos;
    }

    static void Append(std::string& out, std::string_view token)
    {
        if (!out.empty())
        {
            out += ' ';
        }
        out += token;
    }

    // Image offset of `needle`, preferring the first match at or after next_offset_.
    std::optional<std::size_t> Find(bytes::ByteView needle) const
    {
        const std::boyer_moore_horspool_searcher searcher(needle.begin(), needle.end());
        const auto from = image_.begin() + static_cast<std::ptrdiff_t>(std::min(next_offset_, image_.size()));
        auto hit = std::search(from, image_.end(), searcher);
        if (hit == image_.end())
        {
            hit = std::search(image_.begin(), image_.end(), searcher);
        }
        if (hit == image_.end())
        {
            return std::nullopt;
        }
        return static_cast<std::size_t>(hit - image_.begin());
    }

    void FormatSegment(std::string& out, bytes::ByteView segment, bool frame_start)
    {
        // A suffix that occurs in the image stays true for every shorter suffix, so bisect the start.
        const std::size_t min_start = frame_start ? 1 : 0;
        std::size_t best_start = segment.size();
        std::size_t best_offset = 0;
        if (segment.size() >= kMinReferenceBytes + min_start)
        {
            std::size_t low = min_start;
            std::size_t high = segment.size() - kMinReferenceBytes;
            while (low <= high)
            {
                const std::size_t mid = low + (high - low) / 2;
                if (const auto offset = Find(segment.subspan(mid)))
                {
                    best_start = mid;
                    best_offset = *offset;
                    if (mid == 0)
                    {
                        break;
                    }
                    high = mid - 1;
                }
                else
                {
                    low = mid + 1;
                }
            }
        }
        for (std::size_t i = 0; i < best_start; ++i)
        {
            Append(out, HexByte(segment[i]));
        }
        if (best_start < segment.size())
        {
            const std::size_t length = segment.size() - best_start;
            Append(out, std::format("{{image 0x{:x}+{}}}", best_offset, length));
            next_offset_ = best_offset + length;
        }
    }

    bytes::ByteView image_;
    std::size_t next_offset_ = 0;
};

} // namespace

Result<WireTranscript> ParseWireTranscript(std::string_view text, bytes::ByteView image)
{
    WireTranscript transcript;
    std::size_t line_number = 0;
    while (!text.empty())
    {
        const auto newline = text.find('\n');
        const auto raw = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        ++line_number;
        const auto body = Trim(raw);
        if (body.empty() || body.starts_with("##"))
        {
            continue;
        }
        auto step = ParseLine(body, image, line_number);
        if (!step)
        {
            return std::unexpected(step.error());
        }
        transcript.push_back(std::move(*step));
    }
    return transcript;
}

std::string FormatWireTranscript(const WireTranscript& transcript, bytes::ByteView image)
{
    ReferenceFormatter formatter(image);
    std::string out;
    for (const auto& step : transcript)
    {
        switch (step.kind)
        {
        case Kind::kWrite:
            out += std::format("> {}\n", formatter.FormatFrame(step.frame));
            break;
        case Kind::kRead:
            out += std::format("< {}\n", formatter.FormatFrame(step.frame));
            break;
        case Kind::kNoFrame:
            out += "< none\n";
            break;
        case Kind::kReadTimeout:
            out += std::format("@ read-timeout {}\n", step.duration.count());
            break;
        case Kind::kSleep:
            out += std::format("@ sleep {}\n", step.duration.count());
            break;
        case Kind::kSection:
            out += std::format("# section {}\n", step.label);
            break;
        }
    }
    return out;
}

} // namespace fastecu::flash
