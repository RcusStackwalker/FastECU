#include <QCommandLineParser>
#include "src/ui/desktop/mainwindow.h"
#include "apps/desktop/desktop_composition.h"
#include "apps/desktop/startup_diagnostics.h"

#include <QApplication>

#include <algorithm>
#include <cstdlib>
#include <span>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
    QCommandLineParser cmdParser;
    cmdParser.setApplicationDescription("FastECU - software to work on and modify ECUs");
    cmdParser.addHelpOption();
    QCommandLineOption cmdHost(QStringList() << "s" << "host",
                               "Remote host address and port, for example 127.0.0.1:33314, local:33315", "host:port");
    cmdParser.addOption(cmdHost);
    QCommandLineOption cmdPassword(QStringList() << "p" << "password", "Remote host password", "password");
    cmdParser.addOption(cmdPassword);
    QCommandLineOption cmdDebug(QStringList() << "d" << "debug", "Enable console debug output");
    cmdParser.addOption(cmdDebug);

    // Locate debug option before QCommandLineParser to open console properly
#ifdef _WIN32
    const auto debug_console = std::ranges::any_of(std::span(argv + 1, argc - 1),
                                                   [](std::string_view arg)
                                                   {
                                                       using namespace std::string_view_literals;
                                                       return arg == "-d"sv || arg == "--debug"sv || arg == "-debug"sv;
                                                   });

    if (!debug_console)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
    }
    else
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
        {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
    }
#endif

    int return_code;

    do
    {
        QApplication a(argc, argv);

        // Parser works only after QApplication initialization
        cmdParser.process(a);

        QString addr = cmdParser.value(cmdHost);
        QString password = cmdParser.value(cmdPassword);

        // Declared before the window so it outlives it: MainWindow holds
        // references into the composition until it is destroyed.
        DesktopComposition composition{addr, password};
        if (!composition.started())
        {
            present_startup_failure(*composition.startup_error());
            return_code = EXIT_FAILURE;
            break;
        }
        present_startup_warnings(composition.startup_warnings());
        MainWindow w(composition.services(), addr);

        QScreen *screen = QGuiApplication::primaryScreen();
        QRect screenGeometry = screen->geometry();
        w.move(screenGeometry.center() - w.rect().center());

        w.show();

        return_code = a.exec();
    } while (return_code == RESTART_CODE);

    return return_code;
}
