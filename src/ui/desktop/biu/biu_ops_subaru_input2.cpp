#include "biu_ops_subaru_input2.h"
#include <ui_biu_ops_subaru_input2.h>

#include <cstddef>

BiuOpsSubaruInput2::BiuOpsSubaruInput2(QStringList *biuOptionNames, QByteArray *biuOptionResult, QWidget *parent)
    : QWidget(parent), ui_{std::make_unique<Ui::BiuOpsSubaruInput2Window>()}
{
    ui_->setupUi(this);

    this->biu_option_names_ = biuOptionNames;
    this->biu_option_result_ = biuOptionResult;

    QLabel *label;
    QRadioButton *radioButton;
    QButtonGroup *buttonGroup;
    QPushButton *sendSetting;
    unsigned bitmask;
    int i, currentValue;

    for (int byteCounter = 0; byteCounter < biuOptionResult->length(); byteCounter++)
    {
        bitmask = 1U;

        for (int bitCounter = 0; bitCounter < 8; bitCounter++)
        {
            i = byteCounter * 8 + bitCounter;

            currentValue = static_cast<int>(static_cast<unsigned char>(biuOptionResult->at(byteCounter)) & bitmask);
            bitmask = bitmask << 1U;

            label = new QLabel();
            label->setObjectName("Name" + QString::number(i));
            label->setText(biuOptionNames->at(3 * static_cast<qsizetype>(i)));
            ui_->gridLayout->addWidget(label, i, 0);

            buttonGroup = new QButtonGroup();
            buttonGroup->setObjectName("Name GROUP" + QString::number(i));

            radioButton = new QRadioButton();
            radioButton->setObjectName("Name ON" + QString::number(i));
            radioButton->setText(biuOptionNames->at(3 * i + 1));
            if (currentValue != 0)
            {
                radioButton->setChecked(true);
            }
            buttonGroup->addButton(radioButton, 1);
            ui_->gridLayout->addWidget(radioButton, i, 1);

            radioButton = new QRadioButton();
            radioButton->setObjectName("Name OFF" + QString::number(i));
            radioButton->setText(biuOptionNames->at(3 * i + 2));
            if (currentValue == 0)
            {
                radioButton->setChecked(true);
            }
            buttonGroup->addButton(radioButton, 0);
            ui_->gridLayout->addWidget(radioButton, i, 2);
        }
    }

    sendSetting = new QPushButton();
    sendSetting->setObjectName("Name Send");
    sendSetting->setText("Send to BIU");
    ui_->gridLayout->addWidget(sendSetting, static_cast<int>(biuOptionResult->length() * 8 + 1), 2);

    connect(ui_->gridLayoutWidget->findChild<QPushButton *>("Name Send"), SIGNAL(clicked(bool)), this,
            SLOT(prepare_biu_setting2()));
}

BiuOpsSubaruInput2::~BiuOpsSubaruInput2()
{
}

void BiuOpsSubaruInput2::prepare_biu_setting2()
{
    QByteArray output;
    QRadioButton *currentButton;
    int i;
    unsigned bitmask;

    for (int byteCounter = 0; byteCounter < biu_option_result_->length(); byteCounter++)
    {
        bitmask = 1U;

        for (int bitCounter = 0; bitCounter < 8; bitCounter++)
        {
            i = byteCounter * 8 + bitCounter;
            currentButton = ui_->gridLayoutWidget->findChild<QRadioButton *>("Name ON" + QString::number(i));
            if (currentButton != nullptr && currentButton->isChecked())
            {
                output[byteCounter] = static_cast<char>(static_cast<unsigned char>(output[byteCounter]) | bitmask);
            }
            bitmask = bitmask << 1U;
        }
    }

    emit send_biu_setting2(output);
}
