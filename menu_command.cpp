#include "menu_command.h"

namespace {
struct MenuCommandMapping {
    const char *id;
    MenuCommand command;
};

constexpr MenuCommandMapping kMenuCommandMappings[] = {
    {"new", MenuCommand::New},
    {"open_calibration", MenuCommand::OpenCalibration},
    {"save_calibration", MenuCommand::SaveCalibration},
    {"save_calibration_as", MenuCommand::SaveCalibrationAs},
    {"close_calibration", MenuCommand::CloseCalibration},
    {"quit", MenuCommand::Quit},
    {"undo", MenuCommand::Undo},
    {"redo", MenuCommand::Redo},
    {"copy", MenuCommand::Copy},
    {"paste", MenuCommand::Paste},
    {"winols_csv_to_romraider_xml", MenuCommand::WinolsCsvToRomRaiderXml},
    {"settings", MenuCommand::Settings},
    {"fine_inc", MenuCommand::FineIncrement},
    {"fine_dec", MenuCommand::FineDecrement},
    {"coarse_inc", MenuCommand::CoarseIncrement},
    {"coarse_dec", MenuCommand::CoarseDecrement},
    {"set_value", MenuCommand::SetValue},
    {"interpolate_horizontal", MenuCommand::InterpolateHorizontal},
    {"interpolate_vertical", MenuCommand::InterpolateVertical},
    {"interpolate_bidirectional", MenuCommand::InterpolateBidirectional},
    {"toggle_realtime", MenuCommand::ToggleRealtime},
    {"log_to_file", MenuCommand::LogToFile},
    {"connect_to_ecu", MenuCommand::ConnectToEcu},
    {"disconnect_from_ecu", MenuCommand::DisconnectFromEcu},
    {"read_rom_from_ecu", MenuCommand::ReadRomFromEcu},
    {"test_write_rom_to_ecu", MenuCommand::TestWriteRomToEcu},
    {"write_rom_to_ecu", MenuCommand::WriteRomToEcu},
    {"setlogviews", MenuCommand::SetLogViews},
    {"dtc_window", MenuCommand::DtcWindow},
    {"hex_editor", MenuCommand::HexEditor},
    {"haltech_ic7", MenuCommand::HaltechIc7},
    {"simulate_obd", MenuCommand::SimulateObd},
    {"can_listener", MenuCommand::CanListener},
    {"biu_communication", MenuCommand::BiuCommunication},
    {"get_key", MenuCommand::GetKey},
    {"terminal", MenuCommand::Terminal},
    {"about", MenuCommand::About},
};
}

MenuCommand menu_command_from_id(const QString &id)
{
    for (const MenuCommandMapping &mapping : kMenuCommandMappings) {
        if (id == QLatin1String(mapping.id))
            return mapping.command;
    }
    return MenuCommand::Unknown;
}

QString menu_command_id(MenuCommand command)
{
    for (const MenuCommandMapping &mapping : kMenuCommandMappings) {
        if (command == mapping.command)
            return QString::fromLatin1(mapping.id);
    }
    return QString();
}
