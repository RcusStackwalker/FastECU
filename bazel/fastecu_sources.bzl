# Generated from qmake project files by the initial Bazel migration. Keep explicit lists in sync with scripts/check-bazel-qmake-sync.py.

"""Explicit FastECU source manifests generated from qmake project files."""

APP_MAIN_SRCS = [
    "main.cpp",
]

APP_COMMON_SRCS = [
    "calibration_maps.cpp",
    "calibration_treewidget.cpp",
    "cipher.cpp",
    "dataterminal.cpp",
    "definition_file_convert.cpp",
    "dtc_operations.cpp",
    "dtc_parser.cpp",
    "ecu_operations.cpp",
    "expression_evaluator.cpp",
    "file_actions.cpp",
    "file_defs_ecuflash.cpp",
    "file_defs_romraider.cpp",
    "get_key_operations_subaru.cpp",
    "hexedit/hexedit.cpp",
    "hexedit/optionsdialog.cpp",
    "hexedit/qhexedit/chunks.cpp",
    "hexedit/qhexedit/commands.cpp",
    "hexedit/qhexedit/qhexedit.cpp",
    "hexedit/searchdialog.cpp",
    "log_operations_ssm.cpp",
    "logbox.cpp",
    "logging/logging_engine.cpp",
    "logging/logging_worker.cpp",
    "logging/protocols/cdbg_logging_protocol.cpp",
    "logging/protocols/mut_dma_logging_protocol.cpp",
    "logging/protocols/ssm_logging_protocol.cpp",
    "logging/romraider_conversion.cpp",
    "logvalues.cpp",
    "mainwindow.cpp",
    "menu_actions.cpp",
    "menu_command.cpp",
    "modules/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm.cpp",
    "modules/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.cpp",
    "modules/biu/biu_operations_subaru.cpp",
    "modules/biu/biu_ops_subaru_data.cpp",
    "modules/biu/biu_ops_subaru_dtcs.cpp",
    "modules/biu/biu_ops_subaru_input1.cpp",
    "modules/biu/biu_ops_subaru_input2.cpp",
    "modules/biu/biu_ops_subaru_switches.cpp",
    "modules/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode.cpp",
    "modules/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp",
    "modules/checksum/checksum_ecu_subaru_denso_sh705x_diesel.cpp",
    "modules/checksum/checksum_ecu_subaru_denso_sh7xxx.cpp",
    "modules/checksum/checksum_ecu_subaru_hitachi_m32r_can.cpp",
    "modules/checksum/checksum_ecu_subaru_hitachi_m32r_kline.cpp",
    "modules/checksum/checksum_ecu_subaru_hitachi_sh7058.cpp",
    "modules/checksum/checksum_ecu_subaru_hitachi_sh72543r.cpp",
    "modules/checksum/checksum_tcu_mitsu_mh8104_can.cpp",
    "modules/checksum/checksum_tcu_subaru_denso_sh7055.cpp",
    "modules/checksum/checksum_tcu_subaru_hitachi_m32r_can.cpp",
    "modules/ecu/flash_ecu_mitsu_m32r_can.cpp",
    "modules/ecu/flash_ecu_mitsu_m32r_can_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.cpp",
    "modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_1n83m_4m_can.cpp",
    "modules/ecu/flash_ecu_subaru_denso_1n83m_4m_can_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_mc68hc16y5_02.cpp",
    "modules/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh7055_02.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh7055_02_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh7058_can.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh7058_can_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh705x_densocan.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh705x_kline.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh72531_can.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh72531_can_operation.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh72543_can_diesel.cpp",
    "modules/ecu/flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp",
    "modules/ecu/flash_ecu_subaru_hitachi_m32r_can.cpp",
    "modules/ecu/flash_ecu_subaru_hitachi_m32r_can_operation.cpp",
    "modules/ecu/flash_ecu_subaru_hitachi_m32r_kline.cpp",
    "modules/ecu/flash_ecu_subaru_hitachi_m32r_kline_operation.cpp",
    "modules/ecu/flash_ecu_subaru_hitachi_sh7058_can.cpp",
    "modules/ecu/flash_ecu_subaru_hitachi_sh7058_can_operation.cpp",
    "modules/ecu/flash_ecu_subaru_hitachi_sh72543r_can.cpp",
    "modules/ecu/flash_ecu_subaru_hitachi_sh72543r_can_operation.cpp",
    "modules/ecu/flash_ecu_subaru_mitsu_m32r_kline.cpp",
    "modules/ecu/flash_ecu_subaru_mitsu_m32r_kline_operation.cpp",
    "modules/ecu/flash_ecu_subaru_unisia_jecs.cpp",
    "modules/ecu/flash_ecu_subaru_unisia_jecs_m32r.cpp",
    "modules/ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.cpp",
    "modules/ecu/flash_ecu_subaru_unisia_jecs_operation.cpp",
    "modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can.cpp",
    "modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.cpp",
    "modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.cpp",
    "modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.cpp",
    "modules/flash_operation_worker.cpp",
    "modules/flash_utils.cpp",
    "modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag.cpp",
    "modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.cpp",
    "modules/ssm_protocol.cpp",
    "modules/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can.cpp",
    "modules/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.cpp",
    "modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can.cpp",
    "modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.cpp",
    "modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can.cpp",
    "modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.cpp",
    "modules/tcu/flash_tcu_subaru_denso_sh705x_can.cpp",
    "modules/tcu/flash_tcu_subaru_denso_sh705x_can_operation.cpp",
    "modules/tcu/flash_tcu_subaru_hitachi_m32r_can.cpp",
    "modules/tcu/flash_tcu_subaru_hitachi_m32r_can_operation.cpp",
    "modules/tcu/flash_tcu_subaru_hitachi_m32r_kline.cpp",
    "modules/tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.cpp",
    "nrc_parser.cpp",
    "protocol/fastecu_can_transport.cpp",
    "protocol/fastecu_kline_transport.cpp",
    "protocol/fastecu_ssm_transport.cpp",
    "protocol/imut_dma_init.cpp",
    "protocol/mitsu_colt_can_cdbg_driver.cpp",
    "protocol/mitsu_colt_can_cdbg_protocol.cpp",
    "protocol/mitsu_colt_can_protocol.cpp",
    "protocol/mitsu_colt_can_vendor_ext_protocol.cpp",
    "protocol/mut_dma_codec.cpp",
    "protocol/mut_dma_driver.cpp",
    "protocol/mut_dma_freeform.cpp",
    "protocol/mut_dma_memory.cpp",
    "protocol_select.cpp",
    "remote_utility/remote_utility.cpp",
    "serial_port/remote_serial_backend.cpp",
    "serial_port/serial_backend_host.cpp",
    "serial_port/serial_port_actions.cpp",
    "serial_port/serial_port_actions_direct.cpp",
    "serial_port/websocketiodevice.cpp",
    "settings.cpp",
    "systemlogger.cpp",
    "vehicle_select.cpp",
    "verticallabel.cpp",
]

APP_COMMON_HDRS = [
    "cipher.h",
    "dtc_parser.h",
    "error_codes.h",
    "expression_evaluator.h",
    "kernelcomms.h",
    "kernelmemorymodels.h",
    "logging/logging_protocol.h",
    "logging/protocols/cdbg_logging_protocol.h",
    "logging/protocols/mut_dma_logging_protocol.h",
    "logging/protocols/ssm_logging_protocol.h",
    "logging/romraider_conversion.h",
    "menu_command.h",
    "modules/checksum/checksum_ecu_subaru_denso_sh705x_diesel.h",
    "modules/checksum/checksum_ecu_subaru_denso_sh7xxx.h",
    "modules/checksum/checksum_ecu_subaru_hitachi_m32r_can.h",
    "modules/checksum/checksum_ecu_subaru_hitachi_m32r_kline.h",
    "modules/checksum/checksum_ecu_subaru_hitachi_sh7058.h",
    "modules/checksum/checksum_ecu_subaru_hitachi_sh72543r.h",
    "modules/checksum/checksum_result.h",
    "modules/checksum/checksum_tcu_mitsu_mh8104_can.h",
    "modules/checksum/checksum_tcu_subaru_denso_sh7055.h",
    "modules/checksum/checksum_tcu_subaru_hitachi_m32r_can.h",
    "modules/flash_utils.h",
    "modules/ssm_protocol_core.h",
    "modules/ssm_protocol.h",
    "nrc_parser.h",
    "protocol/bytes.h",
    "protocol/fastecu_can_transport.h",
    "protocol/fastecu_kline_transport.h",
    "protocol/fastecu_ssm_transport.h",
    "protocol/ican_transport.h",
    "protocol/ikline_transport.h",
    "protocol/imut_dma_init.h",
    "protocol/issm_transport.h",
    "protocol/mitsu_colt_can_cdbg_driver.h",
    "protocol/mitsu_colt_can_cdbg_protocol.h",
    "protocol/mitsu_colt_can_protocol.h",
    "protocol/mitsu_colt_can_vendor_ext_protocol.h",
    "protocol/mut_dma_codec.h",
    "protocol/mut_dma_driver.h",
    "protocol/mut_dma_freeform.h",
    "protocol/mut_dma_memory.h",
    "protocol/qt_bytes.h",
    "serial_port/j2534_driver_selection.h",
    "serial_port/qtrohelper.hpp",
    "serial_port/serial_backend.h",
    "serial_port/serial_backend_host.h",
]

APP_MOC_HDRS = [
    "calibration_maps.h",
    "calibration_treewidget.h",
    "dataterminal.h",
    "definition_file_convert.h",
    "dtc_operations.h",
    "ecu_operations.h",
    "file_actions.h",
    "get_key_operations_subaru.h",
    "hexedit/hexedit.h",
    "hexedit/optionsdialog.h",
    "hexedit/qhexedit/chunks.h",
    "hexedit/qhexedit/commands.h",
    "hexedit/qhexedit/qhexedit.h",
    "hexedit/searchdialog.h",
    "logbox.h",
    "logging/logging_engine.h",
    "logging/logging_worker.h",
    "mainwindow.h",
    "modules/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm.h",
    "modules/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm_operation.h",
    "modules/biu/biu_operations_subaru.h",
    "modules/biu/biu_ops_subaru_data.h",
    "modules/biu/biu_ops_subaru_dtcs.h",
    "modules/biu/biu_ops_subaru_input1.h",
    "modules/biu/biu_ops_subaru_input2.h",
    "modules/biu/biu_ops_subaru_switches.h",
    "modules/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode.h",
    "modules/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.h",
    "modules/ecu/flash_ecu_mitsu_m32r_can.h",
    "modules/ecu/flash_ecu_mitsu_m32r_can_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.h",
    "modules/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_1n83m_4m_can.h",
    "modules/ecu/flash_ecu_subaru_denso_1n83m_4m_can_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_mc68hc16y5_02.h",
    "modules/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_sh7055_02.h",
    "modules/ecu/flash_ecu_subaru_denso_sh7055_02_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_sh7058_can.h",
    "modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel.h",
    "modules/ecu/flash_ecu_subaru_denso_sh7058_can_diesel_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_sh7058_can_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_sh705x_densocan.h",
    "modules/ecu/flash_ecu_subaru_denso_sh705x_densocan_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_sh705x_kline.h",
    "modules/ecu/flash_ecu_subaru_denso_sh705x_kline_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_sh72531_can.h",
    "modules/ecu/flash_ecu_subaru_denso_sh72531_can_operation.h",
    "modules/ecu/flash_ecu_subaru_denso_sh72543_can_diesel.h",
    "modules/ecu/flash_ecu_subaru_denso_sh72543_can_diesel_operation.h",
    "modules/ecu/flash_ecu_subaru_hitachi_m32r_can.h",
    "modules/ecu/flash_ecu_subaru_hitachi_m32r_can_operation.h",
    "modules/ecu/flash_ecu_subaru_hitachi_m32r_kline.h",
    "modules/ecu/flash_ecu_subaru_hitachi_m32r_kline_operation.h",
    "modules/ecu/flash_ecu_subaru_hitachi_sh7058_can.h",
    "modules/ecu/flash_ecu_subaru_hitachi_sh7058_can_operation.h",
    "modules/ecu/flash_ecu_subaru_hitachi_sh72543r_can.h",
    "modules/ecu/flash_ecu_subaru_hitachi_sh72543r_can_operation.h",
    "modules/ecu/flash_ecu_subaru_mitsu_m32r_kline.h",
    "modules/ecu/flash_ecu_subaru_mitsu_m32r_kline_operation.h",
    "modules/ecu/flash_ecu_subaru_unisia_jecs.h",
    "modules/ecu/flash_ecu_subaru_unisia_jecs_m32r.h",
    "modules/ecu/flash_ecu_subaru_unisia_jecs_m32r_operation.h",
    "modules/ecu/flash_ecu_subaru_unisia_jecs_operation.h",
    "modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can.h",
    "modules/eeprom/eeprom_ecu_subaru_denso_sh705x_can_operation.h",
    "modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.h",
    "modules/eeprom/eeprom_ecu_subaru_denso_sh705x_kline_operation.h",
    "modules/flash_operation_worker.h",
    "modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag.h",
    "modules/jtag/flash_ecu_subaru_hitachi_m32r_jtag_operation.h",
    "modules/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can.h",
    "modules/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can_operation.h",
    "modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can.h",
    "modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can_operation.h",
    "modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can.h",
    "modules/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.h",
    "modules/tcu/flash_tcu_subaru_denso_sh705x_can.h",
    "modules/tcu/flash_tcu_subaru_denso_sh705x_can_operation.h",
    "modules/tcu/flash_tcu_subaru_hitachi_m32r_can.h",
    "modules/tcu/flash_tcu_subaru_hitachi_m32r_can_operation.h",
    "modules/tcu/flash_tcu_subaru_hitachi_m32r_kline.h",
    "modules/tcu/flash_tcu_subaru_hitachi_m32r_kline_operation.h",
    "protocol_select.h",
    "remote_utility/remote_utility.h",
    "serial_port/J2534_unix.h",
    "serial_port/remote_serial_backend.h",
    "serial_port/serial_port_actions.h",
    "serial_port/serial_port_actions_direct.h",
    "serial_port/websocketiodevice.h",
    "settings.h",
    "systemlogger.h",
    "vehicle_select.h",
    "verticallabel.h",
]

APP_FORMS = [
    "calibration_map_table.ui",
    "data_terminal.ui",
    "definition_file_convert.ui",
    "dtc_operations.ui",
    "ecu_operations.ui",
    "hexedit/optionsdialog.ui",
    "hexedit/searchdialog.ui",
    "logvalues.ui",
    "mainwindow.ui",
    "modules/biu/biu_operations_subaru.ui",
    "modules/biu/biu_ops_subaru_data.ui",
    "modules/biu/biu_ops_subaru_dtcs.ui",
    "modules/biu/biu_ops_subaru_input1.ui",
    "modules/biu/biu_ops_subaru_input2.ui",
    "modules/biu/biu_ops_subaru_switches.ui",
    "protocol_select.ui",
    "settings.ui",
    "vehicle_select.ui",
]

APP_RESOURCES = [
    "config.qrc",
    "fonts.qrc",
    "hexedit/qhexedit.qrc",
    "icons.qrc",
    "images.qrc",
    "kernels.qrc",
]

APP_REPS = [
    "remote_utility/remote_utility.rep",
    "serial_port/serial_port_actions.rep",
]

APP_UNIX_SRCS = [
    "serial_port/J2534_unix.cpp",
]

APP_UNIX_HDRS = [
    "serial_port/J2534_tactrix_unix.h",
    "serial_port/J2534_unix.h",
]

APP_WIN_SRCS = [
    "serial_port/J2534_win.cpp",
    "serial_port/j2534_bridge_protocol.cpp",
    "serial_port/pe_bitness.cpp",
]

APP_WIN_HDRS = [
    "serial_port/J2534_tactrix_win.h",
    "serial_port/J2534_win.h",
    "serial_port/j2534_bridge_protocol.h",
    "serial_port/pe_bitness.h",
]

MUT_DMA_TESTS_COMMON_SRCS = [
    "../dtc_parser.cpp",
    "../expression_evaluator.cpp",
    "../file_actions.cpp",
    "../file_defs_ecuflash.cpp",
    "../file_defs_romraider.cpp",
    "../logging/logging_engine.cpp",
    "../logging/logging_worker.cpp",
    "../logging/protocols/cdbg_logging_protocol.cpp",
    "../logging/protocols/mut_dma_logging_protocol.cpp",
    "../logging/protocols/ssm_logging_protocol.cpp",
    "../logging/romraider_conversion.cpp",
    "../menu_command.cpp",
    "../modules/checksum/checksum_ecu_subaru_denso_sh705x_diesel.cpp",
    "../modules/checksum/checksum_ecu_subaru_denso_sh7xxx.cpp",
    "../modules/checksum/checksum_ecu_subaru_hitachi_m32r_can.cpp",
    "../modules/checksum/checksum_ecu_subaru_hitachi_m32r_kline.cpp",
    "../modules/checksum/checksum_ecu_subaru_hitachi_sh7058.cpp",
    "../modules/checksum/checksum_ecu_subaru_hitachi_sh72543r.cpp",
    "../modules/checksum/checksum_tcu_mitsu_mh8104_can.cpp",
    "../modules/checksum/checksum_tcu_subaru_denso_sh7055.cpp",
    "../modules/checksum/checksum_tcu_subaru_hitachi_m32r_can.cpp",
    "../modules/ecu/flash_ecu_mitsu_m32r_can_operation.cpp",
    "../modules/flash_operation_worker.cpp",
    "../modules/flash_utils.cpp",
    "../modules/ssm_protocol.cpp",
    "../nrc_parser.cpp",
    "../serial_port/remote_serial_backend.cpp",
    "../serial_port/serial_backend_host.cpp",
    "../serial_port/serial_port_actions.cpp",
    "../serial_port/serial_port_actions_direct.cpp",
    "../serial_port/websocketiodevice.cpp",
    "protocol/fastecu_can_transport.cpp",
    "protocol/fastecu_kline_transport.cpp",
    "protocol/fastecu_ssm_transport.cpp",
    "protocol/imut_dma_init.cpp",
    "protocol/mitsu_colt_can_cdbg_driver.cpp",
    "protocol/mitsu_colt_can_cdbg_protocol.cpp",
    "protocol/mitsu_colt_can_protocol.cpp",
    "protocol/mitsu_colt_can_vendor_ext_protocol.cpp",
    "protocol/mut_dma_codec.cpp",
    "protocol/mut_dma_driver.cpp",
    "protocol/mut_dma_freeform.cpp",
    "protocol/mut_dma_memory.cpp",
    "tests/main.cpp",
    "tests/test_cdbg_driver.cpp",
    "tests/test_cdbg_logging_protocol.cpp",
    "tests/test_checksum_results.cpp",
    "tests/test_codec.cpp",
    "tests/test_diagnostic_parsers.cpp",
    "tests/test_driver.cpp",
    "tests/test_ecuflash_definition_parsing.cpp",
    "tests/test_expression_evaluator.cpp",
    "tests/test_flash_ecu_mitsu_m32r_can_operation.cpp",
    "tests/test_flash_operation_worker.cpp",
    "tests/test_flash_utils.cpp",
    "tests/test_freeform.cpp",
    "tests/test_init.cpp",
    "tests/test_logging_engine.cpp",
    "tests/test_logging_worker.cpp",
    "tests/test_memory.cpp",
    "tests/test_menu_command.cpp",
    "tests/test_mitsu_colt_can_cdbg_protocol.cpp",
    "tests/test_mitsu_colt_can_protocol.cpp",
    "tests/test_mitsu_colt_can_vendor_ext_protocol.cpp",
    "tests/test_model_validation.cpp",
    "tests/test_mut_dma_logging_protocol.cpp",
    "tests/test_romraider_conversion.cpp",
    "tests/test_ssm_logging_protocol.cpp",
    "tests/test_bytes.cpp",
    "tests/test_ssm_protocol.cpp",
    "tests/test_transport.cpp",
]

MUT_DMA_TESTS_COMMON_HDRS = [
    "../dtc_parser.h",
    "../expression_evaluator.h",
    "../file_actions.h",
    "../logging/logging_engine.h",
    "../logging/logging_protocol.h",
    "../logging/logging_worker.h",
    "../logging/protocols/cdbg_logging_protocol.h",
    "../logging/protocols/mut_dma_logging_protocol.h",
    "../logging/protocols/ssm_logging_protocol.h",
    "../logging/romraider_conversion.h",
    "../menu_command.h",
    "../modules/checksum/checksum_result.h",
    "../modules/ecu/flash_ecu_mitsu_m32r_can_operation.h",
    "../modules/flash_operation_worker.h",
    "../modules/flash_utils.h",
    "../modules/ssm_protocol_core.h",
    "../modules/ssm_protocol.h",
    "../nrc_parser.h",
    "../protocol/bytes.h",
    "../serial_port/j2534_driver_selection.h",
    "../serial_port/remote_serial_backend.h",
    "../serial_port/serial_backend.h",
    "../serial_port/serial_backend_host.h",
    "../serial_port/serial_port_actions.h",
    "../serial_port/serial_port_actions_direct.h",
    "../serial_port/websocketiodevice.h",
    "protocol/fastecu_can_transport.h",
    "protocol/fastecu_kline_transport.h",
    "protocol/fastecu_ssm_transport.h",
    "protocol/ican_transport.h",
    "protocol/ikline_transport.h",
    "protocol/imut_dma_init.h",
    "protocol/issm_transport.h",
    "protocol/mitsu_colt_can_cdbg_driver.h",
    "protocol/mitsu_colt_can_cdbg_protocol.h",
    "protocol/mitsu_colt_can_protocol.h",
    "protocol/mitsu_colt_can_vendor_ext_protocol.h",
    "protocol/mut_dma_codec.h",
    "protocol/mut_dma_driver.h",
    "protocol/mut_dma_freeform.h",
    "protocol/mut_dma_memory.h",
    "../protocol/qt_bytes.h",
    "tests/byte_test_utils.h",
    "tests/fake_backend.h",
    "tests/scripted_can_transport.h",
    "tests/scripted_kline_transport.h",
    "tests/scripted_logging_protocol.h",
    "tests/scripted_ssm_transport.h",
    "tests/test_bytes.h",
    "tests/test_cdbg_driver.h",
    "tests/test_cdbg_logging_protocol.h",
    "tests/test_checksum_results.h",
    "tests/test_codec.h",
    "tests/test_diagnostic_parsers.h",
    "tests/test_driver.h",
    "tests/test_ecuflash_definition_parsing.h",
    "tests/test_expression_evaluator.h",
    "tests/test_flash_ecu_mitsu_m32r_can_operation.h",
    "tests/test_flash_operation_worker.h",
    "tests/test_flash_utils.h",
    "tests/test_freeform.h",
    "tests/test_init.h",
    "tests/test_logging_engine.h",
    "tests/test_logging_worker.h",
    "tests/test_memory.h",
    "tests/test_menu_command.h",
    "tests/test_mitsu_colt_can_cdbg_protocol.h",
    "tests/test_mitsu_colt_can_protocol.h",
    "tests/test_mitsu_colt_can_vendor_ext_protocol.h",
    "tests/test_model_validation.h",
    "tests/test_mut_dma_logging_protocol.h",
    "tests/test_romraider_conversion.h",
    "tests/test_ssm_logging_protocol.h",
    "tests/test_ssm_protocol.h",
    "tests/test_transport.h",
]

MUT_DMA_TESTS_COMMON_REPS = [
    "../remote_utility/remote_utility.rep",
    "../serial_port/serial_port_actions.rep",
]

MUT_DMA_TESTS_UNIX_SRCS = [
    "../serial_port/J2534_unix.cpp",
]

MUT_DMA_TESTS_UNIX_HDRS = [
    "../serial_port/J2534_unix.h",
]

MUT_DMA_TESTS_UNIX_REPS = [
]

MUT_DMA_TESTS_WIN32_SRCS = [
    "../serial_port/J2534_win.cpp",
    "../serial_port/j2534_bridge_protocol.cpp",
    "../serial_port/pe_bitness.cpp",
]

MUT_DMA_TESTS_WIN32_HDRS = [
    "../serial_port/J2534_tactrix_win.h",
    "../serial_port/J2534_win.h",
    "../serial_port/j2534_bridge_protocol.h",
    "../serial_port/pe_bitness.h",
]

MUT_DMA_TESTS_WIN32_REPS = [
]

SERIAL_BACKEND_TESTS_COMMON_SRCS = [
    "../serial_port/remote_serial_backend.cpp",
    "../serial_port/serial_backend_host.cpp",
    "../serial_port/serial_port_actions.cpp",
    "../serial_port/serial_port_actions_direct.cpp",
    "../serial_port/websocketiodevice.cpp",
    "tests/serial_backend_main.cpp",
    "tests/test_direct_backend.cpp",
    "tests/test_facade_threading.cpp",
    "tests/test_remote_backend_smoke.cpp",
]

SERIAL_BACKEND_TESTS_COMMON_HDRS = [
    "../serial_port/remote_serial_backend.h",
    "../serial_port/serial_backend.h",
    "../serial_port/serial_backend_host.h",
    "../serial_port/serial_port_actions.h",
    "../serial_port/serial_port_actions_direct.h",
    "../serial_port/websocketiodevice.h",
    "tests/fake_backend.h",
    "tests/mock_openport.h",
    "tests/test_direct_backend.h",
    "tests/test_facade_threading.h",
    "tests/test_remote_backend_smoke.h",
]

SERIAL_BACKEND_TESTS_COMMON_REPS = [
    "../serial_port/serial_port_actions.rep",
]

SERIAL_BACKEND_TESTS_UNIX_SRCS = [
    "../serial_port/J2534_unix.cpp",
    "tests/test_direct_backend_pty.cpp",
    "tests/test_pty_e2e.cpp",
]

SERIAL_BACKEND_TESTS_UNIX_HDRS = [
    "../serial_port/J2534_unix.h",
    "tests/test_direct_backend_pty.h",
    "tests/test_pty_e2e.h",
]

SERIAL_BACKEND_TESTS_UNIX_REPS = [
]

SERIAL_BACKEND_TESTS_WIN32_SRCS = [
]

SERIAL_BACKEND_TESTS_WIN32_HDRS = [
]

SERIAL_BACKEND_TESTS_WIN32_REPS = [
]

SERIAL_CRASH_TESTS_COMMON_SRCS = [
    "../serial_port/J2534_unix.cpp",
    "../serial_port/serial_port_actions_direct.cpp",
    "tests/tst_serial_port_crash.cpp",
]

SERIAL_CRASH_TESTS_COMMON_HDRS = [
    "../serial_port/J2534_unix.h",
    "../serial_port/serial_port_actions_direct.h",
    "tests/mock_openport.h",
]

SERIAL_CRASH_TESTS_COMMON_REPS = [
]

SERIAL_CRASH_TESTS_UNIX_SRCS = [
]

SERIAL_CRASH_TESTS_UNIX_HDRS = [
]

SERIAL_CRASH_TESTS_UNIX_REPS = [
]

SERIAL_CRASH_TESTS_WIN32_SRCS = [
]

SERIAL_CRASH_TESTS_WIN32_HDRS = [
]

SERIAL_CRASH_TESTS_WIN32_REPS = [
]

MUT_DMA_INTEGRATION_TESTS_COMMON_SRCS = [
    "../protocol/fastecu_kline_transport.cpp",
    "../protocol/imut_dma_init.cpp",
    "../protocol/mut_dma_codec.cpp",
    "../protocol/mut_dma_driver.cpp",
    "../protocol/mut_dma_freeform.cpp",
    "../protocol/mut_dma_memory.cpp",
    "../serial_port/J2534_unix.cpp",
    "../serial_port/remote_serial_backend.cpp",
    "../serial_port/serial_backend_host.cpp",
    "../serial_port/serial_port_actions.cpp",
    "../serial_port/serial_port_actions_direct.cpp",
    "../serial_port/websocketiodevice.cpp",
    "tests/tst_mut_dma_integration.cpp",
]

MUT_DMA_INTEGRATION_TESTS_COMMON_HDRS = [
    "../serial_port/J2534_unix.h",
    "../serial_port/remote_serial_backend.h",
    "../serial_port/serial_backend.h",
    "../serial_port/serial_backend_host.h",
    "../serial_port/serial_port_actions.h",
    "../serial_port/serial_port_actions_direct.h",
    "../serial_port/websocketiodevice.h",
]

MUT_DMA_INTEGRATION_TESTS_COMMON_REPS = [
    "../remote_utility/remote_utility.rep",
    "../serial_port/serial_port_actions.rep",
]

MUT_DMA_INTEGRATION_TESTS_UNIX_SRCS = [
]

MUT_DMA_INTEGRATION_TESTS_UNIX_HDRS = [
]

MUT_DMA_INTEGRATION_TESTS_UNIX_REPS = [
]

MUT_DMA_INTEGRATION_TESTS_WIN32_SRCS = [
]

MUT_DMA_INTEGRATION_TESTS_WIN32_HDRS = [
]

MUT_DMA_INTEGRATION_TESTS_WIN32_REPS = [
]

TST_FORCE_ASSERTS_COMMON_SRCS = [
    "tests/force_asserts/tst_force_asserts.cpp",
]

TST_FORCE_ASSERTS_COMMON_HDRS = [
]

TST_FORCE_ASSERTS_COMMON_REPS = [
]

TST_FORCE_ASSERTS_UNIX_SRCS = [
]

TST_FORCE_ASSERTS_UNIX_HDRS = [
]

TST_FORCE_ASSERTS_UNIX_REPS = [
]

TST_FORCE_ASSERTS_WIN32_SRCS = [
]

TST_FORCE_ASSERTS_WIN32_HDRS = [
]

TST_FORCE_ASSERTS_WIN32_REPS = [
]
