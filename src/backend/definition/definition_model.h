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
    AsciiOrHex,
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

struct UnresolvedScaling
{
    std::string name;
    std::string units;
    std::optional<std::string> from_byte;
    std::optional<std::string> to_byte;
    std::optional<std::string> format;
    std::string minimum;
    std::string maximum;
    std::string coarse_increment;
    std::string fine_increment;
    std::string storage_type;
    std::string endian;
    std::vector<std::pair<std::string, std::string>> selections;

    bool operator==(const UnresolvedScaling&) const = default;
};

struct UnresolvedAxisDefinition
{
    std::string type;
    std::string name;
    std::string units;
    std::string format;
    std::string storage_type;
    std::string endian;
    std::optional<std::uint64_t> address;
    std::optional<std::uint32_t> size;
    std::optional<std::string> from_byte;
    std::optional<std::string> to_byte;
    std::string scaling_name;
    std::optional<std::string> start_position;
    std::optional<std::string> interval;
    std::optional<std::string> log_parameter;
    std::optional<std::vector<std::string>> static_data;

    bool operator==(const UnresolvedAxisDefinition&) const = default;
};

struct UnresolvedCalibrationMap
{
    std::optional<std::string> id;
    std::string name;
    std::string type;
    std::string category;
    std::string subcategory;
    std::string description;
    std::optional<std::uint64_t> address;
    std::optional<std::uint32_t> x_size;
    std::optional<std::uint32_t> y_size;
    std::optional<bool> swap_xy;
    std::optional<bool> flip_x;
    std::optional<bool> flip_y;
    std::string level;
    std::string user_level;
    std::string scaling_name;
    std::string storage_type;
    std::string endian;
    std::optional<std::string> start_position;
    std::optional<std::string> interval;
    std::optional<std::string> log_parameter;
    UnresolvedAxisDefinition x_axis;
    UnresolvedAxisDefinition y_axis;

    bool operator==(const UnresolvedCalibrationMap&) const = default;
};

struct Scaling
{
    std::string name;
    std::string units;
    std::string from_byte{"x"};
    std::string to_byte{"x"};
    std::string format;
    std::string minimum;
    std::string maximum;
    std::string coarse_increment;
    std::string fine_increment;
    std::string storage_type;
    std::string endian;
    std::vector<std::pair<std::string, std::string>> selections;

    bool operator==(const Scaling&) const = default;
};

struct AxisDefinition
{
    std::string type;
    std::string name;
    std::string units;
    std::string format;
    std::string storage_type;
    std::string endian;
    std::optional<std::uint64_t> address;
    std::uint32_t size{1};
    std::string from_byte{"x"};
    std::string to_byte{"x"};
    std::string scaling_name;
    std::string start_position{"1"};
    std::string interval{"1"};
    std::string log_parameter;
    std::vector<std::string> static_data;

    bool operator==(const AxisDefinition&) const = default;
};

struct CalibrationMap
{
    std::string id;
    std::string name;
    std::string type;
    std::string category;
    std::string subcategory;
    std::string description;
    std::optional<std::uint64_t> address;
    std::uint32_t x_size{1};
    std::uint32_t y_size{1};
    bool swap_xy{false};
    bool flip_x{false};
    bool flip_y{false};
    std::string level;
    std::string user_level;
    std::string scaling_name;
    std::string storage_type;
    std::string endian;
    std::string start_position{"1"};
    std::string interval{"1"};
    std::string log_parameter;
    AxisDefinition x_axis;
    AxisDefinition y_axis;

    bool operator==(const CalibrationMap&) const = default;
};

struct RomIdentity
{
    std::string xml_id;
    std::string internal_id;
    std::string ecu_id;
    std::optional<std::uint64_t> internal_id_address;

    bool operator==(const RomIdentity&) const = default;
};

struct RomMetadata
{
    std::string make;
    std::string market;
    std::string model;
    std::string submodel;
    std::string transmission;
    std::string year;
    std::string flash_method;
    std::string memory_model;
    std::string checksum_module;
    std::string file_size;
    std::string notes;

    bool operator==(const RomMetadata&) const = default;
};

struct UnresolvedDefinition
{
    DefinitionFormat format;
    std::string source;
    RomIdentity identity;
    RomMetadata metadata;
    std::vector<std::string> parents;
    std::vector<UnresolvedCalibrationMap> maps;
    std::vector<UnresolvedScaling> scalings;

    bool operator==(const UnresolvedDefinition&) const = default;
};

struct RomDefinition
{
    DefinitionFormat format;
    std::string source;
    RomIdentity identity;
    RomMetadata metadata;
    std::vector<std::string> parents;
    std::vector<CalibrationMap> maps;
    std::vector<Scaling> scalings;
    std::vector<std::string> resolved_sources;
    std::vector<std::string> resolved_definition_ids;

    bool operator==(const RomDefinition&) const = default;
};

// Returns the scaling in rom_definition.scalings whose name matches, or
// nullptr if none does. Shared by LegacyDefinitionAdapter's append_map
// (src/backend/definition/legacy_definition_adapter.cpp) and
// fastecu::calibration::compute_map_cell_values
// (src/backend/calibration/calibration_service.cpp) -- both need the
// same map/axis-to-scaling lookup, so it lives here rather than being
// duplicated in either Qt-linked or portable consumer.
const Scaling *find_scaling(const RomDefinition& rom_definition, std::string_view name);

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
