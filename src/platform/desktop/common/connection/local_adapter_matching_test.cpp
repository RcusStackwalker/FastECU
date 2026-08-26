#include "src/platform/desktop/common/connection/local_adapter_matching.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fastecu::desktop::connection
{
namespace
{

using dashboard::AdapterKind;
using dashboard::PreferredAdapter;

LocalAdapterDescriptor descriptor(std::string candidate_id, AdapterKind kind, std::string vendor,
                                  std::string display_name)
{
    return {
        .candidate_id = std::move(candidate_id),
        .kind = kind,
        .vendor = std::move(vendor),
        .display_name = std::move(display_name),
        .label = "adapter",
    };
}

void expect_selection_required(const std::optional<PreferredAdapter>& preferred,
                               const std::vector<LocalAdapterDescriptor>& candidates)
{
    const auto result = resolve_preferred_adapter(preferred, candidates);
    EXPECT_EQ(result.kind, AdapterResolutionKind::SelectionRequired);
    EXPECT_TRUE(result.candidate_id.empty());
}

} // namespace

TEST(LocalAdapterMatching, NormalizesCaseAndSurroundingWhitespace)
{
    EXPECT_EQ(normalize_adapter_text("  Tactrix Inc.  "), "tactrix inc.");
}

TEST(LocalAdapterMatching, ResolvesOnlyOneExactThreeFieldMatch)
{
    const PreferredAdapter preferred{AdapterKind::J2534, " TACTRIX ", "OpenPort 2.0"};
    const std::vector<LocalAdapterDescriptor> candidates{
        descriptor("j0", AdapterKind::J2534, "Tactrix", "OpenPort 2.0"),
        descriptor("j1", AdapterKind::J2534, "Other", "OpenPort 2.0"),
    };
    const auto result = resolve_preferred_adapter(preferred, candidates);
    ASSERT_EQ(result.kind, AdapterResolutionKind::UniqueMatch);
    EXPECT_EQ(result.candidate_id, "j0");
}

TEST(LocalAdapterMatching, RequiresSelectionWithoutPreference)
{
    expect_selection_required(std::nullopt, {descriptor("j0", AdapterKind::J2534, "Tactrix", "OpenPort 2.0")});
}

TEST(LocalAdapterMatching, RequiresSelectionWhenNoCandidatesMatch)
{
    const PreferredAdapter preferred{AdapterKind::J2534, "Tactrix", "OpenPort 2.0"};
    expect_selection_required(preferred, {descriptor("j0", AdapterKind::J2534, "Other", "OpenPort 2.0")});
}

TEST(LocalAdapterMatching, RequiresSelectionWhenTwoCandidatesMatch)
{
    const PreferredAdapter preferred{AdapterKind::J2534, "Tactrix", "OpenPort 2.0"};
    expect_selection_required(preferred, {
                                             descriptor("j0", AdapterKind::J2534, "Tactrix", "OpenPort 2.0"),
                                             descriptor("j1", AdapterKind::J2534, "Tactrix", "OpenPort 2.0"),
                                         });
}

TEST(LocalAdapterMatching, RequiresSelectionWhenKindDiffers)
{
    const PreferredAdapter preferred{AdapterKind::J2534, "Tactrix", "OpenPort 2.0"};
    expect_selection_required(preferred, {descriptor("s0", AdapterKind::SocketCan, "Tactrix", "OpenPort 2.0")});
}

TEST(LocalAdapterMatching, RequiresSelectionForSubstringOnlyText)
{
    const PreferredAdapter preferred{AdapterKind::J2534, "Tactrix", "OpenPort 2.0"};
    expect_selection_required(preferred,
                              {descriptor("j0", AdapterKind::J2534, "Tactrix Incorporated", "OpenPort 2.0 Pro")});
}

} // namespace fastecu::desktop::connection
