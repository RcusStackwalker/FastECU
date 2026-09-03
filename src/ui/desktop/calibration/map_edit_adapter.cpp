#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <bit>
#include <utility>

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

calibration::MapElementSpec MapElementFields::spec() const&
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
    // FromByteList/MinValueList/MaxValueList/CoarseIncList/FineIncList use
    // QStringList::value() rather than at() -- consistent with RomInfo below.
    // paste_value (menu_actions.cpp) is a MapBody-only, no-clamp, no-from_byte
    // caller: on master it never read any of these five fields, so a
    // caller-supplied def with one of them shorter than expected (a
    // hand-built test fixture, or a ragged real-world definition) must not
    // newly crash the adapter merely because collect_map_element_fields
    // gathers the whole spec unconditionally. AddressList/StorageTypeList/
    // EndianList/ToByteList stay on at(): every caller needs those four to
    // do anything meaningful, so an out-of-range index there is a genuine
    // malformed-definition bug worth surfacing loudly rather than papering
    // over with a silent default.
    case calibration::EditTargetKind::MapBody:
        address_text = def.AddressList.at(map_number);
        storage_type_text = def.StorageTypeList.at(map_number);
        fields.endian_ = def.EndianList.at(map_number).toStdString();
        fields.to_byte_ = def.ToByteList.at(map_number).toStdString();
        fields.from_byte_ = def.FromByteList.value(map_number).toStdString();
        fields.min_value_ = def.MinValueList.value(map_number).toStdString();
        fields.max_value_ = def.MaxValueList.value(map_number).toStdString();
        fields.coarse_increment_ = def.CoarseIncList.value(map_number).toDouble();
        fields.fine_increment_ = def.FineIncList.value(map_number).toDouble();
        break;
    case calibration::EditTargetKind::XAxis:
        address_text = def.XScaleAddressList.at(map_number);
        storage_type_text = def.XScaleStorageTypeList.at(map_number);
        fields.endian_ = def.XScaleEndianList.at(map_number).toStdString();
        fields.to_byte_ = def.XScaleToByteList.at(map_number).toStdString();
        fields.from_byte_ = def.XScaleFromByteList.value(map_number).toStdString();
        fields.min_value_ = def.XScaleMinValueList.value(map_number).toStdString();
        fields.max_value_ = def.XScaleMaxValueList.value(map_number).toStdString();
        fields.coarse_increment_ = def.XScaleCoarseIncList.value(map_number).toDouble();
        fields.fine_increment_ = def.XScaleFineIncList.value(map_number).toDouble();
        break;
    case calibration::EditTargetKind::YAxis:
        address_text = def.YScaleAddressList.at(map_number);
        storage_type_text = def.YScaleStorageTypeList.at(map_number);
        fields.endian_ = def.YScaleEndianList.at(map_number).toStdString();
        fields.to_byte_ = def.YScaleToByteList.at(map_number).toStdString();
        fields.from_byte_ = def.YScaleFromByteList.value(map_number).toStdString();
        fields.min_value_ = def.YScaleMinValueList.value(map_number).toStdString();
        fields.max_value_ = def.YScaleMaxValueList.value(map_number).toStdString();
        fields.coarse_increment_ = def.YScaleCoarseIncList.value(map_number).toDouble();
        fields.fine_increment_ = def.YScaleFineIncList.value(map_number).toDouble();
        break;
    case calibration::EditTargetKind::Rejected:
        // A programming error at this point -- the caller resolves the edit
        // target before collecting fields for it. `assert(false)` compiles
        // out under NDEBUG (--config=release, how this ships), which would
        // let a caller that got this wrong fall through to a
        // default-constructed MapElementFields -- address 0, no storage
        // type -- and write_rom_data_value would go pack bytes at ROM offset
        // 0. std::unreachable() cannot silently continue: reaching it is
        // undefined behavior by contract, not a soft default. All three
        // current call sites resolve the edit target before calling this
        // function, so this case is genuinely unreachable, not a recoverable
        // runtime error -- a Result-returning signature would be the more
        // idiomatic shape for a reachable failure, but would thread an error
        // path through every caller for a case none of them can hit.
        std::unreachable();
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

QString format_raw_element_value(const calibration::MapElementSpec& spec, std::int64_t raw)
{
    // get_rom_data_value's storagetype.startsWith("float"/"uint"/"int")
    // chain matches nothing for a storage-type string outside the known set,
    // leaving `value` an empty (default-constructed) QString. storage_type_
    // from_text maps that same unrecognized string to std::nullopt, under
    // which is_unsigned_storage is false and storage_byte_size defaults to
    // 1 -- falling into the qint8 case below would return "-1"-like text
    // instead of legacy's "", a real divergence (observable in inc_dec_
    // value's `while (rom_data_value == new_rom_data_value)`), not one of
    // the established, deliberately-preserved defects. Handled first and
    // explicitly so it can't be missed among the branches below.
    if (!spec.storage_type.has_value())
    {
        return QString();
    }
    if (spec.storage_type == definition::StorageType::Float)
    {
        return QString::number(std::bit_cast<float>(static_cast<std::int32_t>(raw)));
    }
    if (definition::is_unsigned_storage(spec.storage_type))
    {
        return QString::number(static_cast<quint32>(raw));
    }
    switch (definition::storage_byte_size(spec.storage_type))
    {
    case 1:
        return QString::number(static_cast<qint8>(raw));
    case 2:
        return QString::number(static_cast<qint16>(raw));
    case 4:
        return QString::number(static_cast<qint32>(raw));
    default:
        return QString();
    }
}

} // namespace fastecu::ui
