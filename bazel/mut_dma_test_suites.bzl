"""One Bazel test target per mut_dma_tests suite, using QtTest or GoogleTest.

tests/main.cpp aggregates ~25 independent QTest suites into a single binary
via run_test_X(argc, argv) calls. Bundling them meant a hang or crash in any
one suite produced an opaque TIMEOUT/FAILED on the whole combined target,
with no indication of which suite was responsible. This macro instead
builds one independently selectable QtTest or GoogleTest target per suite.
"""

load("//bazel:gtest_targets.bzl", "fastecu_gtest")
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "COMMON_LINKOPTS", "QT_DEPS", "local_test_hdrs", "qt_cc_test")
load("//bazel:test_sources.bzl", "MUT_DMA_TESTS_COMMON_HDRS")

MUT_DMA_TEST_SUITES = [
    "test_codec",
    "test_freeform",
    "test_memory",
    "test_transport",
    "test_init",
    "test_driver",
    "test_mitsu_colt_can_protocol",
    "test_mitsu_colt_can_vendor_ext_protocol",
    "test_mitsu_colt_can_cdbg_protocol",
    "test_cdbg_driver",
    "test_logging_worker",
    "test_logging_engine",
    "test_romraider_conversion",
    "test_ssm_logging_protocol",
    "test_mut_dma_logging_protocol",
    "test_cdbg_logging_protocol",
    "test_flash_operation_worker",
    "test_flash_ecu_mitsu_m32r_can_operation",
    "test_flash_utils",
    "test_ssm_protocol",
    "test_bytes",
    "test_expression_evaluator",
    "test_menu_command",
    "test_diagnostic_parsers",
    "test_model_validation",
    "test_ecuflash_definition_parsing",
    "test_file_actions_parsing",
    "test_rom_transformations",
    "test_checksum_results",
]

MUT_DMA_GTEST_SUITES = [
    "test_bytes",
    "test_cdbg_driver",
    "test_codec",
    "test_driver",
    "test_expression_evaluator",
    "test_freeform",
    "test_init",
    "test_memory",
    "test_mitsu_colt_can_cdbg_protocol",
    "test_mitsu_colt_can_protocol",
    "test_mitsu_colt_can_vendor_ext_protocol",
    "test_ssm_protocol",
    "test_transport",
]

MUT_DMA_GTEST_SRCS = [base + ".cpp" for base in MUT_DMA_GTEST_SUITES]

_MUT_DMA_GTEST_HELPER_HDRS = {
    "test_cdbg_driver": [
        "byte_test_utils.h",
        "scripted_can_transport.h",
    ],
    "test_driver": [
        "byte_test_utils.h",
        "scripted_kline_transport.h",
    ],
    "test_init": ["scripted_kline_transport.h"],
    "test_mitsu_colt_can_protocol": ["byte_test_utils.h"],
    "test_mitsu_colt_can_vendor_ext_protocol": ["byte_test_utils.h"],
    "test_ssm_protocol": ["byte_test_utils.h"],
    "test_transport": ["scripted_kline_transport.h"],
}

# These suites construct a real QApplication (FileActions derives from
# QWidget), which needs a headless-safe platform plugin on CI runners with
# no display/desktop session. Setting QT_QPA_PLATFORM via Bazel's own env
# attribute means it's present in the test process's environment from the
# moment it starts.
_NEEDS_OFFSCREEN_QT_PLATFORM = [
    "test_cdbg_logging_protocol",
    "test_checksum_results",
    "test_ecuflash_definition_parsing",
    "test_file_actions_parsing",
    "test_flash_ecu_mitsu_m32r_can_operation",
    "test_flash_operation_worker",
    "test_mut_dma_logging_protocol",
    "test_rom_transformations",
    "test_romraider_conversion",
    "test_ssm_logging_protocol",
]

def mut_dma_test_suites(moc_deps_target, header_mocs_target):
    """Create one QtTest or GoogleTest target per MUT_DMA_TEST_SUITES entry.

    Args:
      moc_deps_target: label of the qt_cpp_moc_headers library providing
        each suite's generated .moc file (e.g. ":mut_dma_cpp_mocs").
      header_mocs_target: label of the shared test-header mock library
        (e.g. ":test_header_mocs").
    """
    for base in MUT_DMA_TEST_SUITES:
        if base in MUT_DMA_GTEST_SUITES:
            fastecu_gtest(
                name = base,
                srcs = [base + ".cpp"] + _MUT_DMA_GTEST_HELPER_HDRS.get(base, []),
            )
            continue

        env = {}
        if base in _NEEDS_OFFSCREEN_QT_PLATFORM:
            env = {
                "QT_QPA_PLATFORM": "offscreen",
            }
        qt_cc_test(
            name = base,
            srcs = [
                base + ".cpp",
                base + "_main.cpp",
            ] + local_test_hdrs(MUT_DMA_TESTS_COMMON_HDRS),
            copts = COMMON_COPTS + [
                "-I.",
                "-Itests",
                "-Isrc/platform/desktop/common/serial",
            ],
            linkopts = COMMON_LINKOPTS,
            env = env,
            deps = QT_DEPS + [
                moc_deps_target,
                header_mocs_target,
                "//:fastecu_core_common",
            ] + select({
                "@platforms//os:windows": ["//:fastecu_platform_windows"],
                "//conditions:default": ["//:fastecu_platform_unix"],
            }),
        )
