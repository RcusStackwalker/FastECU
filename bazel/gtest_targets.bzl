"""GoogleTest targets that exercise FastECU's current Qt-linked core."""

load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "COMMON_LINKOPTS", "QT_DEPS")

def fastecu_gtest(
        name,
        srcs,
        deps = [],
        env = {},
        tags = [],
        target_compatible_with = []):
    cc_test(
        name = name,
        srcs = srcs,
        copts = COMMON_COPTS + [
            "-I.",
            "-Itests",
            "-Iserial_port",
            "-Iprotocol",
        ],
        linkopts = COMMON_LINKOPTS,
        data = select({
            "@platforms//os:macos": ["@qt_mac_aarch64//:lib"],
            "//conditions:default": [],
        }),
        env = env,
        tags = tags,
        target_compatible_with = target_compatible_with,
        deps = QT_DEPS + [
            "@googletest//:gtest_main",
            "//:fastecu_core_common",
        ] + deps + select({
            "@platforms//os:windows": ["//:fastecu_platform_windows"],
            "//conditions:default": ["//:fastecu_platform_unix"],
        }),
    )
