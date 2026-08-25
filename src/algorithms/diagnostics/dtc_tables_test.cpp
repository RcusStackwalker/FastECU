#include "src/algorithms/diagnostics/dtc_tables.h"
#include "src/algorithms/diagnostics/dtc_parser.h"
#include "src/algorithms/diagnostics/nrc_parser.h"

#include <gtest/gtest.h>

#include <ios>

TEST(DiagnosticTables, EntryCountsMatchTheLegacyQHashTables)
{
    EXPECT_EQ(nrc_codes().size(), 59U);
    EXPECT_EQ(dtc_p_codes().size(), 1733U);
    EXPECT_EQ(dtc_b_codes().size(), 1147U);
    EXPECT_EQ(dtc_c_codes().size(), 487U);
    EXPECT_EQ(dtc_u_codes().size(), 299U);
}

TEST(DiagnosticTables, SampledNrcEntriesSurvivedTranscription)
{
    EXPECT_EQ(nrc_codes().at(0x10), "General reject");
    EXPECT_EQ(nrc_codes().at(0x11), "Service not supported");
    EXPECT_EQ(nrc_codes().at(0x33), "Security access denied");
    EXPECT_EQ(nrc_codes().at(0x94), "Resource temporary unavailable");
}

TEST(DiagnosticTables, FirstAndLastEntryOfEachDtcTableSurvived)
{
    EXPECT_EQ(dtc_p_codes().at(0x0000), "P0000 - No trouble code");
    EXPECT_EQ(dtc_b_codes().at(0x1200), "B1200 - Climate Control Pushbutton Circuit Failure");
    EXPECT_EQ(dtc_c_codes().at(0x1091), "C1091 - Speed Wheel Sensor All Coherency Failure");
    EXPECT_EQ(dtc_u_codes().at(0x1000), "U1000 - SCP (J1850) Invalid or Missing Data for Primary Id");
    EXPECT_EQ(dtc_u_codes().at(0x2500), "U2500 - (CAN) Lack of Acknowledgement From Engine Management");
}

// Regression for the duplicate key corrected in Step 2: error_codes.h keyed
// both P1496 and P1498 at 0x1496, so a P1496 readout printed P1498's text and
// P1498 was unreachable.
TEST(DiagnosticTables, PowertrainCodesAroundTheFormerDuplicateAreDistinct)
{
    EXPECT_EQ(dtc_p_codes().at(0x1496), "P1496 - EGR Solenoid Valve Signal #3 Circuit Malfunction (Low Input)");
    EXPECT_EQ(dtc_p_codes().at(0x1498), "P1498 - EGR Solenoid Valve Signal #4 Circuit Malfunction (Low Input)");
}

// Every key is below 0x4000, i.e. the category bits are masked off. This is
// the invariant dtc_description's lookup depends on; if a future table edit
// breaks it, the mask in dtc_parser.cpp silently starts mismatching.
TEST(DiagnosticTables, DtcKeysCarryNoCategoryBits)
{
    for (const auto *table : {&dtc_p_codes(), &dtc_b_codes(), &dtc_c_codes(), &dtc_u_codes()})
    {
        for (const auto& entry : *table)
        {
            EXPECT_LT(entry.first, 0x4000) << "key 0x" << std::hex << entry.first << " has category bits set";
        }
    }
}

TEST(DiagnosticDefaults, NrcOverloadUsesTheRealTable)
{
    const bytes::Bytes frame{0x7f, 0x22, 0x33};
    EXPECT_EQ(nrc_description(frame), "Security access denied");
}

TEST(DiagnosticDefaults, NrcOverloadKeepsNonNegativeResponseHandling)
{
    const bytes::Bytes not_negative{0x62, 0x00, 0x01};
    EXPECT_EQ(nrc_description(not_negative), "Not a valid answer");
}

TEST(DiagnosticDefaults, DtcOverloadUsesTheRealPowertrainTable)
{
    EXPECT_EQ(dtc_description(0x0000), "P0000 - No trouble code");
}

// Regression: the tables are keyed by the 14-bit code, but dtc_description
// used to look up the full value including the two category bits it had
// already consumed to pick the table. P codes worked by coincidence
// (category 0 leaves the value unchanged); C, B and U never matched, so
// 1,933 of the 3,666 descriptions in the tree were unreachable.
TEST(DiagnosticDefaults, NonPowertrainCategoriesResolveRealDescriptions)
{
    EXPECT_EQ(dtc_description(0x4000 | 0x1091), "C1091 - Speed Wheel Sensor All Coherency Failure");
    EXPECT_EQ(dtc_description(0x8000 | 0x1200), "B1200 - Climate Control Pushbutton Circuit Failure");
    EXPECT_EQ(dtc_description(0xc000 | 0x1000), "U1000 - SCP (J1850) Invalid or Missing Data for Primary Id");
}

// The category bits must still select the right table -- a mask alone would
// let a B code resolve against the C table.
TEST(DiagnosticDefaults, CategoryStillSelectsTheTable)
{
    // 0x1290 is a real B key and absent from the C table. When asked as a C
    // code, it should fall back to unknown, not return the B description.
    EXPECT_EQ(dtc_description(0x4000 | 0x1290), "C1290 - Unknown error code");
}

TEST(DiagnosticDefaults, UnknownCodesStillFallBack)
{
    EXPECT_EQ(dtc_description(0x8000 | 0x3fff), "B3fff - Unknown error code");
}
