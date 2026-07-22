"""Shared GoogleTest target shapes for portable and Qt-linked FastECU code."""

load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "COMMON_LINKOPTS", "PORTABLE_COPTS", "QT_DEPS")

def fastecu_portable_gtest(
        name,
        srcs,
        deps = [],
        env = {},
        tags = [],
        target_compatible_with = []):
    """GoogleTest target whose compile/link closure is deliberately Qt-free."""
    cc_test(
        name = name,
        srcs = srcs,
        copts = PORTABLE_COPTS + [
            "-I.",
            "-Itests",
        ],
        env = env,
        tags = tags,
        target_compatible_with = target_compatible_with,
        deps = ["@googletest//:gtest_main"] + deps,
    )

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
            "-Isrc/platform/desktop/common/serial",
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
        ] + deps,
    )
