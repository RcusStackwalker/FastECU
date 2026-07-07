#include <QtTest>

#include <menu_command.h>

#include "test_menu_command.h"

class TestMenuCommand : public QObject
{
    Q_OBJECT

  private slots:
    void activeMenuIds_mapToCommands_data()
    {
        QTest::addColumn<QString>("id");
        QTest::addColumn<MenuCommand>("command");

        QTest::newRow("open_calibration") << QStringLiteral("open_calibration") << MenuCommand::OpenCalibration;
        QTest::newRow("save_calibration") << QStringLiteral("save_calibration") << MenuCommand::SaveCalibration;
        QTest::newRow("save_calibration_as") << QStringLiteral("save_calibration_as") << MenuCommand::SaveCalibrationAs;
        QTest::newRow("close_calibration") << QStringLiteral("close_calibration") << MenuCommand::CloseCalibration;
        QTest::newRow("quit") << QStringLiteral("quit") << MenuCommand::Quit;
        QTest::newRow("copy") << QStringLiteral("copy") << MenuCommand::Copy;
        QTest::newRow("paste") << QStringLiteral("paste") << MenuCommand::Paste;
        QTest::newRow("settings") << QStringLiteral("settings") << MenuCommand::Settings;
        QTest::newRow("coarse_inc") << QStringLiteral("coarse_inc") << MenuCommand::CoarseIncrement;
        QTest::newRow("coarse_dec") << QStringLiteral("coarse_dec") << MenuCommand::CoarseDecrement;
        QTest::newRow("fine_inc") << QStringLiteral("fine_inc") << MenuCommand::FineIncrement;
        QTest::newRow("fine_dec") << QStringLiteral("fine_dec") << MenuCommand::FineDecrement;
        QTest::newRow("set_value") << QStringLiteral("set_value") << MenuCommand::SetValue;
        QTest::newRow("interpolate_horizontal") << QStringLiteral("interpolate_horizontal") << MenuCommand::InterpolateHorizontal;
        QTest::newRow("interpolate_vertical") << QStringLiteral("interpolate_vertical") << MenuCommand::InterpolateVertical;
        QTest::newRow("interpolate_bidirectional") << QStringLiteral("interpolate_bidirectional") << MenuCommand::InterpolateBidirectional;
        QTest::newRow("connect_to_ecu") << QStringLiteral("connect_to_ecu") << MenuCommand::ConnectToEcu;
        QTest::newRow("disconnect_from_ecu") << QStringLiteral("disconnect_from_ecu") << MenuCommand::DisconnectFromEcu;
        QTest::newRow("toggle_realtime") << QStringLiteral("toggle_realtime") << MenuCommand::ToggleRealtime;
        QTest::newRow("log_to_file") << QStringLiteral("log_to_file") << MenuCommand::LogToFile;
        QTest::newRow("read_rom_from_ecu") << QStringLiteral("read_rom_from_ecu") << MenuCommand::ReadRomFromEcu;
        QTest::newRow("test_write_rom_to_ecu") << QStringLiteral("test_write_rom_to_ecu") << MenuCommand::TestWriteRomToEcu;
        QTest::newRow("write_rom_to_ecu") << QStringLiteral("write_rom_to_ecu") << MenuCommand::WriteRomToEcu;
        QTest::newRow("setlogviews") << QStringLiteral("setlogviews") << MenuCommand::SetLogViews;
        QTest::newRow("dtc_window") << QStringLiteral("dtc_window") << MenuCommand::DtcWindow;
        QTest::newRow("hex_editor") << QStringLiteral("hex_editor") << MenuCommand::HexEditor;
        QTest::newRow("terminal") << QStringLiteral("terminal") << MenuCommand::Terminal;
        QTest::newRow("biu_communication") << QStringLiteral("biu_communication") << MenuCommand::BiuCommunication;
        QTest::newRow("get_key") << QStringLiteral("get_key") << MenuCommand::GetKey;
        QTest::newRow("winols_csv_to_romraider_xml") << QStringLiteral("winols_csv_to_romraider_xml") << MenuCommand::WinolsCsvToRomRaiderXml;
        QTest::newRow("about") << QStringLiteral("about") << MenuCommand::About;
    }

    void activeMenuIds_mapToCommands()
    {
        QFETCH(QString, id);
        QFETCH(MenuCommand, command);

        QCOMPARE(menu_command_from_id(id), command);
        QCOMPARE(menu_command_id(command), id);
    }

    void unknownIds_returnUnknown()
    {
        QCOMPARE(menu_command_from_id(QStringLiteral("separator")), MenuCommand::Unknown);
        QCOMPARE(menu_command_from_id(QStringLiteral("missing_command")), MenuCommand::Unknown);
        QVERIFY(menu_command_id(MenuCommand::Unknown).isEmpty());
    }
};

int run_test_menu_command(int argc, char **argv)
{
    TestMenuCommand t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_menu_command.moc"
