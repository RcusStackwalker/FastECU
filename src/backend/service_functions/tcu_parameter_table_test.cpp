#include "src/backend/service_functions/tcu_parameter_table.h"

#include <gtest/gtest.h>

namespace fastecu::service_functions
{
namespace
{

TcuParameterValues sample()
{
    return TcuParameterValues{
        .correction_1to2 = 0x11,
        .correction_2to3 = 0x22,
        .correction_3to4 = 0x33,
        .correction_4to5 = 0x44,
        .correction_forward_brake = 0x55,
        .correction_four_wheel_drive = 0x66,
        .correction_line_pressure = 0x77,
        .temperature_basis = 0x88,
        .torque_correction_awd = 0xBEEF,
    };
}

TEST(TcuParameterTable, WritesTwelveFramesNotNine)
{
    // Ten parameter writes (nine values; AWD torque spans two addresses) plus
    // the two-write commit. legacy :213-479.
    EXPECT_EQ(kTcuParameterWriteCount, 12U);
    EXPECT_EQ(tcu_parameter_writes(sample()).size(), 12U);
}

TEST(TcuParameterTable, PreservesTheLegacyWireOrderNotThePromptOrder)
{
    // Prompts run 1->2, 2->3, 3->4, 4->5, ... (legacy :162-202). Writes run
    // 0x16c = 3->4, 0x16d = 2->3, 0x16e = 1->2, 0x16f = 4->5 (legacy :213-286).
    const auto writes = tcu_parameter_writes(sample());

    EXPECT_EQ(writes[0].address, 0x00016cU);
    EXPECT_EQ(writes[0].value, 0x33); // correction_3to4
    EXPECT_EQ(writes[1].address, 0x00016dU);
    EXPECT_EQ(writes[1].value, 0x22); // correction_2to3
    EXPECT_EQ(writes[2].address, 0x00016eU);
    EXPECT_EQ(writes[2].value, 0x11); // correction_1to2
    EXPECT_EQ(writes[3].address, 0x00016fU);
    EXPECT_EQ(writes[3].value, 0x44); // correction_4to5
}

TEST(TcuParameterTable, SplitsAwdTorqueAcrossTwoAddressesHighFirst)
{
    // legacy :309-334.
    const auto writes = tcu_parameter_writes(sample());

    EXPECT_EQ(writes[4].address, 0x000170U);
    EXPECT_EQ(writes[4].value, 0xBE);
    EXPECT_EQ(writes[5].address, 0x000171U);
    EXPECT_EQ(writes[5].value, 0xEF);
}

TEST(TcuParameterTable, WritesTheRemainingFourCorrections)
{
    // legacy :357-430.
    const auto writes = tcu_parameter_writes(sample());

    EXPECT_EQ(writes[6].address, 0x0001bcU);
    EXPECT_EQ(writes[6].value, 0x55); // forward brake
    EXPECT_EQ(writes[7].address, 0x0001bdU);
    EXPECT_EQ(writes[7].value, 0x66); // 4WD
    EXPECT_EQ(writes[8].address, 0x0001beU);
    EXPECT_EQ(writes[8].value, 0x77); // line pressure
    EXPECT_EQ(writes[9].address, 0x0001bfU);
    EXPECT_EQ(writes[9].value, 0x88); // temperature basis
}

TEST(TcuParameterTable, EndsWithTheTwoWriteCommitToTheSameAddress)
{
    // legacy :453-479 forms B8 00 00 EC 55/AA. Not a parameter and not
    // prompted: a table keyed on the nine prompted values would drop it and
    // leave every write uncommitted.
    const auto writes = tcu_parameter_writes(sample());

    EXPECT_EQ(writes[10].address, 0x0000ecU);
    EXPECT_EQ(writes[10].value, 0x55);
    EXPECT_EQ(writes[11].address, 0x0000ecU);
    EXPECT_EQ(writes[11].value, 0xAA);
}

TEST(TcuParameterTable, CommitValuesAreIndependentOfParameterValues)
{
    TcuParameterValues values = sample();
    values.correction_forward_brake = 0xAA;
    const auto writes = tcu_parameter_writes(values);

    EXPECT_EQ(writes[10].value, 0x55);
    EXPECT_EQ(writes[11].value, 0xAA);
}

} // namespace
} // namespace fastecu::service_functions
