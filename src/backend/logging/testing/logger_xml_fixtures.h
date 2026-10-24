#pragma once

#include <string_view>

namespace fastecu::logging::testing
{

// One SSM protocol with parameters P1 (enabled) and P2 (disabled) and switch S1.
inline constexpr std::string_view kDefinition =
    R"(<logger><protocols><protocol id="SSM"><parameters>
  <parameter id="P1" enabled="1"><address>0x1</address></parameter>
  <parameter id="P2" enabled="0"><address>0x2</address></parameter>
</parameters><switches><switch id="S1"/></switches></protocol></protocols></logger>)";

// A conf file holding a stored selection for ECU "ECUID1".
inline constexpr std::string_view kConfWithEcu =
    R"(<config><logger><ecu id="ECUID1"><protocol id="SSM"><parameters>
  <gauges><parameter id="P2" name=""/></gauges>
  <lower_panel><parameter id="P2" name=""/></lower_panel>
</parameters><switches><switch id="S1" name=""/></switches></protocol></ecu></logger></config>)";

// A conf file with no ECU entries.
inline constexpr std::string_view kEmptyConf = "<config><logger/></config>";

} // namespace fastecu::logging::testing
