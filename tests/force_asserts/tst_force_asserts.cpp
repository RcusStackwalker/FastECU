// Proves QT_FORCE_ASSERTS re-arms Qt's bounds-check assertions under release
// (QT_NO_DEBUG) flags. Child does an out-of-range QByteArray access:
//   - asserts armed   -> Q_ASSERT -> qt_assert -> abort()  (SIGABRT, signal 6)
//   - asserts stripped -> Q_ASSERT no-op; verify() is a no-op; g_ba.d.ptr is
//                         nullptr (zero-initialized static), so data[0] is a
//                         null deref -> SIGSEGV (signal 11). RED state.
//
// g_ba is a file-scope static QByteArray so the optimizer cannot constant-fold
// its size into the at() call at compile time; child_body is noinline to
// prevent the entire if(pid==0) branch from being optimized away via
// cross-function UB propagation (which happened with a local QByteArray).
#include <QtTest>
#include <QByteArray>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>

// File-scope static: zero-initialized (ptr=nullptr, size=0). The optimizer
// cannot constant-fold this into child_body's at() call, so child_body is
// emitted and the null-deref (SIGSEGV) or assert (SIGABRT) occurs at runtime.
static QByteArray g_ba;

__attribute__((noinline))
static int child_body()
{
    // g_ba.size() == 0 at runtime; at(0) is out of bounds.
    // With QT_FORCE_ASSERTS: Q_ASSERT fires -> qt_assert -> abort() -> SIGABRT.
    // Without:               Q_ASSERT no-op; data[0] deref's nullptr -> SIGSEGV.
    volatile char c = g_ba.at(0);
    (void)c;
    return 0; // reached only when asserts are stripped
}

class TestForceAsserts : public QObject
{
    Q_OBJECT
private slots:
    void outOfBoundsAtAborts();
};

void TestForceAsserts::outOfBoundsAtAborts()
{
    pid_t pid = fork();
    QVERIFY2(pid >= 0, "fork failed");
    if (pid == 0) {
        int r = child_body();
        _exit(r);   // _exit(0) when asserts stripped, never reached with FORCE_ASSERTS
    }
    int status = 0;
    QVERIFY2(waitpid(pid, &status, 0) == pid, "waitpid failed");
    QVERIFY2(WIFSIGNALED(status), "child exited normally; bounds check did not fire");
    QCOMPARE(WTERMSIG(status), SIGABRT);
}

QTEST_APPLESS_MAIN(TestForceAsserts)
#include "tst_force_asserts.moc"
