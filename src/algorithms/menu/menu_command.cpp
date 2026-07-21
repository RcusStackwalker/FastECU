#include "src/algorithms/menu/menu_command.h"

#include <QLatin1StringView>

#include <array>

namespace
{
struct MenuCommandMapping
{
    QLatin1StringView id;
    MenuCommand command;
};

using namespace Qt::StringLiterals;

constexpr std::array<MenuCommandMapping, 37> kMenuCommandMappings = {
    MenuCommandMapping{"new"_L1, MenuCommand::New},
    {"open_calibration"_L1, MenuCommand::OpenCalibration},
    {"save_calibration"_L1, MenuCommand::SaveCalibration},
    {"save_calibration_as"_L1, MenuCommand::SaveCalibrationAs},
    {"close_calibration"_L1, MenuCommand::CloseCalibration},
    {"quit"_L1, MenuCommand::Quit},
    {"undo"_L1, MenuCommand::Undo},
    {"redo"_L1, MenuCommand::Redo},
    {"copy"_L1, MenuCommand::Copy},
    {"paste"_L1, MenuCommand::Paste},
    {"winols_csv_to_romraider_xml"_L1, MenuCommand::WinolsCsvToRomRaiderXml},
    {"settings"_L1, MenuCommand::Settings},
    {"fine_inc"_L1, MenuCommand::FineIncrement},
    {"fine_dec"_L1, MenuCommand::FineDecrement},
    {"coarse_inc"_L1, MenuCommand::CoarseIncrement},
    {"coarse_dec"_L1, MenuCommand::CoarseDecrement},
    {"set_value"_L1, MenuCommand::SetValue},
    {"interpolate_horizontal"_L1, MenuCommand::InterpolateHorizontal},
    {"interpolate_vertical"_L1, MenuCommand::InterpolateVertical},
    {"interpolate_bidirectional"_L1, MenuCommand::InterpolateBidirectional},
    {"toggle_realtime"_L1, MenuCommand::ToggleRealtime},
    {"log_to_file"_L1, MenuCommand::LogToFile},
    {"connect_to_ecu"_L1, MenuCommand::ConnectToEcu},
    {"disconnect_from_ecu"_L1, MenuCommand::DisconnectFromEcu},
    {"read_rom_from_ecu"_L1, MenuCommand::ReadRomFromEcu},
    {"test_write_rom_to_ecu"_L1, MenuCommand::TestWriteRomToEcu},
    {"write_rom_to_ecu"_L1, MenuCommand::WriteRomToEcu},
    {"setlogviews"_L1, MenuCommand::SetLogViews},
    {"dtc_window"_L1, MenuCommand::DtcWindow},
    {"hex_editor"_L1, MenuCommand::HexEditor},
    {"haltech_ic7"_L1, MenuCommand::HaltechIc7},
    {"simulate_obd"_L1, MenuCommand::SimulateObd},
    {"can_listener"_L1, MenuCommand::CanListener},
    {"biu_communication"_L1, MenuCommand::BiuCommunication},
    {"get_key"_L1, MenuCommand::GetKey},
    {"terminal"_L1, MenuCommand::Terminal},
    {"about"_L1, MenuCommand::About},
};
} // namespace

MenuCommand menu_command_from_id(const QString& id)
{
    for (const MenuCommandMapping& mapping : kMenuCommandMappings)
    {
        if (id == mapping.id)
        {
            return mapping.command;
        }
    }
    return MenuCommand::Unknown;
}

QString menu_command_id(MenuCommand command)
{
    for (const MenuCommandMapping& mapping : kMenuCommandMappings)
    {
        if (command == mapping.command)
        {
            mapping.id.toString();
        }
    }
    return QString();
}
