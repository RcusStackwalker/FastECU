#include "src/ui/desktop/calibration/qt_calibration_interaction.h"

#include <functional>
#include <optional>
#include <string>
#include <tuple>

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QWidget>
#include <gtest/gtest.h>

#include "src/algorithms/memory/testing/memory_views.h"

#include "src/platform/desktop/common/testing/widgets_application_environment.h"

using fastecu::checksum::ChecksumSelection;
using fastecu::ui::CalibrationNotice;
using fastecu::ui::QtCalibrationInteraction;

namespace
{

const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment);

constexpr int kDeadlineMs = 3000;

struct BoxObservation
{
    bool seen = false;
    QString title;
    QString text;
    QMessageBox::Icon icon = QMessageBox::NoIcon;
    QMessageBox::StandardButtons buttons;
    QWidget *parent = nullptr;
};

struct PickerObservation
{
    bool seen = false;
    QString title;
    QString filter;
};

// Polls for a visible dialog of type T while the code under test is blocked in
// its modal exec(); answers it once and fails on the deadline.
template <typename T> class DialogDriver
{
  public:
    DialogDriver(std::function<void(T *)> answer, bool *timedOut) : answer_(std::move(answer)), timed_out_(timedOut)
    {
        deadline_.start();
        timer_.setInterval(5);
        QObject::connect(&timer_, &QTimer::timeout, [this] { drive(); });
        timer_.start();
    }

  private:
    void drive()
    {
        if (deadline_.elapsed() > kDeadlineMs)
        {
            *timed_out_ = true;
            timer_.stop();
            for (QWidget *widget : QApplication::topLevelWidgets())
            {
                if (auto *dialog = qobject_cast<T *>(widget); dialog && dialog->isVisible())
                {
                    dialog->reject();
                }
            }
            return;
        }
        for (QWidget *widget : QApplication::topLevelWidgets())
        {
            if (auto *dialog = qobject_cast<T *>(widget); dialog && dialog->isVisible())
            {
                timer_.stop();
                answer_(dialog);
                return;
            }
        }
    }

    std::function<void(T *)> answer_;
    bool *timed_out_;
    QTimer timer_;
    QElapsedTimer deadline_;
};

BoxObservation observeBox(QMessageBox *box)
{
    BoxObservation observed;
    observed.seen = true;
    observed.title = box->windowTitle();
    observed.text = box->text();
    observed.icon = box->icon();
    observed.buttons = box->standardButtons();
    observed.parent = box->parentWidget();
    return observed;
}

class NativeDialogsDisabled
{
  public:
    NativeDialogsDisabled() : previous_(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs))
    {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    }
    ~NativeDialogsDisabled()
    {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous_);
    }
    NativeDialogsDisabled(const NativeDialogsDisabled&) = delete;
    NativeDialogsDisabled& operator=(const NativeDialogsDisabled&) = delete;

  private:
    bool previous_;
};

// Qt's macOS style gives QMessageBox no window title, so title assertions on a
// message box only hold elsewhere.
#ifdef Q_OS_MACOS
constexpr bool kMessageBoxHasTitle = false;
#else
constexpr bool kMessageBoxHasTitle = true;
#endif

const QString kWriteWarningBody = "WARNING! There is no checksum module for this ROM!\n"
                                  "Be aware that if this ROM need checksum correction it must be done with another "
                                  "software!";

class WriteWarningChoices : public ::testing::TestWithParam<QMessageBox::StandardButton>
{
};

TEST_P(WriteWarningChoices, MapsOnlyCancelToDeclined)
{
    const QMessageBox::StandardButton answer = GetParam();
    QWidget parent;
    QtCalibrationInteraction interaction(&parent);
    BoxObservation observed;
    bool timedOut = false;
    DialogDriver<QMessageBox> driver(
        [&](QMessageBox *box)
        {
            observed = observeBox(box);
            box->done(answer);
        },
        &timedOut);

    const bool proceed = interaction.confirmWriteWithoutChecksum();

    ASSERT_FALSE(timedOut);
    ASSERT_TRUE(observed.seen);
    EXPECT_EQ(proceed, answer != QMessageBox::Cancel);
    if (kMessageBoxHasTitle)
    {
        EXPECT_EQ(observed.title, QString("Checksum warning"));
    }
    EXPECT_EQ(observed.text, kWriteWarningBody);
    EXPECT_EQ(observed.icon, QMessageBox::Warning);
    EXPECT_EQ(observed.buttons, QMessageBox::Ok | QMessageBox::Cancel);
    EXPECT_EQ(observed.parent, nullptr);
}

INSTANTIATE_TEST_SUITE_P(Answers, WriteWarningChoices,
                         ::testing::Values(QMessageBox::Ok, QMessageBox::Cancel, QMessageBox::Rejected));

struct NoticeCase
{
    CalibrationNotice notice;
    QMessageBox::Icon icon;
    const char *title;
    const char *text;
};

class NoticeRendering : public ::testing::TestWithParam<NoticeCase>
{
};

TEST_P(NoticeRendering, ShowsExpectedBoxOnParent)
{
    const NoticeCase expected = GetParam();
    QWidget parent;
    QtCalibrationInteraction interaction(&parent);
    BoxObservation observed;
    bool timedOut = false;
    DialogDriver<QMessageBox> driver(
        [&](QMessageBox *box)
        {
            observed = observeBox(box);
            box->accept();
        },
        &timedOut);

    interaction.showNotice(expected.notice);

    ASSERT_FALSE(timedOut);
    ASSERT_TRUE(observed.seen);
    EXPECT_EQ(observed.parent, &parent);
    EXPECT_EQ(observed.icon, expected.icon);
    if (kMessageBoxHasTitle)
    {
        EXPECT_EQ(observed.title, QString(expected.title));
    }
    EXPECT_EQ(observed.text, QString(expected.text));
}

INSTANTIATE_TEST_SUITE_P(Notices, NoticeRendering,
                         ::testing::Values(NoticeCase{CalibrationNotice::kNoCalibrationToWrite, QMessageBox::Warning,
                                                      "Write ROM", "No file selected!"},
                                           NoticeCase{CalibrationNotice::kNoCalibrationToSave, QMessageBox::Information,
                                                      "Calibration file", "No calibration to save!"},
                                           NoticeCase{CalibrationNotice::kNoSaveFilename, QMessageBox::Information,
                                                      "Calibration file", "No file name selected"}));

TEST(QtCalibrationInteraction, SavePathRoundTrip)
{
    NativeDialogsDisabled noNative;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QWidget parent;
    QtCalibrationInteraction interaction(&parent);
    const QString chosen = QDir(directory.path()).absoluteFilePath(QString::fromUtf8("tune-\xc3\xa9.BIN"));
    const std::string suggested = (directory.path() + "/read.bin").toStdString();
    PickerObservation observed;
    bool timedOut = false;
    DialogDriver<QFileDialog> driver(
        [&](QFileDialog *dialog)
        {
            observed.seen = true;
            observed.title = dialog->windowTitle();
            observed.filter = dialog->nameFilters().value(0);
            // The non-native dialog takes its answer from the file-name field.
            auto *nameEdit = dialog->findChild<QLineEdit *>("fileNameEdit");
            if (!nameEdit)
            {
                ADD_FAILURE() << "QFileDialog has no fileNameEdit child";
                dialog->reject();
                return;
            }
            nameEdit->setText(chosen);
            QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        },
        &timedOut);

    const auto path = interaction.chooseSavePath(suggested);

    ASSERT_FALSE(timedOut);
    ASSERT_TRUE(observed.seen);
    EXPECT_EQ(path, std::optional<std::string>(chosen.toStdString()));
    EXPECT_EQ(observed.title, QString("Save calibration file"));
    EXPECT_EQ(observed.filter, QString("Calibration file (*.bin)"));
}

TEST(QtCalibrationInteraction, CancellingSavePathReturnsNoPath)
{
    NativeDialogsDisabled noNative;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QWidget parent;
    QtCalibrationInteraction interaction(&parent);
    bool timedOut = false;
    DialogDriver<QFileDialog> driver([](QFileDialog *dialog) { dialog->reject(); }, &timedOut);

    const auto path = interaction.chooseSavePath((directory.path() + "/read.bin").toStdString());

    ASSERT_FALSE(timedOut);
    EXPECT_EQ(path, std::nullopt);
}

TEST(QtCalibrationInteraction, UnknownMcuDelegatesToChecksumCommand)
{
    QWidget parent;
    QtCalibrationInteraction interaction(&parent);
    ChecksumSelection selection;
    selection.mcu_type = "M32170";
    const bytes::Bytes image(16, 0);
    int dialogCount = 0;
    QTimer counter;
    counter.setInterval(5);
    QObject::connect(&counter, &QTimer::timeout,
                     [&dialogCount]
                     {
                         for (QWidget *widget : QApplication::topLevelWidgets())
                         {
                             if (widget->isVisible() && qobject_cast<QDialog *>(widget) != nullptr)
                             {
                                 ++dialogCount;
                                 widget->close();
                             }
                         }
                     });
    counter.start();

    const auto result = interaction.correctChecksums(
        fastecu::memory::testing::ImageAt(fastecu::memory::FlashAddress{0}, image), true, selection);
    QCoreApplication::processEvents();

    EXPECT_TRUE(result.unknown_mcu_type);
    EXPECT_EQ(dialogCount, 0);
}

class MainWindowOnlyTranslator : public QTranslator
{
  public:
    QString translate(const char *context, const char *sourceText, const char *, int) const override
    {
        if (std::string_view(context) != "MainWindow")
        {
            return {};
        }
        return QString("T:") + sourceText;
    }
    bool isEmpty() const override
    {
        return false;
    }
};

TEST(QtCalibrationInteraction, MigratedTranslationsUseMainWindowContext)
{
    NativeDialogsDisabled noNative;
    MainWindowOnlyTranslator translator;
    QCoreApplication::installTranslator(&translator);
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QWidget parent;
    QtCalibrationInteraction interaction(&parent);
    PickerObservation picker;
    BoxObservation box;
    bool timedOut = false;
    {
        DialogDriver<QFileDialog> driver(
            [&](QFileDialog *dialog)
            {
                picker.seen = true;
                picker.title = dialog->windowTitle();
                picker.filter = dialog->nameFilters().value(0);
                dialog->reject();
            },
            &timedOut);
        std::ignore = interaction.chooseSavePath((directory.path() + "/read.bin").toStdString());
    }
    {
        DialogDriver<QMessageBox> driver(
            [&](QMessageBox *notice)
            {
                box = observeBox(notice);
                notice->accept();
            },
            &timedOut);
        interaction.showNotice(CalibrationNotice::kNoCalibrationToWrite);
    }
    QCoreApplication::removeTranslator(&translator);

    ASSERT_FALSE(timedOut);
    EXPECT_EQ(picker.title, QString("T:Save calibration file"));
    EXPECT_EQ(picker.filter, QString("T:Calibration file (*.bin)"));
    if (kMessageBoxHasTitle)
    {
        EXPECT_EQ(box.title, QString("T:Write ROM"));
    }
}

} // namespace
