// src/backend/flash/transfer_progress_test.cpp
#include "src/backend/flash/transfer_progress.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

TEST(TransferProgressTest, ComputesSpeedAndEta)
{
    const TransferRate rate = ComputeTransferRate(0x400, 100, 0x4000);
    EXPECT_EQ(rate.speed, 10240U);
    EXPECT_EQ(rate.eta_s, 2U);
}

TEST(TransferProgressTest, ZeroElapsedCountsAsOneMillisecond)
{
    EXPECT_EQ(ComputeTransferRate(0x100, 0, 0).speed, 256000U);
}

TEST(TransferProgressTest, SpeedNeverDropsBelowOne)
{
    const TransferRate rate = ComputeTransferRate(1, 100000, 10);
    EXPECT_EQ(rate.speed, 1U);
    EXPECT_EQ(rate.eta_s, 11U);
}

TEST(TransferProgressTest, EtaClampsInsteadOfWrapping)
{
    EXPECT_EQ(ComputeTransferRate(1, 1000, 10000).eta_s, kMaxEtaSeconds);
    EXPECT_EQ(ComputeTransferRate(1, 1000, 20000).eta_s, kMaxEtaSeconds);
}

TEST(TransferProgressTest, FormatsReadLine)
{
    EXPECT_EQ(FormatReadProgress(0x1000, 0x400, TransferRate{.speed = 512, .eta_s = 7}),
              "Kernel read addr: 0x00001000 length: 0x00000400,    512 B/s      7 s");
}

TEST(TransferProgressTest, FormatsWriteLine)
{
    EXPECT_EQ(FormatWriteProgress(0x2000, 42, TransferRate{.speed = 512, .eta_s = 7}),
              "Write flash buffer: 0x00002000 (42% - 512 B/s, ~ 7 s)");
}

} // namespace
} // namespace fastecu::flash
