#include "src/backend/service_functions/tcu_parameter_table.h"

namespace fastecu::service_functions
{

std::array<TcuParameterWrite, kTcuParameterWriteCount> TcuParameterWrites(const TcuParameterValues& values)
{
    return {{
        // IC correction, 3->4. First on the wire, third prompted.
        {0x00016c, values.correction_3to4},
        // HLRC correction, 2->3.
        {0x00016d, values.correction_2to3},
        // DC correction, 1->2.
        {0x00016e, values.correction_1to2},
        // FB correction, 4->5.
        {0x00016f, values.correction_4to5},
        // AWD clutch torque, high byte.
        {0x000170, static_cast<bytes::Byte>((static_cast<std::uint32_t>(values.torque_correction_awd) >> 8U) & 0xffU)},
        // AWD clutch torque, low byte.
        {0x000171, static_cast<bytes::Byte>(values.torque_correction_awd & 0xff)},
        // Forward brake pressure correction.
        {0x0001bc, values.correction_forward_brake},
        // 4WD pressure correction.
        {0x0001bd, values.correction_four_wheel_drive},
        // Line pressure correction.
        {0x0001be, values.correction_line_pressure},
        // Temperature basis for the corrections above.
        {0x0001bf, values.temperature_basis},
        // Output[2:4] becomes 00 EC 55/AA, so the commit pair targets 0x0000ec.
        // Same address, fixed values, no prompt.
        {0x0000ec, 0x55},
        {0x0000ec, 0xaa},
    }};
}

} // namespace fastecu::service_functions
