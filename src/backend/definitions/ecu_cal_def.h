#ifndef FASTECU_BACKEND_DEFINITIONS_ECU_CAL_DEF_H
#define FASTECU_BACKEND_DEFINITIONS_ECU_CAL_DEF_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace fastecu::definitions
{

struct EcuCalDefStructure
{
    bool operator==(const EcuCalDefStructure&) const = default;

    QString FileName;
    QString DefinitionFileName;
    QString FullFileName;
    QString FileSize;
    QStringList IdList;
    QStringList TypeList;
    QStringList NameList;
    QStringList AddressList;
    QStringList CategoryList;
    QStringList CategoryExpandedList;
    QStringList SubCategoryList;
    QStringList LevelList;
    QStringList UserLevelList;
    QStringList SwapXYList;
    QStringList FlipXList;
    QStringList FlipYList;
    QStringList XSizeList;
    QStringList YSizeList;
    QStringList StartPosList;
    QStringList IntervalList;
    QStringList MinValueList;
    QStringList MaxValueList;
    QStringList UnitsList;
    QStringList FormatList;
    QStringList FineIncList;
    QStringList CoarseIncList;
    QStringList VisibleList;
    QStringList SelectionsNameList;
    QStringList SelectionsValueList;
    QStringList DescriptionList;
    QStringList StateList;
    QStringList MapScalingNameList;
    QStringList MapData;
    QStringList MapCellColorMin;
    QStringList MapCellColorMax;

    QStringList XScaleTypeList;
    QStringList XScaleNameList;
    QStringList XScaleAddressList;
    QStringList XScaleStartPosList;
    QStringList XScaleIntervalList;
    QStringList XScaleMinValueList;
    QStringList XScaleMaxValueList;
    QStringList XScaleUnitsList;
    QStringList XScaleFormatList;
    QStringList XScaleFineIncList;
    QStringList XScaleCoarseIncList;
    QStringList XScaleStorageTypeList;
    QStringList XScaleEndianList;
    QStringList XScaleLogParamList;
    QStringList XScaleFromByteList;
    QStringList XScaleToByteList;
    QStringList XScaleStaticDataList;
    QStringList XScaleScalingNameList;
    QStringList XScaleData;

    QStringList YScaleTypeList;
    QStringList YScaleNameList;
    QStringList YScaleAddressList;
    QStringList YScaleStartPosList;
    QStringList YScaleIntervalList;
    QStringList YScaleMinValueList;
    QStringList YScaleMaxValueList;
    QStringList YScaleUnitsList;
    QStringList YScaleFormatList;
    QStringList YScaleFineIncList;
    QStringList YScaleCoarseIncList;
    QStringList YScaleStorageTypeList;
    QStringList YScaleEndianList;
    QStringList YScaleLogParamList;
    QStringList YScaleFromByteList;
    QStringList YScaleToByteList;
    QStringList YScaleScalingNameList;
    QStringList YScaleData;

    QStringList ScalingNameList;
    QStringList ScalingUnitsList;
    QStringList ScalingFromByteList;
    QStringList ScalingToByteList;
    QStringList ScalingFormatList;
    QStringList ScalingMinValueList;
    QStringList ScalingMaxValueList;
    QStringList ScalingCoarseIncList;
    QStringList ScalingFineIncList;
    QStringList ScalingStorageTypeList;
    QStringList ScalingEndianList;
    QStringList ScalingSelectionsNameList;
    QStringList ScalingSelectionsValueList;

    QStringList RomInfo;
    QString RomInfoExpanded;
    QString RomBase;
    QString RomId;
    QString Kernel;
    QString KernelStartAddr;
    QString FlashMethod;
    QString McuType;

    QStringList StorageTypeList;
    QStringList EndianList;
    QStringList LogParamList;
    QStringList FromByteList;
    QStringList ToByteList;
    QStringList MapDefined;

    QByteArray FullRomData;
    bool OemEcuFile;
    bool SyncedWithEcu;
    bool use_romraider_definition;
    bool use_ecuflash_definition;

    QStringList RomInfoStrings = {
        "XML ID",
        "Internal ID Address",
        "Internal ID String",
        "ECU ID",
        "Make",
        "Market",
        "Model",
        "Submodel",
        "Transmission",
        "Year",
        "Flash Method",
        "Memory Model",
        "Checksum Module",
        "Rom Base",
        "File Size",
        "Def File",
    };

    QStringList RomInfoNames = {
        "xmlid",
        "internalidaddress",
        "internalidstring",
        "ecuid",
        "make",
        "market",
        "model",
        "submodel",
        "transmission",
        "year",
        "flashmethod",
        "memmodel",
        "checksummodule",
        "rombase",
        "filesize",
        "deffile",
    };

    QStringList DefHeaderStrings = {
        "XML ID",
        "Internal ID Address",
        "Internal ID String",
        "ECU ID",
        "Make",
        "Market",
        "Model",
        "Submodel",
        "Transmission",
        "Year",
        "Flash Method",
        "Memory Model",
        "Checksum Module",
        "Include",
        "Notes",
    };

    QStringList DefHeaderNames = {
        "xmlid",
        "internalidaddress",
        "internalidstring",
        "ecuid",
        "make",
        "market",
        "model",
        "submodel",
        "transmission",
        "year",
        "flashmethod",
        "memmodel",
        "checksummodule",
        "include",
        "notes",
    };
};

} // namespace fastecu::definitions

#endif // FASTECU_BACKEND_DEFINITIONS_ECU_CAL_DEF_H
