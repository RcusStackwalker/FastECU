#include "src/platform/desktop/common/connection/local_adapter_matching.h"

namespace fastecu::desktop::connection
{
namespace
{

bool is_ascii_whitespace(char character)
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' ||
           character == '\v';
}

char lowercase_ascii(char character)
{
    if (character >= 'A' && character <= 'Z')
    {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

bool matches_preference(const dashboard::PreferredAdapter& preferred, const LocalAdapterDescriptor& candidate)
{
    return candidate.kind == preferred.kind &&
           normalize_adapter_text(candidate.vendor) == normalize_adapter_text(preferred.vendor) &&
           normalize_adapter_text(candidate.display_name) == normalize_adapter_text(preferred.display_name);
}

} // namespace

std::string normalize_adapter_text(std::string_view text)
{
    std::size_t first = 0;
    while (first < text.size() && is_ascii_whitespace(text[first]))
    {
        ++first;
    }

    std::size_t last = text.size();
    while (last > first && is_ascii_whitespace(text[last - 1]))
    {
        --last;
    }

    std::string normalized;
    normalized.reserve(last - first);
    for (std::size_t index = first; index < last; ++index)
    {
        normalized.push_back(lowercase_ascii(text[index]));
    }
    return normalized;
}

AdapterResolution resolve_preferred_adapter(const std::optional<dashboard::PreferredAdapter>& preferred,
                                            const std::vector<LocalAdapterDescriptor>& candidates)
{
    if (!preferred)
    {
        return {AdapterResolutionKind::SelectionRequired, {}};
    }

    const LocalAdapterDescriptor *match = nullptr;
    for (const auto& candidate : candidates)
    {
        if (!matches_preference(*preferred, candidate))
        {
            continue;
        }
        if (match != nullptr)
        {
            return {AdapterResolutionKind::SelectionRequired, {}};
        }
        match = &candidate;
    }

    if (match == nullptr)
    {
        return {AdapterResolutionKind::SelectionRequired, {}};
    }
    return {AdapterResolutionKind::UniqueMatch, match->candidate_id};
}

} // namespace fastecu::desktop::connection
