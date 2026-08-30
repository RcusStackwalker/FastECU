#pragma once

#include "src/platform/desktop/common/connection/local_adapter.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastecu::desktop::connection
{
enum class AdapterResolutionKind
{
    UniqueMatch,
    SelectionRequired
};

struct AdapterResolution
{
    AdapterResolutionKind kind;
    std::string candidate_id;
};

std::string normalize_adapter_text(std::string_view text);
AdapterResolution resolve_preferred_adapter(const std::optional<dashboard::PreferredAdapter>& preferred,
                                            const std::vector<LocalAdapterDescriptor>& candidates);
} // namespace fastecu::desktop::connection
