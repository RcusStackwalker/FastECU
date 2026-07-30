#pragma once

#include "src/backend/definitions/ecu_cal_def.h"

#include <string_view>
#include <vector>

namespace fastecu::definitions::legacy_columns
{

struct NamedColumn
{
    std::string_view name;
    QStringList EcuCalDefStructure::*values;
};

inline const std::vector<NamedColumn>& map_columns()
{
    static const std::vector<NamedColumn> columns{
        {"id", &EcuCalDefStructure::IdList},
        {"type", &EcuCalDefStructure::TypeList},
        {"name", &EcuCalDefStructure::NameList},
        {"address", &EcuCalDefStructure::AddressList},
        {"category", &EcuCalDefStructure::CategoryList},
        {"category expanded", &EcuCalDefStructure::CategoryExpandedList},
        {"subcategory", &EcuCalDefStructure::SubCategoryList},
        {"level", &EcuCalDefStructure::LevelList},
        {"user level", &EcuCalDefStructure::UserLevelList},
        {"swap xy", &EcuCalDefStructure::SwapXYList},
        {"flip x", &EcuCalDefStructure::FlipXList},
        {"flip y", &EcuCalDefStructure::FlipYList},
        {"x size", &EcuCalDefStructure::XSizeList},
        {"y size", &EcuCalDefStructure::YSizeList},
        {"start position", &EcuCalDefStructure::StartPosList},
        {"interval", &EcuCalDefStructure::IntervalList},
        {"minimum", &EcuCalDefStructure::MinValueList},
        {"maximum", &EcuCalDefStructure::MaxValueList},
        {"units", &EcuCalDefStructure::UnitsList},
        {"format", &EcuCalDefStructure::FormatList},
        {"fine increment", &EcuCalDefStructure::FineIncList},
        {"coarse increment", &EcuCalDefStructure::CoarseIncList},
        {"visible", &EcuCalDefStructure::VisibleList},
        {"selection names", &EcuCalDefStructure::SelectionsNameList},
        {"selection values", &EcuCalDefStructure::SelectionsValueList},
        {"description", &EcuCalDefStructure::DescriptionList},
        {"state", &EcuCalDefStructure::StateList},
        {"map scaling", &EcuCalDefStructure::MapScalingNameList},
        {"map data", &EcuCalDefStructure::MapData},
        {"map color minimum", &EcuCalDefStructure::MapCellColorMin},
        {"map color maximum", &EcuCalDefStructure::MapCellColorMax},
        {"x axis type", &EcuCalDefStructure::XScaleTypeList},
        {"x axis name", &EcuCalDefStructure::XScaleNameList},
        {"x axis address", &EcuCalDefStructure::XScaleAddressList},
        {"x axis start position", &EcuCalDefStructure::XScaleStartPosList},
        {"x axis interval", &EcuCalDefStructure::XScaleIntervalList},
        {"x axis minimum", &EcuCalDefStructure::XScaleMinValueList},
        {"x axis maximum", &EcuCalDefStructure::XScaleMaxValueList},
        {"x axis units", &EcuCalDefStructure::XScaleUnitsList},
        {"x axis format", &EcuCalDefStructure::XScaleFormatList},
        {"x axis fine increment", &EcuCalDefStructure::XScaleFineIncList},
        {"x axis coarse increment", &EcuCalDefStructure::XScaleCoarseIncList},
        {"x axis storage", &EcuCalDefStructure::XScaleStorageTypeList},
        {"x axis endian", &EcuCalDefStructure::XScaleEndianList},
        {"x axis log param", &EcuCalDefStructure::XScaleLogParamList},
        {"x axis from byte", &EcuCalDefStructure::XScaleFromByteList},
        {"x axis to byte", &EcuCalDefStructure::XScaleToByteList},
        {"x axis static data", &EcuCalDefStructure::XScaleStaticDataList},
        {"x axis scaling", &EcuCalDefStructure::XScaleScalingNameList},
        {"x axis data", &EcuCalDefStructure::XScaleData},
        {"y axis type", &EcuCalDefStructure::YScaleTypeList},
        {"y axis name", &EcuCalDefStructure::YScaleNameList},
        {"y axis address", &EcuCalDefStructure::YScaleAddressList},
        {"y axis start position", &EcuCalDefStructure::YScaleStartPosList},
        {"y axis interval", &EcuCalDefStructure::YScaleIntervalList},
        {"y axis minimum", &EcuCalDefStructure::YScaleMinValueList},
        {"y axis maximum", &EcuCalDefStructure::YScaleMaxValueList},
        {"y axis units", &EcuCalDefStructure::YScaleUnitsList},
        {"y axis format", &EcuCalDefStructure::YScaleFormatList},
        {"y axis fine increment", &EcuCalDefStructure::YScaleFineIncList},
        {"y axis coarse increment", &EcuCalDefStructure::YScaleCoarseIncList},
        {"y axis storage", &EcuCalDefStructure::YScaleStorageTypeList},
        {"y axis endian", &EcuCalDefStructure::YScaleEndianList},
        {"y axis log param", &EcuCalDefStructure::YScaleLogParamList},
        {"y axis from byte", &EcuCalDefStructure::YScaleFromByteList},
        {"y axis to byte", &EcuCalDefStructure::YScaleToByteList},
        {"y axis scaling", &EcuCalDefStructure::YScaleScalingNameList},
        {"y axis data", &EcuCalDefStructure::YScaleData},
        {"storage", &EcuCalDefStructure::StorageTypeList},
        {"endian", &EcuCalDefStructure::EndianList},
        {"log param", &EcuCalDefStructure::LogParamList},
        {"from byte", &EcuCalDefStructure::FromByteList},
        {"to byte", &EcuCalDefStructure::ToByteList},
        {"map defined", &EcuCalDefStructure::MapDefined},
    };
    return columns;
}

inline const std::vector<NamedColumn>& scaling_columns()
{
    static const std::vector<NamedColumn> columns{
        {"name", &EcuCalDefStructure::ScalingNameList},
        {"units", &EcuCalDefStructure::ScalingUnitsList},
        {"from byte", &EcuCalDefStructure::ScalingFromByteList},
        {"to byte", &EcuCalDefStructure::ScalingToByteList},
        {"format", &EcuCalDefStructure::ScalingFormatList},
        {"minimum", &EcuCalDefStructure::ScalingMinValueList},
        {"maximum", &EcuCalDefStructure::ScalingMaxValueList},
        {"coarse increment", &EcuCalDefStructure::ScalingCoarseIncList},
        {"fine increment", &EcuCalDefStructure::ScalingFineIncList},
        {"storage", &EcuCalDefStructure::ScalingStorageTypeList},
        {"endian", &EcuCalDefStructure::ScalingEndianList},
        {"selection names", &EcuCalDefStructure::ScalingSelectionsNameList},
        {"selection values", &EcuCalDefStructure::ScalingSelectionsValueList},
    };
    return columns;
}

struct AxisColumns
{
    QStringList *type;
    QStringList *name;
    QStringList *address;
    QStringList *start;
    QStringList *interval;
    QStringList *minimum;
    QStringList *maximum;
    QStringList *units;
    QStringList *format;
    QStringList *fine;
    QStringList *coarse;
    QStringList *storage;
    QStringList *endian;
    QStringList *log_parameter;
    QStringList *from_byte;
    QStringList *to_byte;
    QStringList *scaling_name;
    QStringList *data;
    QStringList *static_data;
};

inline AxisColumns axis_columns(EcuCalDefStructure& value, bool x_axis)
{
    if (x_axis)
    {
        return {
            &value.XScaleTypeList,
            &value.XScaleNameList,
            &value.XScaleAddressList,
            &value.XScaleStartPosList,
            &value.XScaleIntervalList,
            &value.XScaleMinValueList,
            &value.XScaleMaxValueList,
            &value.XScaleUnitsList,
            &value.XScaleFormatList,
            &value.XScaleFineIncList,
            &value.XScaleCoarseIncList,
            &value.XScaleStorageTypeList,
            &value.XScaleEndianList,
            &value.XScaleLogParamList,
            &value.XScaleFromByteList,
            &value.XScaleToByteList,
            &value.XScaleScalingNameList,
            &value.XScaleData,
            &value.XScaleStaticDataList,
        };
    }
    return {
        &value.YScaleTypeList,
        &value.YScaleNameList,
        &value.YScaleAddressList,
        &value.YScaleStartPosList,
        &value.YScaleIntervalList,
        &value.YScaleMinValueList,
        &value.YScaleMaxValueList,
        &value.YScaleUnitsList,
        &value.YScaleFormatList,
        &value.YScaleFineIncList,
        &value.YScaleCoarseIncList,
        &value.YScaleStorageTypeList,
        &value.YScaleEndianList,
        &value.YScaleLogParamList,
        &value.YScaleFromByteList,
        &value.YScaleToByteList,
        &value.YScaleScalingNameList,
        &value.YScaleData,
        nullptr,
    };
}

} // namespace fastecu::definitions::legacy_columns
