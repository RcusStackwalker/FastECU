#include <QtTest>

#include "src/platform/desktop/common/serial/j2534_driver_selection.h"

// Windows entries come from getAllJ2534DriversNames(), carry no description,
// and open_serial_port() drives every one of them through J2534 -- rejecting
// them here would regress Windows.
class TestJ2534DriverSelectionWindows : public QObject
{
    Q_OBJECT
  private slots:
    void capableEntry_acceptsEveryNonEmptyEntry()
    {
        QVERIFY(isJ2534CapableEntry(u"cu.usbmodemTApU_RJO1 - OpenPort 2.0"));
        QVERIFY(isJ2534CapableEntry(u"cu.usbmodem0 - openport 2.0"));
        QVERIFY(isJ2534CapableEntry(u"Tactrix Inc. - OpenPort 2.0 J2534 DLL"));
        QVERIFY(isJ2534CapableEntry(u"Acme J2534 DLL"));
        QVERIFY(!isJ2534CapableEntry(u""));
    }
};

QTEST_GUILESS_MAIN(TestJ2534DriverSelectionWindows)
#include "j2534_driver_selection_windows_test.moc"
