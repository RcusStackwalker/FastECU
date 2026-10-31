#pragma once
#include <any>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_family.h"

namespace fastecu::flash
{

enum class FlashOperation
{
    kRead,
    kTestWrite,
    kWrite,
};

struct MemoryRegion
{
    std::uint32_t start;
    std::uint32_t length;

    bool operator==(const MemoryRegion&) const = default;
};

struct KernelImage
{
    std::string id; // diagnostic identity, not a filesystem path
    std::uint32_t load_address;
    bytes::Bytes bytes; // immutable snapshot owned by the plan
};

struct ConfirmationSpec
{
    enum class Id
    {
        kBeginEepromRead,
        kInspectEepromBytes,
        kCycleIgnition,
        // Plans carry these required confirmations before the desktop prompts.
        // The dialog collects operator answers before the synchronous,
        // dialog-free executor starts; declining prevents execution of the
        // bound attempt. Presence in the plan does not prove a grant.
        kEraseTrigger,
        kTopRegionBootstrap,
        // Requires confirmation that external programming voltage is applied
        // because the adapter cannot supply it, before the attempt runs.
        kApplyProgrammingVoltage,
        // Requires confirmation that VPP and MOD1 are connected for M32R
        // boot mode before the attempt runs.
        kApplyBootModeVoltages,
        // Hitachi SH7058 Read requires confirmation before opening the
        // adapter and starting the K-Line ROM read.
        kStartKlineRead,
        // Denso MC68HC16Y5 BDM Write requires confirmation before uploading
        // the kernel into RAM and starting it; the ROM is not written.
        kKernelBootstrap,
    };

    Id id;
    // Stable semantic arguments; desktop owns translated title/body/buttons.
    std::vector<std::pair<std::string, std::string>> arguments;

    bool operator==(const ConfirmationSpec&) const = default;
};

} // namespace fastecu::flash

template <> struct std::hash<fastecu::flash::ConfirmationSpec::Id>
{
    std::size_t operator()(fastecu::flash::ConfirmationSpec::Id id) const noexcept
    {
        return std::hash<int>{}(static_cast<int>(id));
    }
};

namespace fastecu::flash
{

// The family-specific part of a FlashPlan: one plan type's value, with the
// facts its FamilyTraits states, recorded when it is wrapped (ADR 0030).
// Each plan type lives in its family's header; this file includes none of
// them. A family reads its own plan type back with As (null on a different
// type) or Get (throws std::bad_any_cast on a different type).
class FamilyPlan
{
  public:
    // Empty. ValidateAndBuild rejects a plan whose family plan is empty.
    FamilyPlan() = default;

    // Implicit, as assigning an alternative to a std::variant was. Fails to
    // compile for a plan type without FamilyTraits or a kernel decision.
    template <typename P>
        requires(!std::same_as<P, FamilyPlan>)
    FamilyPlan(P plan)
        : value_(std::move(plan)), family_(FamilyTraits<P>::kFamily), transport_(FamilyTraits<P>::kTransport),
          requires_kernel_(FamilyTraits<P>::kRequiresKernel)
    {
    }

    bool HasValue() const
    {
        return value_.has_value();
    }
    FlashFamily Family() const
    {
        return family_;
    }
    TransportKind Transport() const
    {
        return transport_;
    }
    bool RequiresKernel() const
    {
        return requires_kernel_;
    }

    template <typename P> const P *As() const
    {
        return std::any_cast<P>(&value_);
    }
    template <typename P> const P& Get() const
    {
        return std::any_cast<const P&>(value_);
    }
    // Mutable access keeps the recorded facts true: the plan type cannot change.
    template <typename P> P *As()
    {
        return std::any_cast<P>(&value_);
    }
    template <typename P> P& Get()
    {
        return std::any_cast<P&>(value_);
    }

  private:
    std::any value_;
    FlashFamily family_{};
    TransportKind transport_{};
    bool requires_kernel_ = false;
};

} // namespace fastecu::flash
