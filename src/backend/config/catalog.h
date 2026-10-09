#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fastecu::config
{

// How a protocol's ROM checksum is handled before a write. protocols.cfg
// spelled these "yes", "n/a" and "no".
enum class ChecksumSupport
{
    kCorrected, // a checksum module corrects the image
    kMissing,   // a module should exist but does not: warn before writing
    kNone,      // the family has no checksum: write without a warning
};

// The legacy flag text ChecksumSelection still takes: "yes", "n/a" or "no".
std::string_view checksum_flag(ChecksumSupport support);

// One flash and logging protocol, as compile-time data.
struct ProtocolSpec
{
    std::string_view name;  // unique within a catalog
    std::string_view alias; // one token a definition's flash method may use; empty when none
    std::string_view ecu;
    std::string_view mcu;
    std::string_view mode;
    ChecksumSupport checksum = ChecksumSupport::kNone;
    bool read = false;
    bool test_write = false;
    bool write = false;
    std::string_view flash_transport; // comma-separated; the UI splits it
    std::string_view log_transport;   // comma-separated; the UI splits it
    std::string_view log_protocol;
    std::string_view kernel; // a file in the kernel directory; empty when none is uploaded
    std::optional<std::uint32_t> kernel_load_address;
    std::string_view description;

    bool operator==(const ProtocolSpec&) const = default;
};

// The load address as protocols.cfg spelled it -- "0x" then unpadded
// uppercase hex, e.g. "0xFFFF3000" -- or "" when there is none.
std::string kernel_load_address_text(const ProtocolSpec& protocol);

// One vehicle the operator can select.
struct VehicleSpec
{
    std::string_view id; // saved in fastecu.cfg: never changed, never reused
    std::string_view make;
    std::string_view model;
    std::string_view version;
    std::string_view type;
    std::string_view kw;
    std::string_view hp;
    std::string_view fuel;
    std::string_view year;
    // Into the same catalog's protocols; never null in a consistent catalog.
    const ProtocolSpec *protocol = nullptr;
};

// The protocol named `name`, for wiring VehicleSpec::protocol in constant
// data; nullptr when `protocols` has none, which catalog_references_resolve
// then rejects.
consteval const ProtocolSpec *protocol_in(std::span<const ProtocolSpec> protocols, std::string_view name)
{
    const auto found = std::ranges::find(protocols, name, &ProtocolSpec::name);
    return found == protocols.end() ? nullptr : &*found;
}

// The checks cheap enough to static_assert on every supported compiler:
// every vehicle has a protocol, every protocol has a vehicle, and no protocol
// has a kernel load address without a kernel. catalog_problems() checks
// these and the rest.
constexpr bool catalog_references_resolve(std::span<const ProtocolSpec> protocols,
                                          std::span<const VehicleSpec> vehicles)
{
    const auto has_protocol = [](const VehicleSpec& vehicle) { return vehicle.protocol != nullptr; };
    const auto reachable = [vehicles](const ProtocolSpec& protocol)
    {
        return std::ranges::any_of(vehicles,
                                   [&protocol](const VehicleSpec& vehicle) { return vehicle.protocol == &protocol; });
    };
    const auto kernel_named = [](const ProtocolSpec& protocol)
    { return !protocol.kernel_load_address.has_value() || !protocol.kernel.empty(); };
    return std::ranges::all_of(vehicles, has_protocol) && std::ranges::all_of(protocols, reachable) &&
           std::ranges::all_of(protocols, kernel_named);
}

// A protocol and vehicle table the application runs against. Holds views:
// the arrays it is built from must outlive it.
class Catalog
{
  public:
    constexpr Catalog(std::span<const ProtocolSpec> protocols, std::span<const VehicleSpec> vehicles) noexcept
        : protocols_(protocols), vehicles_(vehicles)
    {
    }

    constexpr std::span<const ProtocolSpec> protocols() const noexcept
    {
        return protocols_;
    }
    // Presentation order; a vehicle's row is its position.
    constexpr std::span<const VehicleSpec> vehicles() const noexcept
    {
        return vehicles_;
    }

    // nullptr when no protocol is named `name`.
    const ProtocolSpec *find_protocol(std::string_view name) const;
    // The row of the vehicle whose id is `id`.
    std::optional<std::size_t> find_vehicle(std::string_view id) const;
    // The LAST row whose protocol is named `protocol_name`: the rule ROM open
    // has always used to select a vehicle from a flash method.
    std::optional<std::size_t> last_vehicle_for_protocol(std::string_view protocol_name) const;
    // The FIRST vehicle whose protocol's alias is `flash_method`: the rule a
    // definition's flash method has always been resolved by. An empty
    // `flash_method` matches nothing.
    const VehicleSpec *first_vehicle_for_alias(std::string_view flash_method) const;

  private:
    std::span<const ProtocolSpec> protocols_;
    std::span<const VehicleSpec> vehicles_;
};

// Every way `catalog` is inconsistent, one message each; empty when it is
// consistent. Covers catalog_references_resolve, plus duplicate protocol
// names and vehicle ids, vehicle id spelling, comma-separated aliases, and
// vehicles whose protocol belongs to another catalog.
std::vector<std::string> catalog_problems(const Catalog& catalog);

} // namespace fastecu::config
