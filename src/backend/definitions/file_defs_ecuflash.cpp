#include "src/backend/definitions/file_actions.h"

#include <set>
#include <string>

namespace
{

std::string ecuflashUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

} // namespace

QString FileActions::convert_value_format(const QString& value_format)
{
    const QStringList format_text = value_format.split(".");
    QString decimal_count;
    QString decimals = "0";
    if (format_text.length() > 1 && format_text.at(1).contains("f"))
    {
        decimal_count = format_text.at(1).split("f").at(0);
    }
    if (decimal_count.toInt() > 0)
    {
        decimals.append(".");
    }
    for (int index = 0; index < decimal_count.toInt(); ++index)
    {
        decimals.append("0");
    }
    return decimals;
}

FileActions::ConfigValuesStructure *FileActions::create_ecuflash_def_id_list(
    ConfigValuesStructure *configValues)
{
    if (configValues->ecuflash_definition_files_directory.isEmpty())
    {
        emit LOG_D("No EcuFlash definition files directory", true, true);
        return configValues;
    }

    const fastecu::Status replaced =
        definitionAdapter_.replace_ecuflash_catalog(
            *configValues,
            ecuflashUtf8(
                configValues->ecuflash_definition_files_directory));
    if (!replaced)
    {
        log_definition_error(
            "Unable to build EcuFlash definition catalog",
            replaced.error());
        return configValues;
    }

    for (QString& address : configValues->ecuflash_def_cal_id_addr)
    {
        if (address.startsWith("0x"))
        {
            address.remove(0, 2);
        }
    }
    std::set<QString> sources;
    for (const QString& source : configValues->ecuflash_def_filename)
    {
        sources.insert(source);
    }
    emit LOG_D(
        QString::number(sources.size()) +
            " EcuFlash definition files found",
        true,
        true);
    emit LOG_D(
        QString::number(configValues->ecuflash_def_cal_id.size()) +
            " EcuFlash ecu id's found",
        true,
        true);
    return configValues;
}

QString FileActions::parse_strict_bool_attribute(
    const QDomElement& element,
    const QString& attrName,
    const QString& tableName)
{
    if (!element.hasAttribute(attrName))
    {
        return "false";
    }
    const QString value = element.attribute(attrName);
    if (value == "true" || value == "false")
    {
        return value;
    }
    emit LOG_W(
        "table " + tableName + ": " + attrName +
            " must be true or false, not [" + value + "]",
        true,
        true);
    return "false";
}

FileActions::EcuCalDefStructure *FileActions::read_ecuflash_ecu_def(
    EcuCalDefStructure *ecuCalDef,
    const QString& cal_id)
{
    if (ConfigValuesStruct.ecuflash_def_cal_id.isEmpty())
    {
        return nullptr;
    }
    emit LOG_D("Search ID: " + cal_id, true, true);
    if (!ConfigValuesStruct.ecuflash_def_cal_id.contains(cal_id))
    {
        return ecuCalDef;
    }

    const QString source = definition_source(
        fastecu::definition::DefinitionFormat::EcuFlash,
        cal_id);
    emit LOG_D("EcuFlash ID found: " + cal_id + " " + cal_id, true, true);
    emit LOG_D("EcuFlash def file name: " + source, true, true);

    auto catalog =
        build_definition_catalog(
            fastecu::definition::DefinitionFormat::EcuFlash);
    if (!catalog)
    {
        log_definition_error(
            "Unable to read EcuFlash definition " + cal_id,
            catalog.error());
        if (!source.isEmpty() &&
            !definitionFileSystem_.exists(ecuflashUtf8(source)))
        {
            QMessageBox::warning(
                this,
                tr("Ecu definitions file"),
                "Unable to open ECU definition file " + source +
                    " for reading");
            return nullptr;
        }
        return ecuCalDef;
    }

    const fastecu::Status replaced = definitionAdapter_.replace_definition(
        *ecuCalDef,
        *catalog,
        fastecu::definition::DefinitionFormat::EcuFlash,
        ecuflashUtf8(cal_id));
    if (!replaced)
    {
        log_definition_error(
            "Unable to read EcuFlash definition " + cal_id,
            replaced.error());
        if (!source.isEmpty() &&
            !definitionFileSystem_.exists(ecuflashUtf8(source)))
        {
            QMessageBox::warning(
                this,
                tr("Ecu definitions file"),
                "Unable to open ECU definition file " + source +
                    " for reading");
            return nullptr;
        }
        return ecuCalDef;
    }

    normalize_definition_addresses(*ecuCalDef);
    emit LOG_D("Found ID: " + cal_id, true, true);
    emit LOG_D(
        "Definition for CAL ID " + cal_id +
            " succesfully read, start parsing definition scalings...",
        true,
        true);
    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::parse_ecuflash_def_scalings(
    EcuCalDefStructure *ecuCalDef)
{
    if (ecuCalDef->RomInfo.size() <= XmlId)
    {
        return ecuCalDef;
    }
    const QString definitionId = ecuCalDef->RomInfo.at(XmlId).trimmed();
    if (definitionId.isEmpty())
    {
        return ecuCalDef;
    }

    auto catalog =
        build_definition_catalog(
            fastecu::definition::DefinitionFormat::EcuFlash);
    if (!catalog)
    {
        log_definition_error(
            "Unable to parse EcuFlash definition scalings",
            catalog.error());
        return ecuCalDef;
    }
    const fastecu::Status replaced = definitionAdapter_.replace_definition(
        *ecuCalDef,
        *catalog,
        fastecu::definition::DefinitionFormat::EcuFlash,
        ecuflashUtf8(definitionId));
    if (!replaced)
    {
        log_definition_error(
            "Unable to parse EcuFlash definition scalings",
            replaced.error());
    }
    else
    {
        normalize_definition_addresses(*ecuCalDef);
    }
    return ecuCalDef;
}

FileActions::EcuCalDefStructure *FileActions::add_ecuflash_def_list_item(
    EcuCalDefStructure *ecuCalDef)
{
    ecuCalDef->IdList.append(" ");
    ecuCalDef->TypeList.append(" ");
    ecuCalDef->NameList.append(" ");
    ecuCalDef->AddressList.append(" ");
    ecuCalDef->CategoryList.append(" ");
    ecuCalDef->CategoryExpandedList.append(" ");
    ecuCalDef->SubCategoryList.append(" ");
    ecuCalDef->LevelList.append(" ");
    ecuCalDef->UserLevelList.append(" ");
    ecuCalDef->SwapXYList.append(" ");
    ecuCalDef->FlipXList.append(" ");
    ecuCalDef->FlipYList.append(" ");
    ecuCalDef->XSizeList.append(" ");
    ecuCalDef->YSizeList.append(" ");
    ecuCalDef->StartPosList.append(" ");
    ecuCalDef->IntervalList.append(" ");
    ecuCalDef->MinValueList.append(" ");
    ecuCalDef->MaxValueList.append(" ");
    ecuCalDef->UnitsList.append(" ");
    ecuCalDef->FormatList.append(" ");
    ecuCalDef->FineIncList.append(" ");
    ecuCalDef->CoarseIncList.append(" ");
    ecuCalDef->VisibleList.append(" ");
    ecuCalDef->SelectionsNameList.append(" ");
    ecuCalDef->SelectionsValueList.append(" ");
    ecuCalDef->DescriptionList.append(" ");
    ecuCalDef->StateList.append(" ");
    ecuCalDef->MapScalingNameList.append(" ");
    ecuCalDef->MapData.append(" ");
    ecuCalDef->MapCellColorMin.append(" ");
    ecuCalDef->MapCellColorMax.append(" ");
    ecuCalDef->XScaleTypeList.append(" ");
    ecuCalDef->XScaleNameList.append(" ");
    ecuCalDef->XScaleAddressList.append(" ");
    ecuCalDef->XScaleStartPosList.append(" ");
    ecuCalDef->XScaleIntervalList.append(" ");
    ecuCalDef->XScaleMinValueList.append(" ");
    ecuCalDef->XScaleMaxValueList.append(" ");
    ecuCalDef->XScaleUnitsList.append(" ");
    ecuCalDef->XScaleFormatList.append(" ");
    ecuCalDef->XScaleFineIncList.append(" ");
    ecuCalDef->XScaleCoarseIncList.append(" ");
    ecuCalDef->XScaleStorageTypeList.append(" ");
    ecuCalDef->XScaleEndianList.append(" ");
    ecuCalDef->XScaleLogParamList.append(" ");
    ecuCalDef->XScaleFromByteList.append(" ");
    ecuCalDef->XScaleToByteList.append(" ");
    ecuCalDef->XScaleStaticDataList.append(" ");
    ecuCalDef->XScaleScalingNameList.append(" ");
    ecuCalDef->XScaleData.append(" ");
    ecuCalDef->YScaleTypeList.append(" ");
    ecuCalDef->YScaleNameList.append(" ");
    ecuCalDef->YScaleAddressList.append(" ");
    ecuCalDef->YScaleStartPosList.append(" ");
    ecuCalDef->YScaleIntervalList.append(" ");
    ecuCalDef->YScaleMinValueList.append(" ");
    ecuCalDef->YScaleMaxValueList.append(" ");
    ecuCalDef->YScaleUnitsList.append(" ");
    ecuCalDef->YScaleFormatList.append(" ");
    ecuCalDef->YScaleFineIncList.append(" ");
    ecuCalDef->YScaleCoarseIncList.append(" ");
    ecuCalDef->YScaleStorageTypeList.append(" ");
    ecuCalDef->YScaleEndianList.append(" ");
    ecuCalDef->YScaleLogParamList.append(" ");
    ecuCalDef->YScaleFromByteList.append(" ");
    ecuCalDef->YScaleToByteList.append(" ");
    ecuCalDef->YScaleScalingNameList.append(" ");
    ecuCalDef->YScaleData.append(" ");
    ecuCalDef->StorageTypeList.append(" ");
    ecuCalDef->EndianList.append(" ");
    ecuCalDef->LogParamList.append(" ");
    ecuCalDef->FromByteList.append(" ");
    ecuCalDef->ToByteList.append(" ");
    ecuCalDef->MapDefined.append(" ");
    return ecuCalDef;
}
