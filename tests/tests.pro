QT += core testlib widgets xml serialport remoteobjects websockets
CONFIG += console c++20
CONFIG -= app_bundle
include(../hardening.pri)
include(../protocol/protocol.pri)
TEMPLATE = app
TARGET = mut_dma_tests
INCLUDEPATH += $$PWD/..

# CdbgLoggingProtocol::start() configures raw-CAN mode on the real
# SerialPortActions facade (see logging/protocols/cdbg_logging_protocol.cpp),
# and test_cdbg_logging_protocol.cpp default-constructs a real SerialPortActions
# to satisfy the constructor, so the facade and its QtRemoteObjects-generated
# replica must be linked in here too (same facade mut_dma_integration_tests.pro
# already pulls in for the same reason).
REPC_REPLICA = \
    ../serial_port/serial_port_actions.rep \
    ../remote_utility/remote_utility.rep

unix {
    SOURCES += ../serial_port/J2534_unix.cpp
    HEADERS += ../serial_port/J2534_unix.h
}

win32 {
    SOURCES += ../serial_port/J2534_win.cpp
    HEADERS += \
        ../serial_port/J2534_win.h \
        ../serial_port/J2534_tactrix_win.h
}

SOURCES += \
    test_flash_operation_worker.cpp \
    ../modules/flash_operation_worker.cpp \
    ../modules/flash_utils.cpp \
    ../dtc_parser.cpp \
    ../expression_evaluator.cpp \
    ../nrc_parser.cpp \
    test_ssm_protocol.cpp \
    ../modules/ssm_protocol.cpp \
    test_flash_utils.cpp \
    test_diagnostic_parsers.cpp \
    test_expression_evaluator.cpp \
    test_menu_command.cpp \
    test_model_validation.cpp \
    test_checksum_results.cpp \
    test_flash_ecu_mitsu_m32r_can_operation.cpp \
    ../modules/ecu/flash_ecu_mitsu_m32r_can_operation.cpp \
    ../serial_port/serial_port_actions.cpp \
    ../serial_port/serial_port_actions_direct.cpp \
    ../serial_port/remote_serial_backend.cpp \
    ../serial_port/serial_backend_host.cpp \
    ../serial_port/websocketiodevice.cpp \
    main.cpp \
    test_codec.cpp \
    test_freeform.cpp \
    test_memory.cpp \
    test_transport.cpp \
    test_init.cpp \
    test_driver.cpp \
    test_mitsu_colt_can_protocol.cpp \
    test_mitsu_colt_can_vendor_ext_protocol.cpp \
    test_mitsu_colt_can_cdbg_protocol.cpp \
    test_cdbg_driver.cpp \
    test_logging_worker.cpp \
    test_logging_engine.cpp \
    test_romraider_conversion.cpp \
    ../logging/romraider_conversion.cpp \
    test_ssm_logging_protocol.cpp \
    ../logging/protocols/ssm_logging_protocol.cpp \
    test_mut_dma_logging_protocol.cpp \
    ../logging/protocols/mut_dma_logging_protocol.cpp \
    test_cdbg_logging_protocol.cpp \
    ../logging/protocols/cdbg_logging_protocol.cpp \
    ../file_actions.cpp \
    ../file_defs_ecuflash.cpp \
    ../file_defs_romraider.cpp \
    ../menu_command.cpp \
    ../modules/checksum/checksum_ecu_subaru_denso_sh705x_diesel.cpp \
    ../modules/checksum/checksum_ecu_subaru_denso_sh7xxx.cpp \
    ../modules/checksum/checksum_ecu_subaru_hitachi_m32r_can.cpp \
    ../modules/checksum/checksum_ecu_subaru_hitachi_m32r_kline.cpp \
    ../modules/checksum/checksum_ecu_subaru_hitachi_sh7058.cpp \
    ../modules/checksum/checksum_ecu_subaru_hitachi_sh72543r.cpp \
    ../modules/checksum/checksum_tcu_mitsu_mh8104_can.cpp \
    ../modules/checksum/checksum_tcu_subaru_denso_sh7055.cpp \
    ../modules/checksum/checksum_tcu_subaru_hitachi_m32r_can.cpp \
    ../logging/logging_worker.cpp \
    ../logging/logging_engine.cpp
HEADERS += \
    byte_test_utils.h \
    test_flash_operation_worker.h \
    ../modules/flash_operation_worker.h \
    ../modules/flash_utils.h \
    ../dtc_parser.h \
    ../expression_evaluator.h \
    ../nrc_parser.h \
    test_ssm_protocol.h \
    ../modules/ssm_protocol_core.h \
    ../modules/ssm_protocol.h \
    ../protocol/bytes.h \
    ../protocol/qt_bytes.h \
    test_flash_utils.h \
    test_diagnostic_parsers.h \
    test_expression_evaluator.h \
    test_menu_command.h \
    test_model_validation.h \
    test_checksum_results.h \
    test_flash_ecu_mitsu_m32r_can_operation.h \
    ../modules/ecu/flash_ecu_mitsu_m32r_can_operation.h \
    fake_backend.h \
    ../serial_port/serial_port_actions.h \
    ../serial_port/serial_port_actions_direct.h \
    ../serial_port/j2534_driver_selection.h \
    ../serial_port/serial_backend.h \
    ../serial_port/remote_serial_backend.h \
    ../serial_port/serial_backend_host.h \
    ../serial_port/websocketiodevice.h \
    test_codec.h \
    test_freeform.h \
    test_memory.h \
    test_transport.h \
    test_init.h \
    test_driver.h \
    test_mitsu_colt_can_protocol.h \
    test_mitsu_colt_can_vendor_ext_protocol.h \
    test_mitsu_colt_can_cdbg_protocol.h \
    test_cdbg_driver.h \
    test_logging_worker.h \
    test_logging_engine.h \
    test_romraider_conversion.h \
    ../logging/romraider_conversion.h \
    test_ssm_logging_protocol.h \
    ../logging/protocols/ssm_logging_protocol.h \
    test_mut_dma_logging_protocol.h \
    ../logging/protocols/mut_dma_logging_protocol.h \
    test_cdbg_logging_protocol.h \
    ../logging/protocols/cdbg_logging_protocol.h \
    ../file_actions.h \
    ../menu_command.h \
    ../modules/checksum/checksum_result.h \
    scripted_kline_transport.h \
    scripted_can_transport.h \
    scripted_logging_protocol.h \
    ../logging/logging_protocol.h \
    ../logging/logging_worker.h \
    ../logging/logging_engine.h \
    scripted_ssm_transport.h
