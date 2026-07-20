# Source lists originally generated from qmake project files during the initial Bazel migration; Bazel is now the sole source of truth for these lists.

"""Explicit FastECU source manifests generated from qmake project files."""

APP_MAIN_SRCS = [
    "apps/desktop/main.cpp",
]

APP_COMMON_SRCS = [
    "src/ui/desktop/calibration_maps.cpp",
    "src/ui/desktop/calibration_treewidget.cpp",
    "src/ui/desktop/dataterminal.cpp",
    "src/ui/desktop/definition_file_convert.cpp",
    "src/ui/desktop/dtc_operations.cpp",
    "src/ui/desktop/ecu_operations.cpp",
    "src/ui/desktop/get_key_operations_subaru.cpp",
    "src/ui/desktop/hexedit/hexedit.cpp",
    "src/ui/desktop/hexedit/optionsdialog.cpp",
    "src/ui/desktop/hexedit/qhexedit/chunks.cpp",
    "src/ui/desktop/hexedit/qhexedit/commands.cpp",
    "src/ui/desktop/hexedit/qhexedit/qhexedit.cpp",
    "src/ui/desktop/hexedit/searchdialog.cpp",
    "src/ui/desktop/log_operations_ssm.cpp",
    "src/ui/desktop/logbox.cpp",
    "src/ui/desktop/logvalues.cpp",
    "src/ui/desktop/mainwindow.cpp",
    "src/ui/desktop/menu_actions.cpp",
    "src/ui/desktop/flash/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm.cpp",
    "src/ui/desktop/biu/biu_operations_subaru.cpp",
    "src/ui/desktop/biu/biu_ops_subaru_data.cpp",
    "src/ui/desktop/biu/biu_ops_subaru_dtcs.cpp",
    "src/ui/desktop/biu/biu_ops_subaru_input1.cpp",
    "src/ui/desktop/biu/biu_ops_subaru_input2.cpp",
    "src/ui/desktop/biu/biu_ops_subaru_switches.cpp",
    "src/ui/desktop/flash/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_1n83m_4m_can.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_mc68hc16y5_02.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7055_02.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7058_can.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7058_can_diesel.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_densocan.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_kline.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh72531_can.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh72543_can_diesel.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_m32r_can.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_m32r_kline.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_sh7058_can.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_sh72543r_can.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_mitsu_m32r_kline.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.cpp",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs_m32r.cpp",
    "src/ui/desktop/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_can.cpp",
    "src/ui/desktop/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.cpp",
    "src/ui/desktop/flash/jtag/flash_ecu_subaru_hitachi_m32r_jtag.cpp",
    "src/ui/desktop/flash/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can.cpp",
    "src/ui/desktop/flash/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can.cpp",
    "src/ui/desktop/flash/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can.cpp",
    "src/ui/desktop/flash/tcu/flash_tcu_subaru_denso_sh705x_can.cpp",
    "src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_can.cpp",
    "src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_kline.cpp",
    "src/ui/desktop/protocol_select.cpp",
    "src/ui/desktop/settings.cpp",
    "src/ui/desktop/vehicle_select.cpp",
    "src/ui/desktop/verticallabel.cpp",
]

APP_COMMON_HDRS = []

APP_MOC_HDRS = [
    "src/ui/desktop/calibration_maps.h",
    "src/ui/desktop/calibration_treewidget.h",
    "src/ui/desktop/dataterminal.h",
    "src/ui/desktop/definition_file_convert.h",
    "src/ui/desktop/dtc_operations.h",
    "src/ui/desktop/ecu_operations.h",
    "src/ui/desktop/get_key_operations_subaru.h",
    "src/ui/desktop/hexedit/hexedit.h",
    "src/ui/desktop/hexedit/optionsdialog.h",
    "src/ui/desktop/hexedit/qhexedit/chunks.h",
    "src/ui/desktop/hexedit/qhexedit/commands.h",
    "src/ui/desktop/hexedit/qhexedit/qhexedit.h",
    "src/ui/desktop/hexedit/searchdialog.h",
    "src/ui/desktop/logbox.h",
    "src/ui/desktop/mainwindow.h",
    "src/ui/desktop/flash/bdm/flash_ecu_subaru_denso_mc68hc16y5_02_bdm.h",
    "src/ui/desktop/biu/biu_operations_subaru.h",
    "src/ui/desktop/biu/biu_ops_subaru_data.h",
    "src/ui/desktop/biu/biu_ops_subaru_dtcs.h",
    "src/ui/desktop/biu/biu_ops_subaru_input1.h",
    "src/ui/desktop/biu/biu_ops_subaru_input2.h",
    "src/ui/desktop/biu/biu_ops_subaru_switches.h",
    "src/ui/desktop/flash/bootmode/flash_ecu_subaru_unisia_jecs_m32r_bootmode.h",
    "src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_1n83m_4m_can.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_mc68hc16y5_02.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7055_02.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7058_can.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh7058_can_diesel.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_densocan.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh705x_kline.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh72531_can.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_denso_sh72543_can_diesel.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_m32r_can.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_m32r_kline.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_sh7058_can.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_hitachi_sh72543r_can.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_mitsu_m32r_kline.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs.h",
    "src/ui/desktop/flash/ecu/flash_ecu_subaru_unisia_jecs_m32r.h",
    "src/ui/desktop/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_can.h",
    "src/ui/desktop/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.h",
    "src/ui/desktop/flash/jtag/flash_ecu_subaru_hitachi_m32r_jtag.h",
    "src/ui/desktop/flash/tcu/flash_tcu_cvt_subaru_hitachi_m32r_can.h",
    "src/ui/desktop/flash/tcu/flash_tcu_cvt_subaru_mitsu_mh8104_can.h",
    "src/ui/desktop/flash/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can.h",
    "src/ui/desktop/flash/tcu/flash_tcu_subaru_denso_sh705x_can.h",
    "src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_can.h",
    "src/ui/desktop/flash/tcu/flash_tcu_subaru_hitachi_m32r_kline.h",
    "src/ui/desktop/protocol_select.h",
    "src/ui/desktop/settings.h",
    "src/ui/desktop/vehicle_select.h",
    "src/ui/desktop/verticallabel.h",
]

APP_FORMS = [
    "src/ui/desktop/calibration_map_table.ui",
    "src/ui/desktop/data_terminal.ui",
    "src/ui/desktop/definition_file_convert.ui",
    "src/ui/desktop/dtc_operations.ui",
    "src/ui/desktop/ecu_operations.ui",
    "src/ui/desktop/hexedit/optionsdialog.ui",
    "src/ui/desktop/hexedit/searchdialog.ui",
    "src/ui/desktop/logvalues.ui",
    "src/ui/desktop/mainwindow.ui",
    "src/ui/desktop/biu/biu_operations_subaru.ui",
    "src/ui/desktop/biu/biu_ops_subaru_data.ui",
    "src/ui/desktop/biu/biu_ops_subaru_dtcs.ui",
    "src/ui/desktop/biu/biu_ops_subaru_input1.ui",
    "src/ui/desktop/biu/biu_ops_subaru_input2.ui",
    "src/ui/desktop/biu/biu_ops_subaru_switches.ui",
    "src/ui/desktop/protocol_select.ui",
    "src/ui/desktop/settings.ui",
    "src/ui/desktop/vehicle_select.ui",
]

APP_RESOURCES = [
    "resources/shared/config.qrc",
    "resources/desktop/fonts.qrc",
    "resources/desktop/hexedit/qhexedit.qrc",
    "resources/desktop/icons.qrc",
    "resources/desktop/images.qrc",
    "resources/shared/kernels.qrc",
]

MUT_DMA_TESTS_COMMON_SRCS = [
    "../src/algorithms/diagnostics/dtc_parser.cpp",
    "../src/algorithms/expression/expression_evaluator.cpp",
    "../src/backend/definitions/file_actions.cpp",
    "../src/backend/definitions/file_defs_ecuflash.cpp",
    "../src/backend/definitions/file_defs_romraider.cpp",
    "../src/backend/logging/logging_engine.cpp",
    "../src/backend/logging/logging_worker.cpp",
    "../src/backend/logging/protocols/cdbg_logging_protocol.cpp",
    "../src/backend/logging/protocols/mut_dma_logging_protocol.cpp",
    "../src/backend/logging/protocols/ssm_logging_protocol.cpp",
    "../src/backend/logging/romraider_conversion.cpp",
    "../src/algorithms/menu/menu_command.cpp",
    "../src/algorithms/checksum/checksum_ecu_subaru_denso_sh705x_diesel.cpp",
    "../src/algorithms/checksum/checksum_ecu_subaru_denso_sh7xxx.cpp",
    "../src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_can.cpp",
    "../src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_kline.cpp",
    "../src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh7058.cpp",
    "../src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh72543r.cpp",
    "../src/algorithms/checksum/checksum_tcu_mitsu_mh8104_can.cpp",
    "../src/algorithms/checksum/checksum_tcu_subaru_denso_sh7055.cpp",
    "../src/algorithms/checksum/checksum_tcu_subaru_hitachi_m32r_can.cpp",
    "../src/backend/flash/ecu/flash_ecu_mitsu_m32r_can_operation.cpp",
    "../src/backend/flash/flash_operation_worker.cpp",
    "../src/backend/flash/flash_utils.cpp",
    "../src/algorithms/protocol/ssm/ssm_protocol.cpp",
    "../src/algorithms/diagnostics/nrc_parser.cpp",
    "../src/platform/desktop/common/serial/remote_serial_backend.cpp",
    "../src/platform/desktop/common/serial/serial_backend_host.cpp",
    "../src/platform/desktop/common/serial/serial_port_actions.cpp",
    "../src/platform/desktop/common/serial/serial_port_actions_direct.cpp",
    "../src/platform/desktop/common/serial/websocketiodevice.cpp",
    "src/platform/desktop/common/transport/fastecu_can_transport.cpp",
    "src/platform/desktop/common/transport/fastecu_kline_transport.cpp",
    "src/platform/desktop/common/transport/fastecu_ssm_transport.cpp",
    "../src/backend/protocol/imut_dma_init.cpp",
    "../src/backend/protocol/mitsu_colt_can_cdbg_driver.cpp",
    "../src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.cpp",
    "../src/algorithms/protocol/colt/mitsu_colt_can_protocol.cpp",
    "../src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.cpp",
    "../src/algorithms/protocol/mut_dma/mut_dma_codec.cpp",
    "../src/backend/protocol/mut_dma_driver.cpp",
    "../src/algorithms/protocol/mut_dma/mut_dma_freeform.cpp",
    "../src/algorithms/protocol/mut_dma/mut_dma_memory.cpp",
    "tests/test_cdbg_driver.cpp",
    "tests/test_cdbg_logging_protocol.cpp",
    "tests/test_checksum_results.cpp",
    "tests/test_codec.cpp",
    "tests/test_diagnostic_parsers.cpp",
    "tests/test_driver.cpp",
    "tests/test_ecuflash_definition_parsing.cpp",
    "tests/test_file_actions_parsing.cpp",
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
    "tests/test_rom_transformations.cpp",
    "tests/test_romraider_conversion.cpp",
    "tests/test_ssm_logging_protocol.cpp",
    "tests/test_bytes.cpp",
    "tests/test_ssm_protocol.cpp",
    "tests/test_transport.cpp",
]

MUT_DMA_TESTS_COMMON_HDRS = [
    "../src/algorithms/diagnostics/dtc_parser.h",
    "../src/algorithms/expression/expression_evaluator.h",
    "../src/backend/definitions/file_actions.h",
    "../src/backend/logging/logging_engine.h",
    "../src/backend/logging/logging_protocol.h",
    "../src/backend/logging/logging_worker.h",
    "../src/backend/logging/protocols/cdbg_logging_protocol.h",
    "../src/backend/logging/protocols/mut_dma_logging_protocol.h",
    "../src/backend/logging/protocols/ssm_logging_protocol.h",
    "../src/backend/logging/romraider_conversion.h",
    "../src/algorithms/menu/menu_command.h",
    "../src/algorithms/checksum/checksum_result.h",
    "../src/backend/flash/ecu/flash_ecu_mitsu_m32r_can_operation.h",
    "../src/backend/flash/flash_operation_worker.h",
    "../src/backend/flash/flash_utils.h",
    "../src/algorithms/protocol/ssm/ssm_protocol_core.h",
    "../src/algorithms/protocol/ssm/ssm_protocol.h",
    "../src/algorithms/diagnostics/nrc_parser.h",
    "../src/algorithms/protocol/bytes.h",
    "../src/platform/desktop/common/serial/j2534_driver_selection.h",
    "../src/platform/desktop/common/serial/remote_serial_backend.h",
    "../src/platform/desktop/common/serial/serial_backend.h",
    "../src/platform/desktop/common/serial/serial_backend_host.h",
    "../src/platform/desktop/common/serial/serial_port_actions.h",
    "../src/platform/desktop/common/serial/serial_port_actions_direct.h",
    "../src/platform/desktop/common/serial/websocketiodevice.h",
    "src/platform/desktop/common/transport/fastecu_can_transport.h",
    "src/platform/desktop/common/transport/fastecu_kline_transport.h",
    "src/platform/desktop/common/transport/fastecu_ssm_transport.h",
    "../src/backend/protocol/ican_transport.h",
    "../src/backend/protocol/ikline_transport.h",
    "../src/backend/protocol/imut_dma_init.h",
    "../src/backend/protocol/issm_transport.h",
    "../src/backend/protocol/mitsu_colt_can_cdbg_driver.h",
    "../src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h",
    "../src/algorithms/protocol/colt/mitsu_colt_can_protocol.h",
    "../src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h",
    "../src/algorithms/protocol/mut_dma/mut_dma_codec.h",
    "../src/backend/protocol/mut_dma_driver.h",
    "../src/algorithms/protocol/mut_dma/mut_dma_freeform.h",
    "../src/algorithms/protocol/mut_dma/mut_dma_memory.h",
    "../src/algorithms/protocol/qt_bytes.h",
    "tests/byte_test_utils.h",
    "tests/expected_message_box.h",
    "tests/fake_backend.h",
    "tests/scripted_can_transport.h",
    "tests/scripted_kline_transport.h",
    "tests/scripted_logging_protocol.h",
    "tests/scripted_ssm_transport.h",
    "tests/test_cdbg_logging_protocol.h",
    "tests/test_checksum_results.h",
    "tests/test_diagnostic_parsers.h",
    "tests/test_ecuflash_definition_parsing.h",
    "tests/test_file_actions_parsing.h",
    "tests/test_flash_ecu_mitsu_m32r_can_operation.h",
    "tests/test_flash_operation_worker.h",
    "tests/test_flash_utils.h",
    "tests/test_logging_engine.h",
    "tests/test_logging_worker.h",
    "tests/test_menu_command.h",
    "tests/test_model_validation.h",
    "tests/test_mut_dma_logging_protocol.h",
    "tests/test_rom_transformations.h",
    "tests/test_romraider_conversion.h",
    "tests/test_ssm_logging_protocol.h",
]

MUT_DMA_TESTS_COMMON_REPS = [
    "../src/platform/desktop/common/remote_utility/remote_utility.rep",
    "../src/platform/desktop/common/serial/serial_port_actions.rep",
]

MUT_DMA_TESTS_UNIX_SRCS = [
    "../src/platform/desktop/unix/j2534/J2534_unix.cpp",
]

MUT_DMA_TESTS_UNIX_HDRS = [
    "../src/platform/desktop/unix/j2534/J2534_unix.h",
]

MUT_DMA_TESTS_UNIX_REPS = [
]

MUT_DMA_TESTS_WIN32_SRCS = [
    "../src/platform/desktop/windows/j2534/J2534_win.cpp",
    "../src/platform/desktop/windows/j2534/j2534_bridge_client.cpp",
    "../src/platform/desktop/windows/j2534/j2534_bridge_protocol.cpp",
    "../src/platform/desktop/windows/j2534/pe_bitness.cpp",
]

MUT_DMA_TESTS_WIN32_HDRS = [
    "../src/platform/desktop/windows/j2534/J2534_tactrix_win.h",
    "../src/platform/desktop/windows/j2534/J2534_win.h",
    "../src/platform/desktop/windows/j2534/j2534_bridge_client.h",
    "../src/platform/desktop/windows/j2534/j2534_bridge_protocol.h",
    "../src/platform/desktop/windows/j2534/pe_bitness.h",
]

MUT_DMA_TESTS_WIN32_REPS = [
]

SERIAL_BACKEND_TESTS_COMMON_SRCS = [
    "../src/platform/desktop/common/serial/remote_serial_backend.cpp",
    "../src/platform/desktop/common/serial/serial_backend_host.cpp",
    "../src/platform/desktop/common/serial/serial_port_actions.cpp",
    "../src/platform/desktop/common/serial/serial_port_actions_direct.cpp",
    "../src/platform/desktop/common/serial/websocketiodevice.cpp",
    "tests/serial_backend_main.cpp",
    "tests/test_direct_backend.cpp",
    "tests/test_facade_threading.cpp",
    "tests/test_remote_backend_smoke.cpp",
]

SERIAL_BACKEND_TESTS_COMMON_HDRS = [
    "../src/platform/desktop/common/serial/remote_serial_backend.h",
    "../src/platform/desktop/common/serial/serial_backend.h",
    "../src/platform/desktop/common/serial/serial_backend_host.h",
    "../src/platform/desktop/common/serial/serial_port_actions.h",
    "../src/platform/desktop/common/serial/serial_port_actions_direct.h",
    "../src/platform/desktop/common/serial/websocketiodevice.h",
    "tests/fake_backend.h",
    "tests/mock_openport.h",
    "tests/test_direct_backend.h",
    "tests/test_facade_threading.h",
    "tests/test_remote_backend_smoke.h",
]

SERIAL_BACKEND_TESTS_COMMON_REPS = [
    "../src/platform/desktop/common/serial/serial_port_actions.rep",
]

SERIAL_BACKEND_TESTS_UNIX_SRCS = [
    "../src/platform/desktop/unix/j2534/J2534_unix.cpp",
    "tests/test_direct_backend_pty.cpp",
    "tests/test_pty_e2e.cpp",
]

SERIAL_BACKEND_TESTS_UNIX_HDRS = [
    "../src/platform/desktop/unix/j2534/J2534_unix.h",
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
    "../src/platform/desktop/unix/j2534/J2534_unix.cpp",
    "../src/platform/desktop/common/serial/serial_port_actions_direct.cpp",
    "tests/tst_serial_port_crash.cpp",
]

SERIAL_CRASH_TESTS_COMMON_HDRS = [
    "../src/platform/desktop/unix/j2534/J2534_unix.h",
    "../src/platform/desktop/common/serial/serial_port_actions_direct.h",
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
    "../src/platform/desktop/common/transport/fastecu_kline_transport.cpp",
    "../src/backend/protocol/imut_dma_init.cpp",
    "../src/algorithms/protocol/mut_dma/mut_dma_codec.cpp",
    "../src/backend/protocol/mut_dma_driver.cpp",
    "../src/algorithms/protocol/mut_dma/mut_dma_freeform.cpp",
    "../src/algorithms/protocol/mut_dma/mut_dma_memory.cpp",
    "../src/platform/desktop/unix/j2534/J2534_unix.cpp",
    "../src/platform/desktop/common/serial/remote_serial_backend.cpp",
    "../src/platform/desktop/common/serial/serial_backend_host.cpp",
    "../src/platform/desktop/common/serial/serial_port_actions.cpp",
    "../src/platform/desktop/common/serial/serial_port_actions_direct.cpp",
    "../src/platform/desktop/common/serial/websocketiodevice.cpp",
    "tests/tst_mut_dma_integration.cpp",
]

MUT_DMA_INTEGRATION_TESTS_COMMON_HDRS = [
    "../src/platform/desktop/unix/j2534/J2534_unix.h",
    "../src/platform/desktop/common/serial/remote_serial_backend.h",
    "../src/platform/desktop/common/serial/serial_backend.h",
    "../src/platform/desktop/common/serial/serial_backend_host.h",
    "../src/platform/desktop/common/serial/serial_port_actions.h",
    "../src/platform/desktop/common/serial/serial_port_actions_direct.h",
    "../src/platform/desktop/common/serial/websocketiodevice.h",
]

MUT_DMA_INTEGRATION_TESTS_COMMON_REPS = [
    "../src/platform/desktop/common/remote_utility/remote_utility.rep",
    "../src/platform/desktop/common/serial/serial_port_actions.rep",
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
