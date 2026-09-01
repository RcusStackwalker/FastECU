#include "src/backend/service_functions/tcu_parameter_table.h"

namespace fastecu::service_functions
{

std::array<TcuParameterWrite, kTcuParameterWriteCount> tcu_parameter_writes(const TcuParameterValues& values)
{
    return {{
        // legacy :213 -- IC correction, 3->4. First on the wire, third prompted.
        {0x00016c, values.correction_3to4},
        // legacy :237 -- HLRC correction, 2->3.
        {0x00016d, values.correction_2to3},
        // legacy :261 -- DC correction, 1->2.
        {0x00016e, values.correction_1to2},
        // legacy :285 -- FB correction, 4->5.
        {0x00016f, values.correction_4to5},
        // legacy :309 -- AWD clutch torque, high byte.
        {0x000170, static_cast<bytes::Byte>((values.torque_correction_awd >> 8) & 0xff)},
        // legacy :333 -- AWD clutch torque, low byte.
        {0x000171, static_cast<bytes::Byte>(values.torque_correction_awd & 0xff)},
        // legacy :357 -- forward brake pressure correction.
        {0x0001bc, values.correction_forward_brake},
        // legacy :381 -- 4WD pressure correction.
        {0x0001bd, values.correction_four_wheel_drive},
        // legacy :405 -- line pressure correction.
        {0x0001be, values.correction_line_pressure},
        // legacy :429 -- temperature basis for the corrections above.
        {0x0001bf, values.temperature_basis},
        // legacy :453-479 -- output[2:4] becomes 00 EC 55/AA, so the commit
        // pair targets 0x0000ec. Same address, fixed values, no prompt.
        {0x0000ec, 0x55},
        {0x0000ec, 0xaa},
    }};
}

} // namespace fastecu::service_functions
