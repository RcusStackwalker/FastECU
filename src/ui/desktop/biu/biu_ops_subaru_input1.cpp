#include "biu_ops_subaru_input1.h"
#include <ui_biu_ops_subaru_input1.h>

BiuOpsSubaruInput1::BiuOpsSubaruInput1(QByteArray *biuTtResult, QWidget *parent)
    : QWidget(parent), ui_{std::make_unique<Ui::BiuOpsSubaruInput1Window>()}
{
    ui_->setupUi(this);

    this->biu_tt_result_ = biuTtResult;

    ui_->light_delay_combo->addItem("Normal");
    ui_->light_delay_combo->addItem("Off");
    ui_->light_delay_combo->addItem("Short");
    ui_->light_delay_combo->addItem("Long");
    ui_->light_delay_combo->setCurrentIndex(biuTtResult->at(0));

    ui_->autolock_combo->addItem("20");
    ui_->autolock_combo->addItem("30");
    ui_->autolock_combo->addItem("40");
    ui_->autolock_combo->addItem("50");
    ui_->autolock_combo->addItem("60");
    ui_->autolock_combo->setCurrentIndex(biuTtResult->at(1) - 2);

    if (biuTtResult->length() == 3)
    {
        ui_->outtemp_combo->addItem("0.0");
        ui_->outtemp_combo->addItem("0.5");
        ui_->outtemp_combo->addItem("1.0");
        ui_->outtemp_combo->addItem("1.5");
        ui_->outtemp_combo->addItem("-2.0");
        ui_->outtemp_combo->addItem("-1.5");
        ui_->outtemp_combo->addItem("-1.0");
        ui_->outtemp_combo->addItem("-0.5");
        ui_->outtemp_combo->setCurrentIndex(biuTtResult->at(2));
    }
    else
    {
        ui_->outtemp_combo->hide();
        ui_->label_3->hide();
    }

    connect(ui_->send_setting, SIGNAL(clicked(bool)), this, SLOT(prepareBiuSetting1()));
}

BiuOpsSubaruInput1::~BiuOpsSubaruInput1()
{
}

void BiuOpsSubaruInput1::prepareBiuSetting1()
{
    QByteArray output;

    output.append(static_cast<char>(ui_->light_delay_combo->currentIndex()));
    output.append(static_cast<char>(ui_->autolock_combo->currentIndex() + 2));
    if (biu_tt_result_->length() == 3)
    {
        output.append(static_cast<char>(ui_->outtemp_combo->currentIndex()));
    }

    emit sendBiuSetting1(output);
}
