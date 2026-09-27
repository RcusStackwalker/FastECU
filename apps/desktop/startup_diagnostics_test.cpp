#include <QDir>
#include <QTest>

#include "apps/desktop/default_config_root.h"
#include "apps/desktop/startup_diagnostics.h"

class StartupDiagnosticsTest : public QObject
{
    Q_OBJECT

  private slots:
    void failureTextCarriesTheDetail()
    {
        const QString text = startup_failure_text(
            fastecu::Error{fastecu::ErrorKind::InvalidConfig, "Unable to load protocols /r/protocols.cfg: bad"});
        QVERIFY(text.contains("/r/protocols.cfg"));
        QVERIFY(text.contains("bad"));
    }

    void warningTextListsEveryWarning()
    {
        const QString text = startup_warning_text({"first /a.cfg", "second"});
        QVERIFY(text.contains("first /a.cfg"));
        QVERIFY(text.contains("second"));
    }

    void defaultRootIsUnderHomeAndEndsInFastEcu()
    {
        const QString root = default_config_root();
        QVERIFY(root.startsWith(QDir::homePath() + "/"));
        QVERIFY(root.endsWith("/FastECU/"));
    }
};

QTEST_MAIN(StartupDiagnosticsTest)
#include "startup_diagnostics_test.moc"
