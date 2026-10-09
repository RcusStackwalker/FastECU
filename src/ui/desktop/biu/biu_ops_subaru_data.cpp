#include "biu_ops_subaru_data.h"
#include <ui_biu_ops_subaru_data.h>

BiuOpsSubaruData::BiuOpsSubaruData(QStringList *dataResult, QWidget *parent)
    : QWidget(parent), ui_{std::make_unique<Ui::BiuOpsSubaruDataWindow>()}
{
    ui_->setupUi(this);

    this->data_result_ = dataResult;

    QFont customFont("Courier New", 10);

    QLabel *label;

    for (int i = 0; i < dataResult->length(); i++)
    {
        label = new QLabel;
        label->setObjectName("Name" + QString::number(i));
        label->setFont(customFont);
        label->setText(dataResult->at(i));
        ui_->gridLayout->addWidget(label, i, 0);
    }
}

BiuOpsSubaruData::~BiuOpsSubaruData()
{
}

void BiuOpsSubaruData::updateDataResults(QStringList *dataResult)
{

    QLabel *currentLabel;

    for (int i = 0; i < dataResult->length(); i++)
    {

        currentLabel = ui_->gridLayoutWidget->findChild<QLabel *>("Name" + QString::number(i));
        if (currentLabel)
        {
            currentLabel->setText(dataResult->at(i));
        }
    }
}
