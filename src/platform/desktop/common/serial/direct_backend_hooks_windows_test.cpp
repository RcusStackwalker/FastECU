#include <QtTest>

#include "src/platform/desktop/common/serial/serial_port_actions_direct.h"

// Exposes the pure per-OS hooks of the direct backend.
class HookProbe : public SerialPortActionsDirect
{
  public:
    using SerialPortActionsDirect::j2534_tx_done;
    using SerialPortActionsDirect::resolve_port;
};

class TestDirectBackendHooksWindows : public QObject
{
    Q_OBJECT
  private slots:
    // Windows entries are J2534 vendor names: no split, every non-empty one is J2534.
    void resolvePort_keepsTheVendorNameWhole()
    {
        HookProbe probe;
        const auto resolved = probe.resolve_port("Tactrix Inc. - OpenPort 2.0 J2534 DLL");
        QCOMPARE(resolved.port, QString("Tactrix Inc. - OpenPort 2.0 J2534 DLL"));
        QVERIFY(resolved.is_j2534);
    }

    void resolvePort_emptyEntryIsNotJ2534()
    {
        HookProbe probe;
        QVERIFY(!probe.resolve_port("").is_j2534);
    }

    void txDone_isAlwaysTrue()
    {
        HookProbe probe;
        QVERIFY(probe.j2534_tx_done());
    }
};

QTEST_GUILESS_MAIN(TestDirectBackendHooksWindows)
#include "direct_backend_hooks_windows_test.moc"
