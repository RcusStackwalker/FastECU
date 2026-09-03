#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <cassert>

#include <QDebug>
#include <QString>

namespace fastecu::ui
{
namespace
{

// FileActions::RomInfoEnum::FlashMethod (src/backend/definitions/file_actions.h)
// is 10. Duplicated here as a literal rather than pulling in file_actions.h --
// and with it the full legacy FileActions dependency graph (legacy config /
// calibration / definition adapters) -- into this lightweight adapter package.
constexpr int kFlashMethodRomInfoIndex = 10;

} // namespace

calibration::MapElementSpec MapElementFields::spec() const
{
    calibration::MapElementSpec spec;
    spec.address = address_;
    spec.storage_type = storage_type_;
    spec.endian = endian_;
    spec.to_byte = to_byte_;
    spec.from_byte = from_byte_;
    spec.min_value = min_value_;
    spec.max_value = max_value_;
    spec.coarse_increment = coarse_increment_;
    spec.fine_increment = fine_increment_;
    spec.x_size = x_size_;
    spec.y_size = y_size_;
    spec.flash_method = flash_method_;
    spec.rom_file_size = rom_file_size_;
    return spec;
}

MapElementFields collect_map_element_fields(const definitions::EcuCalDefStructure& def, int map_number,
                                            calibration::EditTargetKind kind)
{
    MapElementFields fields;

    QString address_text;
    QString storage_type_text;

    switch (kind)
    {
    case calibration::EditTargetKind::MapBody:
        address_text = def.AddressList.at(map_number);
        storage_type_text = def.StorageTypeList.at(map_number);
        fields.endian_ = def.EndianList.at(map_number).toStdString();
        fields.to_byte_ = def.ToByteList.at(map_number).toStdString();
        fields.from_byte_ = def.FromByteList.at(map_number).toStdString();
        fields.min_value_ = def.MinValueList.at(map_number).toStdString();
        fields.max_value_ = def.MaxValueList.at(map_number).toStdString();
        fields.coarse_increment_ = def.CoarseIncList.at(map_number).toDouble();
        fields.fine_increment_ = def.FineIncList.at(map_number).toDouble();
        break;
    case calibration::EditTargetKind::XAxis:
        address_text = def.XScaleAddressList.at(map_number);
        storage_type_text = def.XScaleStorageTypeList.at(map_number);
        fields.endian_ = def.XScaleEndianList.at(map_number).toStdString();
        fields.to_byte_ = def.XScaleToByteList.at(map_number).toStdString();
        fields.from_byte_ = def.XScaleFromByteList.at(map_number).toStdString();
        fields.min_value_ = def.XScaleMinValueList.at(map_number).toStdString();
        fields.max_value_ = def.XScaleMaxValueList.at(map_number).toStdString();
        fields.coarse_increment_ = def.XScaleCoarseIncList.at(map_number).toDouble();
        fields.fine_increment_ = def.XScaleFineIncList.at(map_number).toDouble();
        break;
    case calibration::EditTargetKind::YAxis:
        address_text = def.YScaleAddressList.at(map_number);
        storage_type_text = def.YScaleStorageTypeList.at(map_number);
        fields.endian_ = def.YScaleEndianList.at(map_number).toStdString();
        fields.to_byte_ = def.YScaleToByteList.at(map_number).toStdString();
        fields.from_byte_ = def.YScaleFromByteList.at(map_number).toStdString();
        fields.min_value_ = def.YScaleMinValueList.at(map_number).toStdString();
        fields.max_value_ = def.YScaleMaxValueList.at(map_number).toStdString();
        fields.coarse_increment_ = def.YScaleCoarseIncList.at(map_number).toDouble();
        fields.fine_increment_ = def.YScaleFineIncList.at(map_number).toDouble();
        break;
    case calibration::EditTargetKind::Rejected:
        // A programming error at this point -- the caller resolves the edit
        // target before collecting fields for it.
        assert(false && "collect_map_element_fields called with EditTargetKind::Rejected");
        return fields;
    }

    bool address_ok = false;
    const std::uint32_t parsed_address = address_text.toUInt(&address_ok, 16);
    if (!address_ok)
    {
        qWarning() << "collect_map_element_fields: failed to parse hex address" << address_text << "for map"
                   << map_number;
    }
    fields.address_ = parsed_address;

    fields.storage_type_ = definition::storage_type_from_text(storage_type_text.toStdString());

    // X/Y size are always the map's own geometry -- axes don't carry a
    // separate size field in EcuCalDefStructure.
    fields.x_size_ = def.XSizeList.at(map_number).toUInt();
    fields.y_size_ = def.YSizeList.at(map_number).toUInt();

    // QStringList::value(), not at(): a caller-supplied def with a
    // shorter-than-expected RomInfo (e.g. a hand-built test fixture) must not
    // crash the adapter -- it should simply see an empty flash method, same
    // as if the field were present but blank.
    fields.flash_method_ = def.RomInfo.value(kFlashMethodRomInfoIndex).toStdString();
    fields.rom_file_size_ = def.FileSize.toUInt();

    return fields;
}

} // namespace fastecu::ui
