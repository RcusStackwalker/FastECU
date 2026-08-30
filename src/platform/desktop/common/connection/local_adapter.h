#pragma once

#include "src/backend/dashboard/dashboard_document.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/ican_transport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastecu::desktop::connection
{
struct LocalAdapterDescriptor
{
    std::string candidate_id;
    dashboard::AdapterKind kind;
    std::string vendor;
    std::string display_name;
    std::string label;
    bool operator==(const LocalAdapterDescriptor&) const = default;
};

struct AdapterDiscoverySnapshot
{
    std::uint64_t generation;
    std::vector<LocalAdapterDescriptor> candidates;
    std::vector<Error> diagnostics;
};

class OpenedCanAdapter
{
  public:
    virtual ~OpenedCanAdapter() = default;
    virtual std::unique_ptr<cdbg::ICanTransport> into_transport() && = 0;
};

class ILocalAdapterProvider
{
  public:
    virtual ~ILocalAdapterProvider() = default;
    virtual dashboard::AdapterKind kind() const = 0;
    virtual Result<std::vector<LocalAdapterDescriptor>> discover() = 0;
    virtual Result<std::unique_ptr<OpenedCanAdapter>> open(std::string_view candidate_id,
                                                           const dashboard::CdbgConnectionProfile& profile) = 0;
};
} // namespace fastecu::desktop::connection
