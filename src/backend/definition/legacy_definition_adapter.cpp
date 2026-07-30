#include "src/backend/definition/legacy_definition_adapter.h"
#include "src/backend/definitions/legacy_definition_columns.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "src/backend/definition/text_format.h"

namespace fastecu::definition
{
namespace
{

constexpr std::string_view kPlaceholder = " ";

QString qs(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString legacy_value(std::string_view value)
{
    return value.empty() ? qs(kPlaceholder) : qs(value);
}

QString address_text(const std::optional<std::uint64_t>& address, bool placeholder)
{
    if (!address.has_value())
    {
        return placeholder ? qs(kPlaceholder) : QString{};
    }
    return qs(hex_text(*address));
}

QString bool_text(bool value)
{
    return value ? "true" : "false";
}

QString selection_names(const Scaling *scaling)
{
    if (scaling == nullptr || scaling->selections.empty())
    {
        return qs(kPlaceholder);
    }
    QString result;
    for (const auto& [name, unused] : scaling->selections)
    {
        (void)unused;
        result += qs(name);
        result += ",";
    }
    return result;
}

QString selection_values(const Scaling *scaling)
{
    if (scaling == nullptr || scaling->selections.empty())
    {
        return qs(kPlaceholder);
    }
    QString result;
    for (const auto& [unused, value] : scaling->selections)
    {
        (void)unused;
        result += qs(value);
        result += ",";
    }
    return result;
}

QString static_data_text(const AxisDefinition& axis)
{
    if (axis.static_data.empty())
    {
        return qs(kPlaceholder);
    }
    QString result;
    for (const std::string& value : axis.static_data)
    {
        result += qs(value);
        result += ",";
    }
    return result;
}

const Scaling *find_scaling(const RomDefinition& definition, std::string_view name)
{
    for (const Scaling& scaling : definition.scalings)
    {
        if (scaling.name == name)
        {
            return &scaling;
        }
    }
    return nullptr;
}

QString legacy_format(std::string_view format)
{
    return legacy_value(format);
}

struct CatalogLists
{
    QStringList *ids;
    QStringList *addresses;
    QStringList *ecu_ids;
    QStringList *sources;
};

Status populate_catalog(
    definitions::ConfigValuesStructure& next,
    const DefinitionCatalog& catalog,
    DefinitionFormat format)
{
    CatalogLists lists =
        format == DefinitionFormat::RomRaider
            ? CatalogLists{
                  &next.romraider_def_cal_id,
                  &next.romraider_def_cal_id_addr,
                  &next.romraider_def_ecu_id,
                  &next.romraider_def_filename,
              }
            : CatalogLists{
                  &next.ecuflash_def_cal_id,
                  &next.ecuflash_def_cal_id_addr,
                  &next.ecuflash_def_ecu_id,
                  &next.ecuflash_def_filename,
              };

    lists.ids->clear();
    lists.addresses->clear();
    lists.ecu_ids->clear();
    lists.sources->clear();
    for (const DefinitionIndexEntry& entry : catalog.entries())
    {
        if (entry.format != format)
        {
            continue;
        }
        lists.ids->append(qs(entry.definition_id));
        lists.addresses->append(address_text(entry.internal_id_address, false));
        lists.ecu_ids->append(qs(entry.ecu_id));
        lists.sources->append(qs(entry.source));
    }

    const qsizetype rows = lists.ids->size();
    if (lists.addresses->size() != rows ||
        lists.ecu_ids->size() != rows ||
        lists.sources->size() != rows)
    {
        return fail(ErrorKind::InvalidConfig, "legacy definition catalog lists are not aligned");
    }
    return {};
}

void clear_map_rows(definitions::EcuCalDefStructure& value)
{
    for (const auto& column : definitions::legacy_columns::map_columns())
    {
        (value.*(column.values)).clear();
    }
}

void clear_scaling_rows(definitions::EcuCalDefStructure& value)
{
    for (const auto& column : definitions::legacy_columns::scaling_columns())
    {
        (value.*(column.values)).clear();
    }
}

void append_axis(
    definitions::EcuCalDefStructure& value,
    const AxisDefinition& axis,
    const Scaling *scaling,
    bool x_axis)
{
    const bool present = !axis.type.empty();
    auto text = [present](std::string_view field)
    {
        return present ? legacy_value(field) : qs(kPlaceholder);
    };
    const auto columns = definitions::legacy_columns::axis_columns(value, x_axis);

    columns.type->append(text(axis.type));
    columns.name->append(text(axis.name));
    columns.address->append(present ? address_text(axis.address, true) : qs(kPlaceholder));
    columns.start->append(present ? legacy_value(axis.start_position) : qs(kPlaceholder));
    columns.interval->append(present ? legacy_value(axis.interval) : qs(kPlaceholder));
    columns.minimum->append(present && scaling ? legacy_value(scaling->minimum) : qs(kPlaceholder));
    columns.maximum->append(present && scaling ? legacy_value(scaling->maximum) : qs(kPlaceholder));
    columns.units->append(text(axis.units));
    columns.format->append(
        present && scaling ? legacy_format(scaling->format) : qs(kPlaceholder));
    columns.fine->append(present && scaling ? legacy_value(scaling->fine_increment) : qs(kPlaceholder));
    columns.coarse->append(present && scaling ? legacy_value(scaling->coarse_increment) : qs(kPlaceholder));
    columns.storage->append(text(axis.storage_type));
    columns.endian->append(text(axis.endian));
    columns.log_parameter->append(present ? legacy_value(axis.log_parameter) : qs(kPlaceholder));
    columns.from_byte->append(text(axis.from_byte));
    columns.to_byte->append(text(axis.to_byte));
    columns.scaling_name->append(text(axis.scaling_name));
    columns.data->append(qs(kPlaceholder));
    if (columns.static_data)
    {
        columns.static_data->append(static_data_text(axis));
    }
}

void append_map(definitions::EcuCalDefStructure& value, const RomDefinition& definition, const CalibrationMap& map)
{
    const Scaling *scaling = find_scaling(definition, map.scaling_name);
    const AxisDefinition& x = map.x_axis;
    const AxisDefinition& y = map.y_axis;
    const Scaling *x_scaling = find_scaling(definition, x.scaling_name);
    const Scaling *y_scaling = find_scaling(definition, y.scaling_name);
    const std::string_view storage =
        !map.storage_type.empty() ? std::string_view(map.storage_type)
        : scaling                 ? std::string_view(scaling->storage_type)
                                  : std::string_view{};
    const std::string_view endian =
        !map.endian.empty() ? std::string_view(map.endian)
        : scaling           ? std::string_view(scaling->endian)
                            : std::string_view{};

    value.IdList.append(legacy_value(map.id));
    value.TypeList.append(legacy_value(map.type));
    value.NameList.append(legacy_value(map.name));
    value.AddressList.append(address_text(map.address, true));
    value.CategoryList.append(legacy_value(map.category));
    value.CategoryExpandedList.append(qs(kPlaceholder));
    value.SubCategoryList.append(legacy_value(map.subcategory));
    value.LevelList.append(legacy_value(map.level));
    value.UserLevelList.append(legacy_value(map.user_level));
    value.SwapXYList.append(bool_text(map.swap_xy));
    value.FlipXList.append(bool_text(map.flip_x));
    value.FlipYList.append(bool_text(map.flip_y));
    value.XSizeList.append(QString::number(map.x_size));
    value.YSizeList.append(QString::number(map.y_size));
    value.StartPosList.append(legacy_value(map.start_position));
    value.IntervalList.append(legacy_value(map.interval));
    value.MinValueList.append(scaling ? legacy_value(scaling->minimum) : qs(kPlaceholder));
    value.MaxValueList.append(scaling ? legacy_value(scaling->maximum) : qs(kPlaceholder));
    value.UnitsList.append(scaling ? legacy_value(scaling->units) : qs(kPlaceholder));
    value.FormatList.append(
        scaling ? legacy_format(scaling->format) : qs(kPlaceholder));
    value.FineIncList.append(scaling ? legacy_value(scaling->fine_increment) : qs(kPlaceholder));
    value.CoarseIncList.append(scaling ? legacy_value(scaling->coarse_increment) : qs(kPlaceholder));
    value.VisibleList.append(qs(kPlaceholder));
    value.SelectionsNameList.append(selection_names(scaling));
    value.SelectionsValueList.append(selection_values(scaling));
    value.DescriptionList.append(legacy_value(map.description));
    value.StateList.append(qs(kPlaceholder));
    value.MapScalingNameList.append(legacy_value(map.scaling_name));
    value.MapData.append(qs(kPlaceholder));
    value.MapCellColorMin.append(qs(kPlaceholder));
    value.MapCellColorMax.append(qs(kPlaceholder));
    append_axis(value, x, x_scaling, true);
    append_axis(value, y, y_scaling, false);
    value.StorageTypeList.append(legacy_value(storage));
    value.EndianList.append(legacy_value(endian));
    value.LogParamList.append(legacy_value(map.log_parameter));
    value.FromByteList.append(scaling ? legacy_value(scaling->from_byte) : qs(kPlaceholder));
    value.ToByteList.append(scaling ? legacy_value(scaling->to_byte) : qs(kPlaceholder));
    value.MapDefined.append(qs(kPlaceholder));
}

void append_scaling(definitions::EcuCalDefStructure& value, const Scaling& scaling)
{
    value.ScalingNameList.append(legacy_value(scaling.name));
    value.ScalingUnitsList.append(legacy_value(scaling.units));
    value.ScalingFromByteList.append(legacy_value(scaling.from_byte));
    value.ScalingToByteList.append(legacy_value(scaling.to_byte));
    value.ScalingFormatList.append(legacy_value(scaling.format));
    value.ScalingMinValueList.append(legacy_value(scaling.minimum));
    value.ScalingMaxValueList.append(legacy_value(scaling.maximum));
    value.ScalingCoarseIncList.append(legacy_value(scaling.coarse_increment));
    value.ScalingFineIncList.append(legacy_value(scaling.fine_increment));
    value.ScalingStorageTypeList.append(legacy_value(scaling.storage_type));
    value.ScalingEndianList.append(legacy_value(scaling.endian));
    value.ScalingSelectionsNameList.append(selection_names(&scaling));
    value.ScalingSelectionsValueList.append(selection_values(&scaling));
}

Status validate_definition_alignment(const definitions::EcuCalDefStructure& value)
{
    const qsizetype map_rows = value.NameList.size();
    const auto map_lists = definitions::legacy_columns::map_columns();
    for (const auto& [name, list] : map_lists)
    {
        if ((value.*list).size() != map_rows)
        {
            return fail(
                ErrorKind::InvalidConfig,
                "legacy definition map list '" + std::string(name) + "' is not aligned");
        }
    }

    const qsizetype scaling_rows = value.ScalingNameList.size();
    const auto scaling_lists = definitions::legacy_columns::scaling_columns();
    for (const auto& [name, list] : scaling_lists)
    {
        if ((value.*list).size() != scaling_rows)
        {
            return fail(
                ErrorKind::InvalidConfig,
                "legacy definition scaling list '" + std::string(name) + "' is not aligned");
        }
    }
    return {};
}

Status populate_rom_info(
    definitions::EcuCalDefStructure& value,
    const RomDefinition& definition)
{
    enum RomInfoIndex
    {
        XmlId,
        InternalIdAddress,
        InternalIdString,
        EcuId,
        Make,
        Market,
        Model,
        SubModel,
        Transmission,
        Year,
        FlashMethod,
        MemModel,
        ChecksumModule,
        RomBase,
        FileSize,
        DefFile,
    };

    constexpr qsizetype kRequiredSlots = DefFile + 1;
    if (value.RomInfoStrings.size() < kRequiredSlots ||
        value.RomInfoNames.size() < kRequiredSlots ||
        value.RomInfoStrings.size() != value.RomInfoNames.size())
    {
        return fail(
            ErrorKind::InvalidConfig,
            "legacy RomInfo labels must provide matching names and text for all 16 slots");
    }
    value.RomInfo = QStringList(value.RomInfoStrings.size(), QString{});
    // resolved_definition_ids is ordered root-ancestor-first (self last), so its front() is the
    // wrong end for "the base this definition directly inherits from" -- that's the definition's
    // own declared parents, which survive resolution unmodified on RomDefinition::parents.
    const QString parent =
        definition.parents.empty()
            ? QString{}
            : qs(definition.parents.front());
    value.RomInfo[XmlId] = qs(definition.identity.xml_id);
    value.RomInfo[InternalIdAddress] =
        address_text(definition.identity.internal_id_address, false);
    value.RomInfo[InternalIdString] = qs(definition.identity.internal_id);
    value.RomInfo[EcuId] = qs(definition.identity.ecu_id);
    value.RomInfo[Make] = qs(definition.metadata.make);
    value.RomInfo[Market] = qs(definition.metadata.market);
    value.RomInfo[Model] = qs(definition.metadata.model);
    value.RomInfo[SubModel] = qs(definition.metadata.submodel);
    value.RomInfo[Transmission] = qs(definition.metadata.transmission);
    value.RomInfo[Year] = qs(definition.metadata.year);
    value.RomInfo[FlashMethod] = qs(definition.metadata.flash_method);
    value.RomInfo[MemModel] = qs(definition.metadata.memory_model);
    value.RomInfo[ChecksumModule] = qs(definition.metadata.checksum_module);
    value.RomInfo[RomBase] = parent;
    value.RomInfo[FileSize] = qs(definition.metadata.file_size);
    value.RomInfo[DefFile] = qs(definition.source);
    value.RomBase = parent;
    value.DefinitionFileName = qs(definition.source);
    return {};
}

} // namespace

LegacyDefinitionAdapter::LegacyDefinitionAdapter(DefinitionService& service)
    : service_(service)
{
}

Status LegacyDefinitionAdapter::replace_romraider_catalog(
    definitions::ConfigValuesStructure& current,
    std::span<const std::string> ordered_handles)
{
    auto catalog = service_.build_romraider_catalog(ordered_handles);
    if (!catalog.has_value())
    {
        return std::unexpected(catalog.error());
    }
    definitions::ConfigValuesStructure next = current;
    auto populated = populate_catalog(next, *catalog, DefinitionFormat::RomRaider);
    if (!populated.has_value())
    {
        return populated;
    }
    current = std::move(next);
    return {};
}

Status LegacyDefinitionAdapter::replace_ecuflash_catalog(
    definitions::ConfigValuesStructure& current,
    std::string_view directory,
    std::span<const std::string> explicit_handles)
{
    auto catalog =
        service_.build_ecuflash_catalog(directory, explicit_handles);
    if (!catalog.has_value())
    {
        return std::unexpected(catalog.error());
    }
    definitions::ConfigValuesStructure next = current;
    auto populated = populate_catalog(next, *catalog, DefinitionFormat::EcuFlash);
    if (!populated.has_value())
    {
        return populated;
    }
    current = std::move(next);
    return {};
}

Status LegacyDefinitionAdapter::replace_definition(
    definitions::EcuCalDefStructure& current,
    const DefinitionCatalog& catalog,
    DefinitionFormat format,
    std::string_view id)
{
    auto definition = service_.load(catalog, format, id);
    if (!definition.has_value())
    {
        return std::unexpected(definition.error());
    }

    definitions::EcuCalDefStructure next = current;
    clear_map_rows(next);
    clear_scaling_rows(next);
    auto rom_info = populate_rom_info(next, *definition);
    if (!rom_info.has_value())
    {
        return rom_info;
    }
    next.use_romraider_definition = format == DefinitionFormat::RomRaider;
    next.use_ecuflash_definition = format == DefinitionFormat::EcuFlash;
    for (const Scaling& scaling : definition->scalings)
    {
        append_scaling(next, scaling);
    }
    for (const CalibrationMap& map : definition->maps)
    {
        append_map(next, *definition, map);
    }
    auto aligned = validate_definition_alignment(next);
    if (!aligned.has_value())
    {
        return aligned;
    }
    current = std::move(next);
    return {};
}

Status LegacyDefinitionAdapter::create_definition(
    std::string_view destination,
    const DefinitionHeaderInput& input,
    bool allow_overwrite)
{
    return service_.create_definition(destination, input, allow_overwrite);
}

Status LegacyDefinitionAdapter::import_definition(
    std::string_view source,
    std::string_view destination,
    const DefinitionHeaderInput& input)
{
    return service_.import_definition(source, destination, input);
}

} // namespace fastecu::definition
