#pragma once

#include <memory>

#include <QWidget>
#include <QLabel>
#include <QButtonGroup>
#include <QRadioButton>
#include <QPushButton>

QT_BEGIN_NAMESPACE
namespace Ui
{
class BiuOpsSubaruInput2Window;
}
QT_END_NAMESPACE

class BiuOpsSubaruInput2 : public QWidget
{
    Q_OBJECT

  public:
    explicit BiuOpsSubaruInput2(QStringList *biuOptionNames, QByteArray *biuOptionResult, QWidget *parent = nullptr);
    ~BiuOpsSubaruInput2();

  private:
    QByteArray *biu_option_result_;
    QStringList *biu_option_names_;

  private slots:
    void prepare_biu_setting2();

  signals:
    void send_biu_setting2(QByteArray output);

  private:
    std::unique_ptr<Ui::BiuOpsSubaruInput2Window> ui_;
};
