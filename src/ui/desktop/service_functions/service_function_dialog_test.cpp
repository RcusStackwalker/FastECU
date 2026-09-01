#include "src/ui/desktop/service_functions/service_function_dialog.h"

#include <QSpinBox>
#include <QTableWidget>
#include <QTest>

using fastecu::service_functions::ServiceFunctionDialog;
using fastecu::service_functions::ServiceFunctionKind;
using fastecu::service_functions::TcuParameterReadout;

namespace
{

QSpinBox *box(ServiceFunctionDialog& dialog, const char *name)
{
    return dialog.findChild<QSpinBox *>(QString::fromLatin1(name));
}

} // namespace

class ServiceFunctionDialogTest : public QObject
{
    Q_OBJECT

  private slots:
    void setParametersSpinBoxesCarryTheLegacyPromptBounds()
    {
        // legacy :162-202 -- eight prompts bounded 0-255 and one bounded
        // 0-65535. The value model makes these unrepresentable rather than
        // rejectable, so this is where the bounds are actually asserted.
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::SetParameters};

        for (const char *name :
             {"correction_1to2", "correction_2to3", "correction_3to4", "correction_4to5", "correction_forward_brake",
              "correction_four_wheel_drive", "correction_line_pressure", "temperature_basis"})
        {
            QSpinBox *spin = box(dialog, name);
            QVERIFY2(spin != nullptr, name);
            QCOMPARE(spin->minimum(), 0);
            QCOMPARE(spin->maximum(), 255);
        }

        QSpinBox *torque = box(dialog, "torque_correction_awd");
        QVERIFY(torque != nullptr);
        QCOMPARE(torque->minimum(), 0);
        QCOMPARE(torque->maximum(), 65535);
    }

    void everyFormFieldLandsInItsOwnStructMember()
    {
        // Guards against a form-to-struct mix-up, which the wire-order table
        // in tcu_parameter_table_test cannot catch: nine distinct values in,
        // nine distinct members out.
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::SetParameters};

        box(dialog, "correction_1to2")->setValue(0x11);
        box(dialog, "correction_2to3")->setValue(0x22);
        box(dialog, "correction_3to4")->setValue(0x33);
        box(dialog, "correction_4to5")->setValue(0x44);
        box(dialog, "correction_forward_brake")->setValue(0x55);
        box(dialog, "correction_four_wheel_drive")->setValue(0x66);
        box(dialog, "correction_line_pressure")->setValue(0x77);
        box(dialog, "temperature_basis")->setValue(0x88);
        box(dialog, "torque_correction_awd")->setValue(0xbeef);

        const auto values = dialog.collectedValues();
        QCOMPARE(values.correction_1to2, 0x11);
        QCOMPARE(values.correction_2to3, 0x22);
        QCOMPARE(values.correction_3to4, 0x33);
        QCOMPARE(values.correction_4to5, 0x44);
        QCOMPARE(values.correction_forward_brake, 0x55);
        QCOMPARE(values.correction_four_wheel_drive, 0x66);
        QCOMPARE(values.correction_line_pressure, 0x77);
        QCOMPARE(values.temperature_basis, 0x88);
        QCOMPARE(values.torque_correction_awd, 0xbeef);
    }

    void setParametersFormIsOneDialogNotNineModals()
    {
        // The legacy asks nine sequential QInputDialogs (:162-202); this shows
        // all nine at once so the operator can review before any write.
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::SetParameters};
        QCOMPARE(dialog.findChildren<QSpinBox *>().count(), 9);
    }

    void readParametersRendersAllNineLabelledValues()
    {
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::ReadParameters};
        dialog.showReadout(TcuParameterReadout{
            .input_clutch = 0x11,
            .high_low_reverse_clutch = 0x22,
            .direct_clutch = 0x33,
            .front_brake = 0x44,
            .awd_clutch_torque = 0xbeef,
            .forward_brake = 0x55,
            .four_wheel_drive = 0x66,
            .line_pressure = 0x77,
            .temperature_basis = 0x88,
        });

        auto *table = dialog.findChild<QTableWidget *>("readout");
        QVERIFY(table != nullptr);
        QCOMPARE(table->rowCount(), 9);
        QCOMPARE(table->item(0, 0)->text(), QString("Input Clutch Pressure Correction"));
        QCOMPARE(table->item(0, 1)->text(), QString("17"));
        QCOMPARE(table->item(4, 0)->text(), QString("Correction of AWD Clutch Torque"));
        QCOMPARE(table->item(4, 1)->text(), QString("48879"));
        QCOMPARE(table->item(8, 0)->text(), QString("Temperature basis for above Pressure Corrections"));
    }

    void readParametersHasNoSpinBoxes()
    {
        ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::ReadParameters};
        QCOMPARE(dialog.findChildren<QSpinBox *>().count(), 0);
    }
};

QTEST_MAIN(ServiceFunctionDialogTest)
#include "service_function_dialog_test.moc"
