#ifndef MENU_COMMAND_H
#define MENU_COMMAND_H

#include <string>
#include <string_view>

enum class MenuCommand
{
    Unknown,
    New,
    OpenCalibration,
    SaveCalibration,
    SaveCalibrationAs,
    CloseCalibration,
    Quit,
    Undo,
    Redo,
    Copy,
    Paste,
    WinolsCsvToRomRaiderXml,
    Settings,
    FineIncrement,
    FineDecrement,
    CoarseIncrement,
    CoarseDecrement,
    SetValue,
    InterpolateHorizontal,
    InterpolateVertical,
    InterpolateBidirectional,
    ToggleRealtime,
    LogToFile,
    ConnectToEcu,
    DisconnectFromEcu,
    ReadRomFromEcu,
    TestWriteRomToEcu,
    WriteRomToEcu,
    SetLogViews,
    DtcWindow,
    HexEditor,
    HaltechIc7,
    SimulateObd,
    CanListener,
    BiuCommunication,
    GetKey,
    Terminal,
    About,
};

MenuCommand menu_command_from_id(std::string_view id);
std::string menu_command_id(MenuCommand command);

#endif // MENU_COMMAND_H
