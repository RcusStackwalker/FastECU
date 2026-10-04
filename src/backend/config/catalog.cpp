#include "src/backend/config/catalog.h"

#include <format>
#include <utility>

namespace fastecu::config
{
namespace
{

bool is_vehicle_id(std::string_view id)
{
    return !id.empty() &&
           std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

void add_protocol_problems(const Catalog& catalog, std::vector<std::string>& problems)
{
    const std::span<const ProtocolSpec> protocols = catalog.protocols();
    for (std::size_t index = 0; index < protocols.size(); ++index)
    {
        const ProtocolSpec& protocol = protocols[index];
        const std::span<const ProtocolSpec> earlier = protocols.first(index);
        if (protocol.name.empty())
        {
            problems.emplace_back("protocol name is empty");
        }
        else if (std::ranges::find(earlier, protocol.name, &ProtocolSpec::name) != earlier.end())
        {
            problems.push_back(std::format("duplicate protocol name '{}'", protocol.name));
        }
        if (protocol.alias.contains(','))
        {
            problems.push_back(std::format("protocol '{}' alias '{}' contains ','", protocol.name, protocol.alias));
        }
        if (protocol.kernel_load_address.has_value() && protocol.kernel.empty())
        {
            problems.push_back(std::format("protocol '{}' has a kernel load address but no kernel", protocol.name));
        }
        if (std::ranges::none_of(catalog.vehicles(),
                                 [&protocol](const VehicleSpec& vehicle) { return vehicle.protocol == &protocol; }))
        {
            problems.push_back(std::format("protocol '{}' has no vehicle", protocol.name));
        }
    }
}

void add_vehicle_problems(const Catalog& catalog, std::vector<std::string>& problems)
{
    const std::span<const VehicleSpec> vehicles = catalog.vehicles();
    for (std::size_t index = 0; index < vehicles.size(); ++index)
    {
        const VehicleSpec& vehicle = vehicles[index];
        const std::span<const VehicleSpec> earlier = vehicles.first(index);
        if (!is_vehicle_id(vehicle.id))
        {
            problems.push_back(std::format("vehicle id '{}' is not lowercase [a-z0-9-]", vehicle.id));
        }
        else if (std::ranges::find(earlier, vehicle.id, &VehicleSpec::id) != earlier.end())
        {
            problems.push_back(std::format("duplicate vehicle id '{}'", vehicle.id));
        }
        if (vehicle.protocol == nullptr)
        {
            problems.push_back(std::format("vehicle '{}' has no protocol", vehicle.id));
        }
        else if (std::ranges::none_of(catalog.protocols(), [&vehicle](const ProtocolSpec& protocol)
                                      { return &protocol == vehicle.protocol; }))
        {
            problems.push_back(std::format("vehicle '{}' protocol is not in this catalog", vehicle.id));
        }
    }
}

} // namespace

std::string_view checksum_flag(ChecksumSupport support)
{
    switch (support)
    {
    case ChecksumSupport::Corrected:
        return "yes";
    case ChecksumSupport::Missing:
        return "n/a";
    case ChecksumSupport::None:
        return "no";
    }
    std::unreachable();
}

std::string kernel_load_address_text(const ProtocolSpec& protocol)
{
    return protocol.kernel_load_address.has_value() ? std::format("0x{:X}", *protocol.kernel_load_address)
                                                    : std::string{};
}

const ProtocolSpec *Catalog::find_protocol(std::string_view name) const
{
    const auto found = std::ranges::find(protocols_, name, &ProtocolSpec::name);
    return found == protocols_.end() ? nullptr : &*found;
}

std::optional<std::size_t> Catalog::find_vehicle(std::string_view id) const
{
    const auto found = std::ranges::find(vehicles_, id, &VehicleSpec::id);
    if (id.empty() || found == vehicles_.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::ranges::distance(vehicles_.begin(), found));
}

std::optional<std::size_t> Catalog::last_vehicle_for_protocol(std::string_view protocol_name) const
{
    std::optional<std::size_t> last;
    for (std::size_t row = 0; row < vehicles_.size(); ++row)
    {
        if (vehicles_[row].protocol != nullptr && vehicles_[row].protocol->name == protocol_name)
        {
            last = row;
        }
    }
    return last;
}

const VehicleSpec *Catalog::first_vehicle_for_alias(std::string_view flash_method) const
{
    if (flash_method.empty())
    {
        return nullptr;
    }
    const auto found =
        std::ranges::find_if(vehicles_, [flash_method](const VehicleSpec& vehicle)
                             { return vehicle.protocol != nullptr && vehicle.protocol->alias == flash_method; });
    return found == vehicles_.end() ? nullptr : &*found;
}

std::vector<std::string> catalog_problems(const Catalog& catalog)
{
    std::vector<std::string> problems;
    add_protocol_problems(catalog, problems);
    add_vehicle_problems(catalog, problems);
    return problems;
}

} // namespace fastecu::config
