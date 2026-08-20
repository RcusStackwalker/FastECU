#include "apps/bench/bench_format.h"

#include <format>

namespace fastecu::bench
{
namespace
{

std::string packedHex(bytes::ByteView data)
{
    std::string out;
    out.reserve(data.size() * 2);
    for (const bytes::Byte byte : data)
    {
        out += std::format("{:02x}", byte);
    }
    return out;
}

std::string jsonEscaped(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char character : text)
    {
        switch (character)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += character;
            break;
        }
    }
    return out;
}

} // namespace

std::string format_text(const CommandOutcome& outcome)
{
    std::string out = std::format("{}\n", outcome.step);
    if (!outcome.tx.empty())
    {
        out += std::format("  TX {}\n", bytes::toHex(outcome.tx));
    }
    if (!outcome.rx.empty())
    {
        out += std::format("  RX {}\n", bytes::toHex(outcome.rx));
    }
    if (!outcome.note.empty())
    {
        out += std::format("  {}\n", outcome.note);
    }
    if (outcome.vbatt.has_value())
    {
        out += std::format("  battery {:.3f} V\n", *outcome.vbatt);
    }
    out += outcome.ok ? std::format("  ok ({} ms)\n", outcome.elapsed_ms)
                      : std::format("  FAIL ({}) {}\n",
                                    outcome.error_kind.has_value() ? to_string(*outcome.error_kind) : "Internal",
                                    outcome.error_detail);
    return out;
}

std::string format_json(const CommandOutcome& outcome)
{
    std::string out =
        std::format(R"({{"step":"{}","tx":"{}","rx":"{}","ms":{},"ok":{})", jsonEscaped(outcome.step),
                    packedHex(outcome.tx), packedHex(outcome.rx), outcome.elapsed_ms, outcome.ok ? "true" : "false");
    if (outcome.vbatt.has_value())
    {
        out += std::format(R"(,"vbatt":{:.3f})", *outcome.vbatt);
    }
    if (!outcome.note.empty())
    {
        out += std::format(R"(,"note":"{}")", jsonEscaped(outcome.note));
    }
    if (outcome.error_kind.has_value())
    {
        out += std::format(R"(,"error_kind":"{}","error_detail":"{}")", to_string(*outcome.error_kind),
                           jsonEscaped(outcome.error_detail));
    }
    out += "}";
    return out;
}

int exit_code_for(ErrorKind kind)
{
    switch (kind)
    {
    case ErrorKind::InvalidConfig:
        return 2;
    case ErrorKind::Timeout:
        return 3;
    case ErrorKind::Disconnected:
        return 4;
    case ErrorKind::BadResponse:
        return 5;
    case ErrorKind::Cancelled:
        return 6;
    case ErrorKind::Unsupported:
        return 7;
    case ErrorKind::Internal:
        return 8;
    }
    return 8;
}

} // namespace fastecu::bench
