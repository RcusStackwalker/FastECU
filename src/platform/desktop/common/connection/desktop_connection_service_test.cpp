#include "src/platform/desktop/common/connection/desktop_connection_service.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "src/backend/dashboard/test_fixtures.h"

namespace fastecu::desktop::connection
{
namespace
{

using dashboard::AdapterKind;

static_assert(!std::copy_constructible<ConnectionPreparationOutcome>);
static_assert(std::move_constructible<ConnectionPreparationOutcome>);

struct AdapterLifetime
{
    int opened = 0;
    int transports = 0;
};

class FakeTransport final : public cdbg::ICanTransport
{
  public:
    explicit FakeTransport(AdapterLifetime& lifetime) : lifetime_(lifetime)
    {
        ++lifetime_.transports;
    }

    ~FakeTransport() override
    {
        --lifetime_.transports;
    }

    Result<std::size_t> write(std::uint32_t, bytes::ByteView payload) override
    {
        return payload.size();
    }

    Result<std::optional<cdbg::CanFrame>> read(int, const ICancellationToken&) override
    {
        return std::optional<cdbg::CanFrame>{};
    }

    bool isOpen() const override
    {
        return true;
    }

  private:
    AdapterLifetime& lifetime_;
};

class FakeOpenedAdapter final : public OpenedCanAdapter
{
  public:
    enum class TransportBehavior
    {
        ReturnTransport,
        ReturnNull,
        ThrowStandard,
    };

    FakeOpenedAdapter(AdapterLifetime& lifetime, TransportBehavior behavior) : lifetime_(lifetime), behavior_(behavior)
    {
        ++lifetime_.opened;
    }

    ~FakeOpenedAdapter() override
    {
        --lifetime_.opened;
    }

    std::unique_ptr<cdbg::ICanTransport> into_transport() && override
    {
        switch (behavior_)
        {
        case TransportBehavior::ReturnTransport:
            return std::make_unique<FakeTransport>(lifetime_);
        case TransportBehavior::ReturnNull:
            return nullptr;
        case TransportBehavior::ThrowStandard:
            throw std::runtime_error("transport configuration failed");
        }
        return nullptr;
    }

  private:
    AdapterLifetime& lifetime_;
    TransportBehavior behavior_;
};

class FakeProvider final : public ILocalAdapterProvider
{
  public:
    explicit FakeProvider(AdapterKind kind) : kind_(kind)
    {
    }

    AdapterKind kind() const override
    {
        return kind_;
    }

    Result<std::vector<LocalAdapterDescriptor>> discover() override
    {
        ++discover_calls;
        if (throw_on_discover)
        {
            throw std::runtime_error("discovery threw");
        }
        if (discovery_error)
        {
            return std::unexpected(*discovery_error);
        }
        return candidates;
    }

    Result<std::unique_ptr<OpenedCanAdapter>> open(std::string_view candidate_id,
                                                   const dashboard::CdbgConnectionProfile& profile) override
    {
        ++open_calls;
        opened_candidate_ids.emplace_back(candidate_id);
        opened_profiles.push_back(profile);
        if (open_error)
        {
            return std::unexpected(*open_error);
        }
        return std::make_unique<FakeOpenedAdapter>(lifetime, transport_behavior);
    }

    AdapterKind kind_;
    std::vector<LocalAdapterDescriptor> candidates;
    std::optional<Error> discovery_error;
    std::optional<Error> open_error;
    bool throw_on_discover = false;
    FakeOpenedAdapter::TransportBehavior transport_behavior = FakeOpenedAdapter::TransportBehavior::ReturnTransport;
    AdapterLifetime lifetime;
    int discover_calls = 0;
    int open_calls = 0;
    std::vector<std::string> opened_candidate_ids;
    std::vector<dashboard::CdbgConnectionProfile> opened_profiles;
};

LocalAdapterDescriptor descriptor(std::string id, AdapterKind kind, std::string vendor, std::string display_name,
                                  std::string label = "adapter")
{
    return LocalAdapterDescriptor{
        .candidate_id = std::move(id),
        .kind = kind,
        .vendor = std::move(vendor),
        .display_name = std::move(display_name),
        .label = std::move(label),
    };
}

dashboard::DashboardDocument usable_document()
{
    auto document = dashboard::test::valid_document();
    document.cards.push_back(dashboard::DashboardCard{
        .id = "rpm-card",
        .channel_id = "CDBG_ENGINE_RPM",
        .conversion_id = "conversion-1",
        .display_type = dashboard::CardDisplayType::Numeric,
        .title = std::nullopt,
        .order = 0,
        .gauge_bounds = std::nullopt,
        .sparkline_history_seconds = std::nullopt,
    });
    return document;
}

dashboard::PreferredAdapter preference_for(const LocalAdapterDescriptor& candidate)
{
    return dashboard::PreferredAdapter{candidate.kind, candidate.vendor, candidate.display_name};
}

template <class Alternative> const Alternative& require_alternative(const ConnectionPreparationOutcome& outcome)
{
    return std::get<Alternative>(outcome);
}

TEST(DesktopConnectionService, InvalidDocumentFailsBeforeHardwareAccess)
{
    FakeProvider j2534(AdapterKind::J2534);
    FakeProvider socketcan(AdapterKind::SocketCan);
    DesktopConnectionService service({&j2534, &socketcan});
    auto document = usable_document();
    document.channels.push_back(document.channels.front());

    const auto outcome = service.prepare_run(document, std::nullopt);

    const auto& error = require_alternative<Error>(outcome);
    EXPECT_EQ(error.kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(j2534.discover_calls, 0);
    EXPECT_EQ(socketcan.discover_calls, 0);
    EXPECT_EQ(j2534.open_calls, 0);
    EXPECT_EQ(socketcan.open_calls, 0);
}

TEST(DesktopConnectionService, EmptyCardSetFailsBeforeHardwareAccess)
{
    FakeProvider provider(AdapterKind::J2534);
    DesktopConnectionService service({&provider});

    const auto outcome = service.prepare_run(dashboard::test::valid_document(), std::nullopt);

    const auto& error = require_alternative<Error>(outcome);
    EXPECT_EQ(error.kind, ErrorKind::InvalidConfig);
    EXPECT_EQ(provider.discover_calls, 0);
    EXPECT_EQ(provider.open_calls, 0);
}

TEST(DesktopConnectionService, NoPreferenceRequiresSelectionWithEverySortedCandidate)
{
    FakeProvider j2534(AdapterKind::J2534);
    FakeProvider socketcan(AdapterKind::SocketCan);
    j2534.candidates = {
        descriptor("j-b", AdapterKind::J2534, "Zulu", "Beta"),
        descriptor("j-a", AdapterKind::J2534, "Alpha", "Gamma"),
    };
    socketcan.candidates = {descriptor("s-a", AdapterKind::SocketCan, "Linux", "can0")};
    DesktopConnectionService service({&socketcan, &j2534});

    const auto outcome = service.prepare_run(usable_document(), std::nullopt);

    const auto& required = require_alternative<AdapterSelectionRequired>(outcome);
    EXPECT_EQ(required.reason, AdapterSelectionRequired::Reason::NoPreference);
    ASSERT_EQ(required.snapshot.candidates.size(), 3U);
    EXPECT_EQ(required.snapshot.candidates[0].candidate_id, "j-a");
    EXPECT_EQ(required.snapshot.candidates[1].candidate_id, "j-b");
    EXPECT_EQ(required.snapshot.candidates[2].candidate_id, "s-a");
    EXPECT_NE(required.snapshot.generation, 0U);
    EXPECT_EQ(j2534.open_calls + socketcan.open_calls, 0);
}

TEST(DesktopConnectionService, ExactPreferenceOpensOnlyItsProviderAndReturnsCompleteOwnedRun)
{
    FakeProvider j2534(AdapterKind::J2534);
    FakeProvider socketcan(AdapterKind::SocketCan);
    const auto selected = descriptor("j0", AdapterKind::J2534, "Tactrix", "OpenPort 2.0", "OpenPort");
    j2534.candidates = {selected};
    socketcan.candidates = {descriptor("s0", AdapterKind::SocketCan, "Linux", "can0")};
    DesktopConnectionService service({&j2534, &socketcan});
    auto document = usable_document();
    document.connection.preferred_adapter = preference_for(selected);

    {
        auto outcome = service.prepare_run(document, std::nullopt);
        const auto& prepared = require_alternative<PreparedConnection>(outcome);
        EXPECT_EQ(prepared.selected, selected);
        EXPECT_NE(prepared.run.protocol, nullptr);
        EXPECT_EQ(prepared.run.session.channels().size(), 1U);
        ASSERT_EQ(j2534.open_calls, 1);
        EXPECT_EQ(j2534.opened_profiles.front(), document.connection);
        EXPECT_EQ(j2534.lifetime.transports, 1);
        EXPECT_EQ(j2534.lifetime.opened, 0);
    }
    EXPECT_EQ(j2534.open_calls, 1);
    EXPECT_EQ(socketcan.open_calls, 0);
    EXPECT_EQ(j2534.lifetime.transports, 0);
}

TEST(DesktopConnectionService, MissingPreferredMatchRequiresSelection)
{
    FakeProvider provider(AdapterKind::J2534);
    provider.candidates = {descriptor("j0", AdapterKind::J2534, "Other", "Adapter")};
    DesktopConnectionService service({&provider});
    auto document = usable_document();
    document.connection.preferred_adapter = dashboard::PreferredAdapter{AdapterKind::J2534, "Tactrix", "OpenPort 2.0"};

    const auto outcome = service.prepare_run(document, std::nullopt);

    EXPECT_EQ(require_alternative<AdapterSelectionRequired>(outcome).reason, AdapterSelectionRequired::Reason::NoMatch);
    EXPECT_EQ(provider.open_calls, 0);
}

TEST(DesktopConnectionService, DuplicateExactPreferredMatchesRequireSelection)
{
    FakeProvider provider(AdapterKind::J2534);
    provider.candidates = {
        descriptor("j0", AdapterKind::J2534, "Tactrix", "OpenPort 2.0"),
        descriptor("j1", AdapterKind::J2534, " tactrix ", "OPENPORT 2.0"),
    };
    DesktopConnectionService service({&provider});
    auto document = usable_document();
    document.connection.preferred_adapter = preference_for(provider.candidates.front());

    const auto outcome = service.prepare_run(document, std::nullopt);

    EXPECT_EQ(require_alternative<AdapterSelectionRequired>(outcome).reason,
              AdapterSelectionRequired::Reason::AmbiguousMatch);
    EXPECT_EQ(provider.open_calls, 0);
}

TEST(DesktopConnectionService, ExplicitCurrentGenerationSelectionOverridesPreference)
{
    FakeProvider provider(AdapterKind::J2534);
    const auto preferred = descriptor("preferred", AdapterKind::J2534, "Vendor", "Preferred");
    const auto explicit_choice = descriptor("explicit", AdapterKind::J2534, "Vendor", "Explicit");
    provider.candidates = {preferred, explicit_choice};
    DesktopConnectionService service({&provider});
    auto document = usable_document();

    const auto first = service.prepare_run(document, std::nullopt);
    const auto& selection = require_alternative<AdapterSelectionRequired>(first);
    document.connection.preferred_adapter = preference_for(preferred);
    auto outcome =
        service.prepare_run(document, AdapterSelection{selection.snapshot.generation, explicit_choice.candidate_id});

    const auto& prepared = require_alternative<PreparedConnection>(outcome);
    EXPECT_EQ(prepared.selected, explicit_choice);
    EXPECT_EQ(provider.discover_calls, 1);
    EXPECT_EQ(provider.opened_candidate_ids, std::vector<std::string>{"explicit"});
}

TEST(DesktopConnectionService, StaleGenerationRediscoversAndRequiresSelection)
{
    FakeProvider provider(AdapterKind::J2534);
    provider.candidates = {descriptor("j0", AdapterKind::J2534, "Vendor", "Adapter")};
    DesktopConnectionService service({&provider});
    const auto first = service.prepare_run(usable_document(), std::nullopt);
    const auto generation = require_alternative<AdapterSelectionRequired>(first).snapshot.generation;

    const auto outcome = service.prepare_run(usable_document(), AdapterSelection{generation + 1, "j0"});

    const auto& required = require_alternative<AdapterSelectionRequired>(outcome);
    EXPECT_EQ(required.reason, AdapterSelectionRequired::Reason::StaleSelection);
    EXPECT_NE(required.snapshot.generation, generation);
    EXPECT_EQ(provider.discover_calls, 2);
    EXPECT_EQ(provider.open_calls, 0);
}

TEST(DesktopConnectionService, UnknownCurrentGenerationIdRediscoversAndRequiresSelection)
{
    FakeProvider provider(AdapterKind::J2534);
    provider.candidates = {descriptor("j0", AdapterKind::J2534, "Vendor", "Adapter")};
    DesktopConnectionService service({&provider});
    const auto first = service.prepare_run(usable_document(), std::nullopt);
    const auto generation = require_alternative<AdapterSelectionRequired>(first).snapshot.generation;

    const auto outcome = service.prepare_run(usable_document(), AdapterSelection{generation, "unknown"});

    EXPECT_EQ(require_alternative<AdapterSelectionRequired>(outcome).reason,
              AdapterSelectionRequired::Reason::StaleSelection);
    EXPECT_EQ(provider.discover_calls, 2);
    EXPECT_EQ(provider.open_calls, 0);
}

TEST(DesktopConnectionService, ProviderDiscoveryFailureRemainsDiagnosticWithOtherCandidatesSelectable)
{
    FakeProvider failed(AdapterKind::J2534);
    FakeProvider available(AdapterKind::SocketCan);
    failed.discovery_error = Error{ErrorKind::Disconnected, "J2534 registry unavailable"};
    available.candidates = {descriptor("s0", AdapterKind::SocketCan, "Linux", "can0")};
    DesktopConnectionService service({&failed, &available});

    const auto outcome = service.prepare_run(usable_document(), std::nullopt);

    const auto& required = require_alternative<AdapterSelectionRequired>(outcome);
    ASSERT_EQ(required.snapshot.candidates.size(), 1U);
    EXPECT_EQ(required.snapshot.candidates.front().candidate_id, "s0");
    ASSERT_EQ(required.snapshot.diagnostics.size(), 1U);
    EXPECT_EQ(required.snapshot.diagnostics.front(), *failed.discovery_error);
}

TEST(DesktopConnectionService, DuplicateCandidateIdsFailAsInternal)
{
    FakeProvider first(AdapterKind::J2534);
    FakeProvider second(AdapterKind::SocketCan);
    first.candidates = {descriptor("same", AdapterKind::J2534, "A", "One")};
    second.candidates = {descriptor("same", AdapterKind::SocketCan, "B", "Two")};
    DesktopConnectionService service({&first, &second});

    const auto outcome = service.prepare_run(usable_document(), std::nullopt);

    EXPECT_EQ(require_alternative<Error>(outcome).kind, ErrorKind::Internal);
    EXPECT_EQ(first.open_calls + second.open_calls, 0);
}

TEST(DesktopConnectionService, FailedOpenReturnsErrorWithoutRetainedAdapter)
{
    FakeProvider provider(AdapterKind::J2534);
    const auto selected = descriptor("j0", AdapterKind::J2534, "Vendor", "Adapter");
    provider.candidates = {selected};
    provider.open_error = Error{ErrorKind::Unsupported, "bitrate unsupported"};
    DesktopConnectionService service({&provider});
    auto document = usable_document();
    document.connection.preferred_adapter = preference_for(selected);

    const auto outcome = service.prepare_run(document, std::nullopt);

    EXPECT_EQ(require_alternative<Error>(outcome), *provider.open_error);
    EXPECT_EQ(provider.lifetime.opened, 0);
    EXPECT_EQ(provider.lifetime.transports, 0);
}

TEST(DesktopConnectionService, TransportConfigurationFailureReturnsInternalWithoutRetainedAdapter)
{
    FakeProvider provider(AdapterKind::J2534);
    const auto selected = descriptor("j0", AdapterKind::J2534, "Vendor", "Adapter");
    provider.candidates = {selected};
    provider.transport_behavior = FakeOpenedAdapter::TransportBehavior::ThrowStandard;
    DesktopConnectionService service({&provider});
    auto document = usable_document();
    document.connection.preferred_adapter = preference_for(selected);

    const auto outcome = service.prepare_run(document, std::nullopt);

    EXPECT_EQ(require_alternative<Error>(outcome).kind, ErrorKind::Internal);
    EXPECT_EQ(provider.lifetime.opened, 0);
    EXPECT_EQ(provider.lifetime.transports, 0);
}

TEST(DesktopConnectionService, NullTransportReturnsInternalWithoutRetainedAdapter)
{
    FakeProvider provider(AdapterKind::J2534);
    const auto selected = descriptor("j0", AdapterKind::J2534, "Vendor", "Adapter");
    provider.candidates = {selected};
    provider.transport_behavior = FakeOpenedAdapter::TransportBehavior::ReturnNull;
    DesktopConnectionService service({&provider});
    auto document = usable_document();
    document.connection.preferred_adapter = preference_for(selected);

    const auto outcome = service.prepare_run(document, std::nullopt);

    EXPECT_EQ(require_alternative<Error>(outcome).kind, ErrorKind::Internal);
    EXPECT_EQ(provider.lifetime.opened, 0);
    EXPECT_EQ(provider.lifetime.transports, 0);
}

TEST(DesktopConnectionService, DiscoveryExceptionsBecomeDiagnosticsAndDoNotSkipOtherProviders)
{
    FakeProvider throwing(AdapterKind::J2534);
    FakeProvider available(AdapterKind::SocketCan);
    throwing.throw_on_discover = true;
    available.candidates = {descriptor("s0", AdapterKind::SocketCan, "Linux", "can0")};
    DesktopConnectionService service({&throwing, &available});

    const auto outcome = service.prepare_run(usable_document(), std::nullopt);

    const auto& required = require_alternative<AdapterSelectionRequired>(outcome);
    EXPECT_EQ(required.snapshot.candidates.size(), 1U);
    ASSERT_EQ(required.snapshot.diagnostics.size(), 1U);
    EXPECT_EQ(required.snapshot.diagnostics.front().kind, ErrorKind::Internal);
    EXPECT_EQ(available.discover_calls, 1);
}

TEST(DesktopConnectionService, RefreshGenerationsAreNonzeroAndMonotonic)
{
    FakeProvider provider(AdapterKind::J2534);
    DesktopConnectionService service({&provider});

    const auto first = service.refresh();
    const auto second = service.refresh();

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->generation, 1U);
    EXPECT_EQ(second->generation, 2U);
}

} // namespace
} // namespace fastecu::desktop::connection
