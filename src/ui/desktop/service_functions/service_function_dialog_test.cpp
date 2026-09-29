#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include "src/ui/desktop/service_functions/service_function_dialog.h"

#include <QSpinBox>
#include <QTableWidget>
#include <gtest/gtest.h>

#include <array>

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

class ServiceFunctionDialogTest : public ::testing::Test
{

  public:
};

TEST_F(ServiceFunctionDialogTest, setParametersSpinBoxesCarryTheLegacyPromptBounds)
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
        ASSERT_TRUE(spin != nullptr) << name;
        ASSERT_EQ(spin->minimum(), 0);
        ASSERT_EQ(spin->maximum(), 255);
    }

    QSpinBox *torque = box(dialog, "torque_correction_awd");
    ASSERT_TRUE(torque != nullptr);
    ASSERT_EQ(torque->minimum(), 0);
    ASSERT_EQ(torque->maximum(), 65535);
}

TEST_F(ServiceFunctionDialogTest, everyFormFieldLandsInItsOwnStructMember)
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
    ASSERT_EQ(values.correction_1to2, 0x11);
    ASSERT_EQ(values.correction_2to3, 0x22);
    ASSERT_EQ(values.correction_3to4, 0x33);
    ASSERT_EQ(values.correction_4to5, 0x44);
    ASSERT_EQ(values.correction_forward_brake, 0x55);
    ASSERT_EQ(values.correction_four_wheel_drive, 0x66);
    ASSERT_EQ(values.correction_line_pressure, 0x77);
    ASSERT_EQ(values.temperature_basis, 0x88);
    ASSERT_EQ(values.torque_correction_awd, 0xbeef);
}

TEST_F(ServiceFunctionDialogTest, setParametersFormIsOneDialogNotNineModals)
{
    // The legacy asks nine sequential QInputDialogs (:162-202); this shows
    // all nine at once so the operator can review before any write.
    ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::SetParameters};
    ASSERT_EQ(dialog.findChildren<QSpinBox *>().count(), 9);
}

TEST_F(ServiceFunctionDialogTest, readParametersRendersAllNineLegacyQualifiedLabelsAndValues)
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
    ASSERT_TRUE(table != nullptr);
    ASSERT_EQ(table->rowCount(), 9);
    const std::array<const char *, 9> labels{
        "Input Clutch Pressure Correction (raw byte)",
        "High Low Reverse Clutch Pressure Correction (raw byte)",
        "Direct Clutch Pressure Correction (raw byte)",
        "Front Brake Pressure Correction (raw byte)",
        "Correction of AWD Clutch Torque (raw word)",
        "Forward Brake Pressure Correction (raw byte)",
        "4WD Pressure Correction (raw byte)",
        "Line Pressure Correction (raw byte)",
        "Temperature basis for above Pressure Corrections (raw byte)",
    };
    for (int row = 0; row < static_cast<int>(labels.size()); ++row)
    {
        ASSERT_EQ(table->item(row, 0)->text(), QString::fromLatin1(labels[static_cast<std::size_t>(row)]));
    }
    ASSERT_EQ(table->item(0, 1)->text(), QString("17"));
    ASSERT_EQ(table->item(4, 1)->text(), QString("48879"));
}

TEST_F(ServiceFunctionDialogTest, readParametersHasNoSpinBoxes)
{
    ServiceFunctionDialog dialog{nullptr, "sub_tcu_denso_sh7058_can", ServiceFunctionKind::ReadParameters};
    ASSERT_EQ(dialog.findChildren<QSpinBox *>().count(), 0);
}

namespace
{
const auto *const application_environment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment);
}
