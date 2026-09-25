#pragma once

namespace fastecu::flash
{
// Step 5 tail, wave 6c-1. The BDM bridge speaks a fixed-baud ASCII command
// protocol; the baud is the only wire parameter.
struct SubaruDensoMc68hc16y5_02BdmPlan
{
    int baud;
};
} // namespace fastecu::flash
