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

# Each suite depends only on the packages it includes. Derived from the
# #include graph; see the step 3 design doc. A suite needing more than
# its obvious package is step 4/5 scope surfacing early -- record it in
# the PR description rather than widening quietly.
SUITE_DEPS = {
    "test_bytes": [
        "//src/algorithms/protocol",
        "//src/algorithms/protocol:qt_compat",
    ],
    "test_codec": ["//src/algorithms/protocol/mut_dma"],
    "test_freeform": ["//src/algorithms/protocol/mut_dma"],
    "test_memory": ["//src/algorithms/protocol/mut_dma"],
    # test_init.cpp includes imut_dma_init.h directly; scripted_kline_transport.h
    # (compiled into the same TU) includes ikline_transport.h -- both live in
    # backend/protocol. No mut_dma header is directly included; that package
    # is a phantom edge here and reaches the test only transitively (via
    # backend/protocol's own dep on it), so it is not declared.
    "test_init": ["//src/backend/protocol"],
    # test_driver.cpp includes mut_dma_codec/freeform/memory.h (mut_dma) AND
    # mut_dma_driver.h (backend/protocol) directly -- both are real edges. It also
    # includes qt_mut_dma.h to bridge std::vector<Channel> locals into the
    # still-QVector-based MutDmaDriver API, so it needs the mut_dma Qt shim
    # (which transitively provides :mut_dma's portable headers too).
    "test_driver": [
        "//src/algorithms/protocol/mut_dma:qt_compat",
        "//src/backend/protocol",
    ],
    # test_transport.cpp includes qt_bytes.h (algorithms/protocol root) only
    # directly; scripted_kline_transport.h pulls in backend/protocol's
    # ikline_transport.h, which itself depends on algorithms/protocol root,
    # so backend/protocol alone covers both real edges.
    "test_transport": ["//src/backend/protocol"],
    # These three include qt_colt.h (the Qt shim) alongside the portable
    # header, since the tests exercise both the bytes::-native functions and
    # the QByteArray/QVector overloads (Task 8).
    "test_mitsu_colt_can_protocol": ["//src/algorithms/protocol/colt:qt_compat"],
    "test_mitsu_colt_can_vendor_ext_protocol": ["//src/algorithms/protocol/colt:qt_compat"],
    "test_mitsu_colt_can_cdbg_protocol": ["//src/algorithms/protocol/colt:qt_compat"],
    # test_cdbg_driver.cpp includes mitsu_colt_can_cdbg_driver.h, which lives
    # in backend/protocol, NOT algorithms/protocol/colt (a different file:
    # mitsu_colt_can_cdbg_protocol.h). It also includes qt_colt.h directly
    # (Task 8) to bridge QVector<CdbgChannel> locals into the still-QVector-
    # based CdbgLogDriver API, so it needs the colt Qt shim as a real edge.
    "test_cdbg_driver": [
        "//src/algorithms/protocol/colt:qt_compat",
        "//src/backend/protocol",
    ],
    "test_ssm_protocol": ["//src/algorithms/protocol/ssm:qt_compat"],
    "test_expression_evaluator": ["//src/algorithms/expression:qt_compat"],
    "test_menu_command": ["//src/algorithms/menu:qt_compat"],
    "test_diagnostic_parsers": ["//src/algorithms/diagnostics:qt_compat"],
    # test_checksum_results.cpp was retargeted (Task 10) to the Qt shim
    # (qt_checksum.h / QtChecksumResult) so it can keep asserting the frozen
    # QByteArray/QString contract now that ChecksumResult itself is portable.
    "test_checksum_results": ["//src/algorithms/checksum:qt_compat"],
    "test_logging_worker": ["//src/backend/logging"],
    "test_logging_engine": ["//src/backend/logging"],
    "test_romraider_conversion": ["//src/backend/logging"],
    # These three include a src/backend/logging/protocols/*.h header directly
    # (a subpackage of backend/logging, not the same label) -- backend/logging
    # alone doesn't expose it.
    "test_ssm_logging_protocol": ["//src/backend/logging/protocols"],
    "test_mut_dma_logging_protocol": ["//src/backend/logging/protocols"],
    "test_cdbg_logging_protocol": ["//src/backend/logging/protocols"],
    "test_flash_operation_worker": ["//src/backend/flash"],
    # test_flash_ecu_mitsu_m32r_can_operation.cpp includes qt_colt.h directly
    # (Task 8) for QByteArray-typed Colt helper wrappers, so it needs the
    # colt Qt shim as a real edge in addition to backend/flash/ecu.
    "test_flash_ecu_mitsu_m32r_can_operation": [
        "//src/algorithms/protocol/colt:qt_compat",
        "//src/backend/flash/ecu",
    ],
    "test_flash_utils": [
        "//src/backend/flash",
        # test_flash_utils.cpp includes serial_port_actions.h directly;
        # this package is on the serial_qt_compat allowlist in Task 8.
        "//src/platform/desktop/common/serial:serial_qt_compat",
    ],
    "test_ecuflash_definition_parsing": ["//src/backend/definitions"],
    "test_file_actions_parsing": ["//src/backend/definitions"],
    "test_rom_transformations": ["//src/backend/definitions"],
    "test_model_validation": ["//src/backend/definitions"],
}

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
                deps = SUITE_DEPS[base],
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
            ] + SUITE_DEPS[base],
        )
