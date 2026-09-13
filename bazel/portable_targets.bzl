"""The portable-target registry: the single source //:portable_closure reads.

A package maps to the portable targets it must contain. An empty list means
"every cc_library here is portable" -- the whole package is scanned, and no
individual target is required. A non-empty list both requires those targets to
exist and limits the scan to them, because the package also holds targets that
are deliberately not portable.

Three things are derived from this file, so a portable target is registered
once rather than three times: the genquery closure roots and the `data` list on
//:portable_closure in BUILD.bazel, and the JSON registry that
scripts/check-portable-closure.py reads in place of a hardcoded copy.
"""

PORTABLE_PACKAGES = {
    "src/algorithms/checksum": [],
    "src/algorithms/diagnostics": [],
    "src/algorithms/expression": [],
    "src/algorithms/menu": [],
    "src/algorithms/protocol": [],
    "src/algorithms/protocol/colt": [],
    "src/algorithms/protocol/mut_dma": [],
    "src/algorithms/protocol/ssm": [],
    "src/algorithms/protocol/testing": [],
    "src/algorithms/protocol/uds": [],
    "src/backend/calibration": [
        "calibration_service",
        "map_edit",
    ],
    "src/backend/checksum": [
        "checksum_selection",
        "dispatch",
    ],
    "src/backend/config": [
        "app_config",
        "car_model_catalog",
        "config_paths",
        "menu_definition",
        "protocol_catalog",
        "provisioning",
    ],
    "src/backend/definition": [
        "definition_model",
        "definition_resolver",
        "definition_service",
        "definition_writer",
        "ecuflash_parser",
        "parser_utils",
        "romraider_parser",
        "text_format",
    ],
    "src/backend/flash": [
        "can_flash_uds_channel",
        "flash_device_lookup",
        "flash_executor",
        "flash_plan",
        "flash_types",
        "flash_validation",
    ],
    "src/backend/flash/ecu": [
        "denso_iso15765_can_common",
        "mitsu_colt_m32r_can_executor",
        "mitsu_colt_m32r_can_plan",
        "mitsu_colt_m32r_can_types",
        "single_window_plan",
        "subaru_denso_1n83m_1_5m_can_executor",
        "subaru_denso_1n83m_1_5m_can_plan",
        "subaru_denso_1n83m_1_5m_can_types",
        "subaru_denso_1n83m_4m_can_executor",
        "subaru_denso_1n83m_4m_can_plan",
        "subaru_denso_1n83m_4m_can_types",
        "subaru_denso_mc68hc16y5_02_executor",
        "subaru_denso_mc68hc16y5_02_plan",
        "subaru_denso_mc68hc16y5_02_types",
        "subaru_denso_sh7055_02_executor",
        "subaru_denso_sh7055_02_plan",
        "subaru_denso_sh7055_02_types",
        "subaru_denso_sh72531_can_executor",
        "subaru_denso_sh72531_can_plan",
        "subaru_denso_sh72531_can_types",
        "subaru_denso_sh72543_can_diesel_executor",
        "subaru_denso_sh72543_can_diesel_plan",
        "subaru_denso_sh72543_can_diesel_types",
        "subaru_hitachi_m32r_can_executor",
        "subaru_hitachi_m32r_can_plan",
        "subaru_hitachi_m32r_can_types",
        "subaru_hitachi_m32r_kline_executor",
        "subaru_hitachi_m32r_kline_plan",
        "subaru_hitachi_m32r_kline_types",
        "subaru_mitsu_m32r_kline_executor",
        "subaru_mitsu_m32r_kline_plan",
        "subaru_mitsu_m32r_kline_types",
        "subaru_tcu_cvt_hitachi_m32r_can_executor",
        "subaru_tcu_cvt_hitachi_m32r_can_plan",
        "subaru_tcu_cvt_hitachi_m32r_can_types",
        "subaru_tcu_cvt_mitsu_mh8104_can_executor",
        "subaru_tcu_cvt_mitsu_mh8104_can_plan",
        "subaru_tcu_cvt_mitsu_mh8104_can_types",
        "subaru_tcu_cvt_mitsu_mh8111_can_executor",
        "subaru_tcu_cvt_mitsu_mh8111_can_plan",
        "subaru_tcu_cvt_mitsu_mh8111_can_types",
    ],
    "src/backend/flash/eeprom": [],
    "src/backend/logging": [
        "logger_conf",
        "logger_definition_model",
        "logger_definition_parser",
        "logger_definition_service",
        "logging_conversion",
        "logging_session",
        "logging_types",
        "logging_use_case",
    ],
    "src/backend/logging/protocols": [
        "protocols",
    ],
    "src/backend/ports": [
        "ports",
    ],
    "src/backend/protocol": [
        "protocol",
    ],
    "src/backend/protocol/uds": [
        "uds_client",
    ],
    "src/backend/service_functions": [
        "read_parameters_session",
        "relearn_session",
        "service_function_session",
        "service_function_types",
        "set_parameters_session",
        "tcu_parameter_table",
    ],
}

# Swept for platform labels with the registered targets, but not themselves
# required to exist: test targets and the shared resource files they read.
CLOSURE_EXTRA_ROOTS = [
    "//resources/shared:test_protocols_cfg_eeprom_capabilities",
    "//resources/shared:test_protocols_cfg_subaru_hitachi_m32r_kline",
    "//resources/shared:test_protocols_cfg_subaru_mitsu_m32r_kline",
    "//src/algorithms/protocol/mut_dma:test_codec",
    "//src/algorithms/protocol/mut_dma:test_freeform",
    "//src/algorithms/protocol/mut_dma:test_memory",
    "//src/backend/flash/eeprom:denso_sh705x_eeprom_can",
    "//src/backend/flash/eeprom:denso_sh705x_eeprom_common",
    "//src/backend/flash/eeprom:denso_sh705x_eeprom_kline",
    "//src/backend/flash/eeprom:eeprom_read_plan",
    "//src/backend/flash/eeprom:test_eeprom_read_plan_goldens",
    "//src/backend/logging/protocols:test_cdbg_logging_protocol",
    "//src/backend/logging/protocols:test_mut_dma_logging_protocol",
    "//src/backend/logging/protocols:test_ssm_logging_protocol",
    "//src/backend/protocol:test_cdbg_driver",
    "//src/backend/protocol:test_driver",
    "//src/backend/protocol:test_init",
    "//src/backend/protocol:test_transport",
]

# Roots of the transitive-closure query. Every registered target plus the
# extras above; the query rejects any //src/platform label reachable from one.
CLOSURE_ROOTS = sorted([
    "//" + package + ":" + name
    for package, names in PORTABLE_PACKAGES.items()
    for name in names
] + CLOSURE_EXTRA_ROOTS)

# The BUILD files the scan reads, declared so the test reruns when one changes
# and so it sees the same set on Windows, where runfiles are copies.
PORTABLE_BUILD_FILES = ["//" + package + ":BUILD.bazel" for package in sorted(PORTABLE_PACKAGES)]
