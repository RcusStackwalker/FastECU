#include "apps/bench/bench_format.h"

#include <cmath>
#include <format>
#include <optional>

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
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            // Every other C0 control character is not valid raw JSON text;
            // escape it as \u00XX. Cast through unsigned char first: char may
            // be signed, and a negative value would break the `< 0x20` test
            // and the hex formatting below.
            if (const auto byte = static_cast<unsigned char>(character); byte < 0x20)
            {
                out += std::format("\\u{:04x}", byte);
            }
            else
            {
                out += character;
            }
            break;
        }
    }
    return out;
}

std::optional<double> bytesPerSecond(const CommandOutcome& outcome)
{
    if (outcome.data.empty() || outcome.elapsed_ms == 0)
    {
        return std::nullopt;
    }
    return static_cast<double>(outcome.data.size()) * 1000.0 / static_cast<double>(outcome.elapsed_ms);
}

std::optional<double> msPerExchange(const CommandOutcome& outcome)
{
    if (outcome.exchange_count == 0)
    {
        return std::nullopt;
    }
    return static_cast<double>(outcome.elapsed_ms) / static_cast<double>(outcome.exchange_count);
}

} // namespace

std::string format_text(const CommandOutcome& outcome, bool stats)
{
    std::string out = std::format("{}\n", outcome.step);
    out += std::format("  {} {} ({} ms)\n", outcome.exchange_count,
                       outcome.exchange_count == 1 ? "exchange" : "exchanges", outcome.elapsed_ms);
    if (outcome.exchange_count > 0)
    {
        out += std::format("  TX first {}\n", bytes::toHex(outcome.tx));
        out += std::format("  RX first {}\n", bytes::toHex(outcome.rx));
        out += std::format("  TX last {}\n", bytes::toHex(outcome.last_tx));
        out += std::format("  RX last {}\n", bytes::toHex(outcome.last_rx));
    }
    if (!outcome.data.empty())
    {
        out += std::format("  DATA {}\n", bytes::toHex(outcome.data));
    }
    if (!outcome.note.empty())
    {
        out += std::format("  {}\n", outcome.note);
    }
    if (outcome.vbatt.has_value())
    {
        out += std::format("  battery {:.3f} V\n", *outcome.vbatt);
    }
    if (stats)
    {
        const std::optional<double> rate = bytesPerSecond(outcome);
        const std::optional<double> per_exchange = msPerExchange(outcome);
        if (rate.has_value() && per_exchange.has_value())
        {
            out += std::format("  {:.1f} bytes/s, {:.1f} ms/exchange\n", *rate, *per_exchange);
        }
        else if (rate.has_value())
        {
            out += std::format("  {:.1f} bytes/s\n", *rate);
        }
        else if (per_exchange.has_value())
        {
            out += std::format("  {:.1f} ms/exchange\n", *per_exchange);
        }
    }
    out += outcome.ok ? std::format("  ok ({} ms)\n", outcome.elapsed_ms)
                      : std::format("  FAIL ({}) {}\n",
                                    outcome.error_kind.has_value() ? to_string(*outcome.error_kind) : "Internal",
                                    outcome.error_detail);
    return out;
}

std::string format_json(const CommandOutcome& outcome, bool stats)
{
    std::string out = std::format(
        R"({{"step":"{}","exchanges":{},"tx":"{}","rx":"{}","last_tx":"{}","last_rx":"{}","data":"{}","ms":{},"ok":{})",
        jsonEscaped(outcome.step), outcome.exchange_count, packedHex(outcome.tx), packedHex(outcome.rx),
        packedHex(outcome.last_tx), packedHex(outcome.last_rx), packedHex(outcome.data), outcome.elapsed_ms,
        outcome.ok ? "true" : "false");
    if (outcome.vbatt.has_value() && std::isfinite(*outcome.vbatt))
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
    if (stats)
    {
        if (const std::optional<double> rate = bytesPerSecond(outcome); rate.has_value())
        {
            out += std::format(R"(,"bytes_per_s":{:.1f})", *rate);
        }
        if (const std::optional<double> per_exchange = msPerExchange(outcome); per_exchange.has_value())
        {
            out += std::format(R"(,"ms_per_exchange":{:.1f})", *per_exchange);
        }
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
