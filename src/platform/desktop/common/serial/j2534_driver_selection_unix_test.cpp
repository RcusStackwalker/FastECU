#include <QtTest>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

// Unix check_serial_ports() entries are "<portName> - <description>"; only the
// description tells an adapter from a Bluetooth or debug-console port (#243).
class TestJ2534DriverSelectionUnix : public QObject
{
    Q_OBJECT
  private slots:
    void capableEntry_matchesOnlyTheAdapterDescription()
    {
        QVERIFY(isJ2534CapableEntry(u"cu.usbmodemTApU_RJO1 - OpenPort 2.0"));
        QVERIFY(isJ2534CapableEntry(u"cu.usbmodem0 - openport 2.0")); // case-insensitive
        // macOS enumerates these ahead of the adapter; driving ISO-15765 over one
        // yields a timeout per exchange, never a response.
        QVERIFY(!isJ2534CapableEntry(u"cu.Bluetooth-Incoming-Port - "));
        QVERIFY(!isJ2534CapableEntry(u"cu.debug-console - "));
        QVERIFY(!isJ2534CapableEntry(u"ttyUSB0 - USB Serial"));
        QVERIFY(!isJ2534CapableEntry(u"ttyUSB0")); // no separator at all
        QVERIFY(!isJ2534CapableEntry(u""));
    }
};

QTEST_GUILESS_MAIN(TestJ2534DriverSelectionUnix)
#include "j2534_driver_selection_unix_test.moc"
