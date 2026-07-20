"""Explicit test source manifests moved out of bazel/fastecu_sources.bzl.

These lists are consumed by tests/BUILD.bazel and bazel/mut_dma_test_suites.bzl
through local_test_srcs()/local_test_hdrs()/test_cpp_moc_srcs() (see
bazel/qt_targets.bzl), which keep only "tests/"-prefixed paths. The qmake-era
"../src/..." entries those helpers used to filter out have been dropped here.
"""

MUT_DMA_TESTS_COMMON_SRCS = [
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

SERIAL_BACKEND_TESTS_COMMON_SRCS = [
    "tests/serial_backend_main.cpp",
    "tests/test_direct_backend.cpp",
    "tests/test_facade_threading.cpp",
    "tests/test_remote_backend_smoke.cpp",
]

SERIAL_BACKEND_TESTS_COMMON_HDRS = [
    "tests/fake_backend.h",
    "tests/mock_openport.h",
    "tests/test_direct_backend.h",
    "tests/test_facade_threading.h",
    "tests/test_remote_backend_smoke.h",
]

SERIAL_BACKEND_TESTS_UNIX_SRCS = [
    "tests/test_direct_backend_pty.cpp",
    "tests/test_pty_e2e.cpp",
]

SERIAL_BACKEND_TESTS_UNIX_HDRS = [
    "tests/test_direct_backend_pty.h",
    "tests/test_pty_e2e.h",
]

SERIAL_CRASH_TESTS_COMMON_SRCS = [
    "tests/tst_serial_port_crash.cpp",
]

SERIAL_CRASH_TESTS_COMMON_HDRS = [
    "tests/mock_openport.h",
]

MUT_DMA_INTEGRATION_TESTS_COMMON_SRCS = [
    "tests/tst_mut_dma_integration.cpp",
]

MUT_DMA_INTEGRATION_TESTS_COMMON_HDRS = [
]
