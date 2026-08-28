#include "src/platform/desktop/common/connection/desktop_connection_service.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "src/backend/dashboard/dashboard_session_builder.h"
#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"
#include "src/platform/desktop/common/connection/local_adapter_matching.h"

namespace fastecu::desktop::connection
{
namespace
{

Error internal_error(std::string detail)
{
    return Error{ErrorKind::Internal, std::move(detail)};
}

Error exception_error(std::string_view operation, const std::exception& exception)
{
    return internal_error(std::string(operation) + ": " + exception.what());
}

Error unknown_exception_error(std::string_view operation)
{
    return internal_error(std::string(operation) + ": unknown exception");
}

bool matches_preference(const dashboard::PreferredAdapter& preferred, const LocalAdapterDescriptor& candidate)
{
    return candidate.kind == preferred.kind &&
           normalize_adapter_text(candidate.vendor) == normalize_adapter_text(preferred.vendor) &&
           normalize_adapter_text(candidate.display_name) == normalize_adapter_text(preferred.display_name);
}

AdapterSelectionRequired::Reason selection_reason(const std::optional<dashboard::PreferredAdapter>& preferred,
                                                  const std::vector<LocalAdapterDescriptor>& candidates)
{
    if (!preferred)
    {
        return AdapterSelectionRequired::Reason::NoPreference;
    }

    const auto match_count = static_cast<std::size_t>(
        std::count_if(candidates.begin(), candidates.end(), [&](const LocalAdapterDescriptor& candidate)
                      { return matches_preference(*preferred, candidate); }));
    return match_count == 0 ? AdapterSelectionRequired::Reason::NoMatch
                            : AdapterSelectionRequired::Reason::AmbiguousMatch;
}

std::string_view provider_name(dashboard::AdapterKind kind)
{
    switch (kind)
    {
    case dashboard::AdapterKind::J2534:
        return "J2534";
    case dashboard::AdapterKind::SocketCan:
        return "SocketCAN";
    }
    return "Unknown";
}

struct ProviderDiscoveryFailure
{
    std::string provider;
    Error error;
};

Error aggregate_discovery_failures(const std::vector<ProviderDiscoveryFailure>& failures)
{
    ErrorKind kind = failures.front().error.kind;
    std::string detail = "all adapter providers failed discovery";
    for (const ProviderDiscoveryFailure& failure : failures)
    {
        if (failure.error.kind != kind)
        {
            kind = ErrorKind::Internal;
        }
        detail += "; " + failure.provider + ": " + failure.error.detail;
    }
    return {kind, std::move(detail)};
}

} // namespace

DesktopConnectionService::DesktopConnectionService(std::vector<ILocalAdapterProvider *> providers)
    : providers_(std::move(providers))
{
}

Result<AdapterDiscoverySnapshot> DesktopConnectionService::refresh()
{
    if (generation_ != std::numeric_limits<std::uint64_t>::max())
    {
        ++generation_;
    }
    if (generation_ == 0)
    {
        generation_ = 1;
    }

    AdapterDiscoverySnapshot snapshot{.generation = generation_, .candidates = {}, .diagnostics = {}};
    std::unordered_map<std::string, ILocalAdapterProvider *> candidate_providers;
    bool duplicate_candidate_id = false;
    std::size_t successful_providers = 0;
    std::vector<ProviderDiscoveryFailure> provider_failures;

    try
    {
        for (ILocalAdapterProvider *provider : providers_)
        {
            if (provider == nullptr)
            {
                Error error = internal_error("adapter discovery: null provider");
                snapshot.diagnostics.push_back(error);
                provider_failures.push_back({"Unknown", std::move(error)});
                continue;
            }

            const std::string provider_label(provider_name(provider->kind()));

            Result<std::vector<LocalAdapterDescriptor>> discovered = [&]()
            {
                try
                {
                    return provider->discover();
                }
                catch (const std::exception& exception)
                {
                    return Result<std::vector<LocalAdapterDescriptor>>{
                        std::unexpected(exception_error("adapter discovery", exception))};
                }
                catch (...)
                {
                    return Result<std::vector<LocalAdapterDescriptor>>{
                        std::unexpected(unknown_exception_error("adapter discovery"))};
                }
            }();

            if (!discovered)
            {
                snapshot.diagnostics.push_back(discovered.error());
                provider_failures.push_back({provider_label, discovered.error()});
                continue;
            }
            ++successful_providers;

            for (auto& candidate : *discovered)
            {
                if (!candidate_providers.emplace(candidate.candidate_id, provider).second)
                {
                    duplicate_candidate_id = true;
                }
                snapshot.candidates.push_back(std::move(candidate));
            }
        }

        if (duplicate_candidate_id)
        {
            current_snapshot_.reset();
            candidate_providers_.clear();
            return std::unexpected(internal_error("adapter discovery returned duplicate candidate IDs"));
        }

        if (successful_providers == 0 && !provider_failures.empty())
        {
            current_snapshot_.reset();
            candidate_providers_.clear();
            return std::unexpected(aggregate_discovery_failures(provider_failures));
        }

        std::sort(snapshot.candidates.begin(), snapshot.candidates.end(),
                  [](const LocalAdapterDescriptor& left, const LocalAdapterDescriptor& right)
                  {
                      return std::tie(left.kind, left.vendor, left.display_name, left.candidate_id) <
                             std::tie(right.kind, right.vendor, right.display_name, right.candidate_id);
                  });
        current_snapshot_ = snapshot;
        candidate_providers_ = std::move(candidate_providers);
        return snapshot;
    }
    catch (const std::exception& exception)
    {
        current_snapshot_.reset();
        candidate_providers_.clear();
        return std::unexpected(exception_error("adapter refresh", exception));
    }
    catch (...)
    {
        current_snapshot_.reset();
        candidate_providers_.clear();
        return std::unexpected(unknown_exception_error("adapter refresh"));
    }
}

ConnectionPreparationOutcome DesktopConnectionService::prepare_run(const dashboard::DashboardDocument& document,
                                                                   std::optional<AdapterSelection> selection)
{
    try
    {
        auto prepared_session = dashboard::prepare_dashboard_session(document);
        if (!prepared_session)
        {
            return prepared_session.error();
        }
        if (prepared_session->session().channels().empty())
        {
            return Error{ErrorKind::InvalidConfig, "dashboard has no cards to log"};
        }

        if (selection)
        {
            if (current_snapshot_ && selection->generation == current_snapshot_->generation)
            {
                const auto provider = candidate_providers_.find(selection->candidate_id);
                const auto descriptor =
                    std::find_if(current_snapshot_->candidates.begin(), current_snapshot_->candidates.end(),
                                 [&](const LocalAdapterDescriptor& candidate)
                                 { return candidate.candidate_id == selection->candidate_id; });
                if (provider != candidate_providers_.end() && descriptor != current_snapshot_->candidates.end())
                {
                    const LocalAdapterDescriptor selected_descriptor = *descriptor;
                    ILocalAdapterProvider *selected_provider = provider->second;
                    auto refreshed = refresh();
                    if (!refreshed)
                    {
                        return refreshed.error();
                    }
                    const auto current_provider = candidate_providers_.find(selection->candidate_id);
                    const auto current_descriptor =
                        std::find_if(refreshed->candidates.begin(), refreshed->candidates.end(),
                                     [&](const LocalAdapterDescriptor& candidate)
                                     { return candidate.candidate_id == selection->candidate_id; });
                    if (current_provider == candidate_providers_.end() ||
                        current_descriptor == refreshed->candidates.end() ||
                        current_provider->second != selected_provider || *current_descriptor != selected_descriptor)
                    {
                        return AdapterSelectionRequired{.snapshot = std::move(*refreshed),
                                                        .reason = AdapterSelectionRequired::Reason::StaleSelection};
                    }
                    return open_run(std::move(*prepared_session), document.connection, *current_descriptor,
                                    *current_provider->second);
                }
            }

            auto refreshed = refresh();
            if (!refreshed)
            {
                return refreshed.error();
            }
            return AdapterSelectionRequired{.snapshot = std::move(*refreshed),
                                            .reason = AdapterSelectionRequired::Reason::StaleSelection};
        }

        auto refreshed = refresh();
        if (!refreshed)
        {
            return refreshed.error();
        }

        const AdapterResolution resolution =
            resolve_preferred_adapter(document.connection.preferred_adapter, refreshed->candidates);
        if (resolution.kind != AdapterResolutionKind::UniqueMatch)
        {
            const auto reason = selection_reason(document.connection.preferred_adapter, refreshed->candidates);
            return AdapterSelectionRequired{
                .snapshot = std::move(*refreshed),
                .reason = reason,
            };
        }

        const auto descriptor = std::find_if(current_snapshot_->candidates.begin(), current_snapshot_->candidates.end(),
                                             [&](const LocalAdapterDescriptor& candidate)
                                             { return candidate.candidate_id == resolution.candidate_id; });
        const auto provider = candidate_providers_.find(resolution.candidate_id);
        if (descriptor == current_snapshot_->candidates.end() || provider == candidate_providers_.end())
        {
            return internal_error("resolved adapter is absent from the current discovery snapshot");
        }
        return open_run(std::move(*prepared_session), document.connection, *descriptor, *provider->second);
    }
    catch (const std::exception& exception)
    {
        return exception_error("connection preparation", exception);
    }
    catch (...)
    {
        return unknown_exception_error("connection preparation");
    }
}

ConnectionPreparationOutcome DesktopConnectionService::open_run(dashboard::PreparedDashboardSession prepared_session,
                                                                const dashboard::CdbgConnectionProfile& profile,
                                                                const LocalAdapterDescriptor& descriptor,
                                                                ILocalAdapterProvider& provider)
{
    auto opened = provider.open(descriptor.candidate_id, profile);
    if (!opened)
    {
        return opened.error();
    }
    if (!*opened)
    {
        return internal_error("adapter provider returned a null opened adapter");
    }

    auto transport = std::move(**opened).into_transport();
    if (!transport)
    {
        return internal_error("opened adapter returned a null CAN transport");
    }

    auto [session, config] = std::move(prepared_session).into_parts();
    auto protocol = std::make_unique<fastecu::logging::CdbgLoggingProtocol>(std::move(transport), session.channels(),
                                                                            std::move(config));
    return PreparedConnection{
        .run = logging::LoggingRun{std::move(session), std::move(protocol)},
        .selected = descriptor,
    };
}

} // namespace fastecu::desktop::connection
