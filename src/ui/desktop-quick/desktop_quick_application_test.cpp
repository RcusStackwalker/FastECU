#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QString>
#include <QtTest>

namespace
{
class DesktopQuickApplicationTest : public QObject
{
    Q_OBJECT
  private slots:
    void loadsEmbeddedApplicationShell()
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QQmlApplicationEngine engine;
        QVERIFY(fastecu::desktop_quick::load_root(engine));
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().front();
        QCOMPARE(root->objectName(), QStringLiteral("desktopQuickRoot"));
        QCOMPARE(root->property("title").toString(), QStringLiteral("OmniHaste"));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("navigationRail")) != nullptr);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("dashboardNavigation")) != nullptr);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("workspace")) != nullptr);
    }
};
} // namespace

QTEST_MAIN(DesktopQuickApplicationTest)
#include "desktop_quick_application_test.moc"
