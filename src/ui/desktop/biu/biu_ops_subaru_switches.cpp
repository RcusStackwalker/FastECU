#include "biu_ops_subaru_switches.h"
#include <ui_biu_ops_subaru_switches.h>

#include <cstddef>

BiuOpsSubaruSwitches::BiuOpsSubaruSwitches(QStringList *switchResult, QWidget *parent)
    : QWidget(parent), ui_{std::make_unique<Ui::BiuOpsSubaruSwitchesWindow>()}
{
    ui_->setupUi(this);
    // this->setParent(parent);

    this->switch_result_ = switchResult;

    QFont customFont("Courier New", 7);

    QLabel *label;

    int rowNum, colNum;

    colNum = 0;
    rowNum = 0;

    for (int i = 0; i < (switchResult->length() / 2); i++)
    {
        if ((i == 40) || (i == 72))
        {
            colNum += 2;
            rowNum = 0;
        }
        label = new QLabel();
        label->setObjectName("Name" + QString::number(i));
        label->setFont(customFont);
        label->setText(switchResult->at(2 * static_cast<qsizetype>(i)));
        ui_->gridLayout->addWidget(label, rowNum, colNum);

        label = new QLabel();
        label->setObjectName("Result" + QString::number(i));
        label->setFont(customFont);
        label->setText(switchResult->at(2 * i + 1));
        label->setAlignment(Qt::AlignCenter);
        if (switchResult->at(2 * i + 1) == "ON" || switchResult->at(2 * i + 1) == "YES")
        {
            label->setStyleSheet("QLabel { background-color : green; color : white;}");
        }
        else if (switchResult->at(2 * i + 1) == "OFF" || switchResult->at(2 * i + 1) == "NO")
        {
            label->setStyleSheet("QLabel { background-color : red; color : white;}");
        }
        else
        {
            label->setStyleSheet("QLabel { background-color : grey; color : white;}");
        }
        ui_->gridLayout->addWidget(label, rowNum, colNum + 1);

        rowNum++;
    }
}

BiuOpsSubaruSwitches::~BiuOpsSubaruSwitches()
{
}

void BiuOpsSubaruSwitches::update_switch_results(QStringList *switchResult)
{

    QLabel *currentLabel;

    for (int i = 0; i < (switchResult->length() / 2); i++)
    {

        currentLabel = ui_->gridLayoutWidget->findChild<QLabel *>("Result" + QString::number(i));

        if (currentLabel)
        {
            currentLabel->setText(switchResult->at(2 * i + 1));
            if (switchResult->at(2 * i + 1) == "ON" || switchResult->at(2 * i + 1) == "YES")
            {
                currentLabel->setStyleSheet("QLabel { background-color : green; color : white;}");
            }
            else if (switchResult->at(2 * i + 1) == "OFF" || switchResult->at(2 * i + 1) == "NO")
            {
                currentLabel->setStyleSheet("QLabel { background-color : red; color : white;}");
            }
            else
            {
                currentLabel->setStyleSheet("QLabel { background-color : grey; color : white;}");
            }
            // current_label->repaint();
        }
    }
}
