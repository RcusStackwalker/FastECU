#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include "src/platform/desktop/common/testing/widgets_application_environment.h"

#include "src/ui/desktop/hexedit/hexedit.h"
#include "src/ui/desktop/hexedit/qhexedit/qhexedit.h"

// The window reads its layout from QSettings. When the store cannot persist
// (the OptionsDialog write-through is lost, as on the Windows CI runner),
// unset values read as 0 and layout divided by a zero bytes-per-line.
TEST(HexEditTest, unpersistableSettingsStillGiveAUsableLayout)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    QCoreApplication::setOrganizationName("FastECU-test");
    QCoreApplication::setApplicationName("hexedit-test");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);

    HexEdit window(QByteArray(64, '\x0a'), "x.bin");
    auto *edit = window.findChild<QHexEdit *>();
    ASSERT_TRUE(edit != nullptr);

    ASSERT_TRUE(edit->bytesPerLine() >= 1);
    ASSERT_TRUE(edit->addressWidth() >= 1);
    ASSERT_TRUE(edit->addressArea());
    ASSERT_TRUE(edit->asciiArea());
}

TEST(HexEditTest, bytesPerLineIsNeverZero)
{
    QHexEdit edit;
    edit.setData(QByteArray(64, '\x0a'));
    edit.setBytesPerLine(0);
    ASSERT_TRUE(edit.bytesPerLine() >= 1);
}
namespace
{
const auto *const environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
