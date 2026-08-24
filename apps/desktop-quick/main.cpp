#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QString>
#include <QStringList>

#include <cstdlib>

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OmniHaste"));
    QCoreApplication::setApplicationName(QStringLiteral("OmniHaste"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;
    if (!fastecu::desktop_quick::load_root(engine))
    {
        return EXIT_FAILURE;
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test")))
    {
        return EXIT_SUCCESS;
    }
    return QGuiApplication::exec();
}
