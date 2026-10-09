#include "biu_ops_subaru_dtcs.h"
#include <ui_biu_ops_subaru_dtcs.h>

BiuOpsSubaruDtcs::BiuOpsSubaruDtcs(QStringList *dtcResult, QWidget *parent)
    : QWidget(parent), ui_{std::make_unique<Ui::BiuOpsSubaruDtcsWindow>()}
{
    ui_->setupUi(this);

    this->dtc_result_ = dtcResult;

    QLabel *label;

    for (int i = 0; i < dtcResult->length(); i++)
    {
        label = new QLabel;
        label->setText(dtcResult->at(i));
        ui_->gridLayout->addWidget(label, i, 0);
    }
}

BiuOpsSubaruDtcs::~BiuOpsSubaruDtcs()
{
}

void BiuOpsSubaruDtcs::closeEvent(QCloseEvent *event)
{
    qDebug() << "Closing BIU DTC window";
}
