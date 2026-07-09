"""One Bazel test target per mut_dma_tests suite.

tests/main.cpp aggregates ~25 independent QTest suites into a single binary
via run_test_X(argc, argv) calls. Bundling them meant a hang or crash in any
one suite produced an opaque TIMEOUT/FAILED on the whole combined target,
with no indication of which suite was responsible. This macro instead
builds one qt_cc_test per suite, each linking a tiny "<suite>_main.cpp"
shim (tests/<suite>_main.cpp) that calls only that suite's run_test_X.
"""

load("//bazel:fastecu_sources.bzl", "MUT_DMA_TESTS_COMMON_HDRS")
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "COMMON_LINKOPTS", "QT_DEPS", "local_test_hdrs", "qt_cc_test")

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
    "test_expression_evaluator",
    "test_menu_command",
    "test_diagnostic_parsers",
    "test_model_validation",
    "test_checksum_results",
]

# These suites construct a real QApplication (FileActions derives from
# QWidget), which needs a headless-safe platform plugin on CI runners that
# have no display/desktop session. Setting QT_QPA_PLATFORM here, via
# Bazel's own env attribute, means it's present in the test process's
# environment from the moment it starts -- unlike an in-process qputenv()
# call, which was confirmed (via zero effect from two different env vars
# set that way) to not reliably reach Qt's environment reads in this
# Windows Bazel test-execution context.
_NEEDS_OFFSCREEN_QT_PLATFORM = [
    "test_cdbg_logging_protocol",
    "test_flash_ecu_mitsu_m32r_can_operation",
    "test_flash_operation_worker",
    "test_mut_dma_logging_protocol",
    "test_romraider_conversion",
    "test_ssm_logging_protocol",
]

def mut_dma_test_suites(moc_deps_target, header_mocs_target):
    """Create one qt_cc_test per entry in MUT_DMA_TEST_SUITES.

    Args:
      moc_deps_target: label of the qt_cpp_moc_headers library providing
        each suite's generated .moc file (e.g. ":mut_dma_cpp_mocs").
      header_mocs_target: label of the shared test-header mock library
        (e.g. ":test_header_mocs").
    """
    for base in MUT_DMA_TEST_SUITES:
        env = {}
        if base in _NEEDS_OFFSCREEN_QT_PLATFORM:
            env = {
                "QT_QPA_PLATFORM": "offscreen",
                "QT_DEBUG_PLUGINS": "1",
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
                "-Iserial_port",
                "-Iprotocol",
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
