#include <QTest>

#include "src/ui/desktop/hexedit/qhexedit/qhexedit.h"

class QHexEditTest : public QObject
{
    Q_OBJECT

  private slots:
    // Layout divides by the glyph metrics. A runner without installed fonts (the
    // Windows offscreen CI job) reports zero-pixel glyphs, which would raise an
    // integer divide-by-zero here.
    void showingAndResizingLaysOutWithoutCrashing()
    {
        QHexEdit edit;
        edit.setData(QByteArray(64, '\x0a'));
        edit.resize(300, 200);
        edit.show();
        QTest::qWait(10);

        QCOMPARE(edit.data().size(), qsizetype{64});
    }
};

QTEST_MAIN(QHexEditTest)
#include "qhexedit_test.moc"
