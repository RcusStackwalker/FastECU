#ifndef TESTS_EXPECTED_MESSAGE_BOX_H
#define TESTS_EXPECTED_MESSAGE_BOX_H

#include <QApplication>
#include <QMessageBox>
#include <QTimer>

class ExpectedMessageBoxCloser : public QObject
{
  public:
    ExpectedMessageBoxCloser()
    {
        timer_.setSingleShot(true);
        timer_.setInterval(0);
        connect(&timer_, &QTimer::timeout, this, [this]() {
            for (QWidget *widget : QApplication::topLevelWidgets())
            {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (!box)
                    continue;
#if defined(Q_OS_MACOS)
                const bool titleMatches = true;
#else
                const bool titleMatches = box->windowTitle() == expectedTitle_;
#endif
                if (titleMatches && box->text() == expectedText_)
                {
                    sawExpected_ = true;
                    box->accept();
                    return;
                }
                failure_ = QString("Unexpected QMessageBox: title='%1' text='%2'")
                               .arg(box->windowTitle(), box->text());
                box->reject();
                return;
            }
            failure_ = "Expected QMessageBox was not visible";
        });
    }

    void arm(const QString& title, const QString& text)
    {
        expectedTitle_ = title;
        expectedText_ = text;
        sawExpected_ = false;
        failure_.clear();
        timer_.start();
    }

    void stop()
    {
        timer_.stop();
        if (!sawExpected_ && failure_.isEmpty())
            failure_ = "Expected QMessageBox was not shown";
    }
    bool sawExpected() const { return sawExpected_; }
    QString failure() const { return failure_; }

  private:
    QTimer timer_;
    QString expectedTitle_;
    QString expectedText_;
    bool sawExpected_ = false;
    QString failure_;
};

#endif
