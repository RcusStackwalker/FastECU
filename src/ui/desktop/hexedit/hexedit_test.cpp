#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "src/ui/desktop/hexedit/hexedit.h"
#include "src/ui/desktop/hexedit/qhexedit/qhexedit.h"

class HexEditTest : public QObject
{
    Q_OBJECT

  private slots:
    // The window reads its layout from QSettings. When the store cannot persist
    // (the OptionsDialog write-through is lost, as on the Windows CI runner),
    // unset values read as 0 and layout divided by a zero bytes-per-line.
    void unpersistableSettingsStillGiveAUsableLayout()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QCoreApplication::setOrganizationName("FastECU-test");
        QCoreApplication::setApplicationName("hexedit-test");
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
        QSettings::setDefaultFormat(QSettings::IniFormat);

        HexEdit window(QByteArray(64, '\x0a'), "x.bin");
        auto *edit = window.findChild<QHexEdit *>();
        QVERIFY(edit != nullptr);

        QVERIFY(edit->bytesPerLine() >= 1);
        QVERIFY(edit->addressWidth() >= 1);
        QVERIFY(edit->addressArea());
        QVERIFY(edit->asciiArea());
    }

    void bytesPerLineIsNeverZero()
    {
        QHexEdit edit;
        edit.setData(QByteArray(64, '\x0a'));
        edit.setBytesPerLine(0);
        QVERIFY(edit.bytesPerLine() >= 1);
    }
};

QTEST_MAIN(HexEditTest)
#include "hexedit_test.moc"
