#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/ports/result.h"

namespace fastecu::definition
{

enum class DefinitionFormat
{
    RomRaider,
    EcuFlash,
};

enum class IdEncoding
{
    Ascii,
    Hex,
};

struct DefinitionIndexEntry
{
    DefinitionFormat format;
    std::string definition_id;
    std::string internal_id;
    std::optional<std::uint64_t> internal_id_address;
    IdEncoding internal_id_encoding;
    std::string ecu_id;
    std::string source;
    std::vector<std::string> parents;

    bool operator==(const DefinitionIndexEntry&) const = default;
};

struct ScalingPresence
{
    bool tracked{false}; // True when the per-field flags came from a parser.
    bool from_byte{false}, to_byte{false}, format{false};

    bool operator==(const ScalingPresence&) const = default;
};

struct Scaling
{
    std::string name, units, from_byte, to_byte, format;
    std::string minimum, maximum, coarse_increment, fine_increment;
    std::string storage_type, endian;
    std::vector<std::pair<std::string, std::string>> selections;
    ScalingPresence supplied;

    bool operator==(const Scaling&) const = default;
};

struct AxisDefinitionPresence
{
    bool tracked{false}; // False keeps value-built callers source-compatible.
    bool size{false}, from_byte{false}, to_byte{false};
    bool start_position{false}, interval{false}, log_parameter{false}, static_data{false};

    bool operator==(const AxisDefinitionPresence&) const = default;
};

struct AxisDefinition
{
    std::string type, name, units, format, storage_type, endian;
    std::optional<std::uint64_t> address;
    std::uint32_t size{1};
    std::string from_byte{"x"}, to_byte{"x"}, scaling_name;
    std::string start_position{"1"}, interval{"1"}, log_parameter;
    std::vector<std::string> static_data;
    AxisDefinitionPresence supplied;

    bool operator==(const AxisDefinition&) const = default;
};

struct CalibrationMapPresence
{
    bool tracked{false}; // False keeps value-built callers source-compatible.
    bool stable_id{false}, x_size{false}, y_size{false};
    bool swap_xy{false}, flip_x{false}, flip_y{false};
    bool start_position{false}, interval{false}, log_parameter{false};

    bool operator==(const CalibrationMapPresence&) const = default;
};

struct CalibrationMap
{
    std::string id, name, type, category, subcategory, description;
    std::optional<std::uint64_t> address;
    std::uint32_t x_size{1}, y_size{1};
    bool swap_xy{false}, flip_x{false}, flip_y{false};
    std::string level, user_level, scaling_name, storage_type, endian;
    std::string start_position{"1"}, interval{"1"}, log_parameter;
    AxisDefinition x_axis, y_axis;
    CalibrationMapPresence supplied;

    bool operator==(const CalibrationMap&) const = default;
};

struct RomIdentity
{
    std::string xml_id, internal_id, ecu_id;
    std::optional<std::uint64_t> internal_id_address;

    bool operator==(const RomIdentity&) const = default;
};

struct RomMetadata
{
    std::string make, market, model, submodel, transmission, year;
    std::string flash_method, memory_model, checksum_module, file_size, notes;

    bool operator==(const RomMetadata&) const = default;
};

struct UnresolvedDefinition
{
    DefinitionFormat format;
    std::string source;
    RomIdentity identity;
    RomMetadata metadata;
    std::vector<std::string> parents;
    std::vector<CalibrationMap> maps;
    std::vector<Scaling> scalings;

    bool operator==(const UnresolvedDefinition&) const = default;
};

struct RomDefinition : UnresolvedDefinition
{
    std::vector<std::string> resolved_sources;
    std::vector<std::string> resolved_definition_ids;

    bool operator==(const RomDefinition&) const = default;
};

class DefinitionCatalog
{
  public:
    static Result<DefinitionCatalog> create(std::vector<DefinitionIndexEntry> entries);

    Result<std::reference_wrapper<const DefinitionIndexEntry>> find(DefinitionFormat format,
                                                                    std::string_view id) const;

    std::span<const DefinitionIndexEntry> entries() const;

  private:
    explicit DefinitionCatalog(std::vector<DefinitionIndexEntry> entries);

    std::vector<DefinitionIndexEntry> entries_;
};

} // namespace fastecu::definition
