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

struct ScalingPresence
{
    bool tracked{false}; // True when the per-field flags came from a parser.
    bool from_byte{false};
    bool to_byte{false};
    bool format{false};

    bool operator==(const ScalingPresence&) const = default;
};

struct Scaling
{
    std::string name;
    std::string units;
    std::string from_byte;
    std::string to_byte;
    std::string format;
    std::string minimum;
    std::string maximum;
    std::string coarse_increment;
    std::string fine_increment;
    std::string storage_type;
    std::string endian;
    std::vector<std::pair<std::string, std::string>> selections;
    ScalingPresence supplied;

    bool operator==(const Scaling&) const = default;
};

struct AxisDefinitionPresence
{
    bool tracked{false}; // False keeps value-built callers source-compatible.
    bool size{false};
    bool from_byte{false};
    bool to_byte{false};
    bool start_position{false};
    bool interval{false};
    bool log_parameter{false};
    bool static_data{false};

    bool operator==(const AxisDefinitionPresence&) const = default;
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
    AxisDefinitionPresence supplied;

    bool operator==(const AxisDefinition&) const = default;
};

struct CalibrationMapPresence
{
    bool tracked{false}; // False keeps value-built callers source-compatible.
    bool stable_id{false};
    bool x_size{false};
    bool y_size{false};
    bool swap_xy{false};
    bool flip_x{false};
    bool flip_y{false};
    bool start_position{false};
    bool interval{false};
    bool log_parameter{false};

    bool operator==(const CalibrationMapPresence&) const = default;
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
    CalibrationMapPresence supplied;

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
