#include <cstdio>

#include <QCoreApplication>

int run_test_facade_threading(int argc, char **argv);
int run_throwing_backend_child();

int main(int argc, char **argv)
{
    // Run the QTest classes' output unbuffered. These suites exercise the
    // serial facade's I/O-thread and QRemoteObjects teardown paths, which have
    // an intermittent, Windows-only crash (tracked separately). When Bazel
    // redirects stdout to test.log it is block-buffered, so a hard crash
    // (access violation, no CRT flush) discards the whole buffer and the
    // failing run shows *zero* output -- making the culprit slot impossible to
    // identify. Unbuffered stdout/stderr lands every "PASS : Class::slot()"
    // line as it happens, so the first slot with no trailing PASS/FAIL line is
    // exactly the one that crashed.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("FASTECU_THROWING_BACKEND_CHILD"))
    {
        return run_throwing_backend_child();
    }

    return run_test_facade_threading(argc, argv);
}
