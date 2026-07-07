#ifndef BIUOPSSUBARUINPUT1_H
#define BIUOPSSUBARUINPUT1_H

#include <memory>

#include <QWidget>
#include <QLabel>

QT_BEGIN_NAMESPACE
namespace Ui
{
class BiuOpsSubaruInput1Window;
}
QT_END_NAMESPACE

class BiuOpsSubaruInput1 : public QWidget
{
    Q_OBJECT

  public:
    explicit BiuOpsSubaruInput1(QByteArray *biu_tt_result, QWidget *parent = nullptr);
    ~BiuOpsSubaruInput1();

  private:
    QByteArray *biu_tt_result;

  private slots:
    void prepare_biu_setting1();

  signals:
    void send_biu_setting1(QByteArray output);

  private:
    std::unique_ptr<Ui::BiuOpsSubaruInput1Window> ui;
};

#endif // BIUOPSSUBARUINPUT1_H
