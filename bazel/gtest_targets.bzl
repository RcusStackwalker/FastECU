"""Shared GoogleTest target shapes for portable and Qt-linked FastECU code."""

load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("//bazel:qt_common.bzl", "COMMON_COPTS", "QT_DEPS_NO_WIDGETS", "qt_cc_test")

def _fastecu_portable_gtest_impl(
        name,
        visibility,
        srcs,
        deps,
        data,
        env,
        tags,
        target_compatible_with,
        copts,
        size):
    """GoogleTest target whose compile/link closure is deliberately Qt-free."""
    cc_test(
        name = name,
        visibility = visibility,
        srcs = srcs,
        copts = copts,
        data = data,
        env = env,
        size = size,
        tags = tags,
        target_compatible_with = target_compatible_with,
        deps = [
            "//src/algorithms/protocol/testing:byte_matchers",
            "@googletest//:gtest_main",
        ] + deps,
    )

fastecu_portable_gtest = macro(
    implementation = _fastecu_portable_gtest_impl,
    attrs = {
        "srcs": attr.label_list(mandatory = True, allow_files = True),
        "deps": attr.label_list(default = []),
        "data": attr.label_list(default = [], allow_files = True),
        "env": attr.string_dict(default = {}),
        "tags": attr.string_list(default = [], configurable = False),
        "target_compatible_with": attr.label_list(default = []),
        "copts": attr.string_list(default = []),
        "size": attr.string(default = "small", configurable = False),
    },
    doc = "GoogleTest target whose compile/link closure is deliberately Qt-free.",
)

def fastecu_gtest(
        name,
        srcs,
        deps = [],
        data = [],
        env = {},
        tags = [],
        target_compatible_with = [],
        copts = [],
        size = "small",
        qt_deps = QT_DEPS_NO_WIDGETS):
    # qt_cc_test (not bare cc_test) is required here: it wires up the
    # per-platform Qt plugin data + QT_PLUGIN_PATH/QT_QPA_PLATFORM_PLUGIN_PATH
    # env that widget-instantiating tests need to find "offscreen" (Linux),
    # "xcb"/"windows" runtime plugins under Bazel's test sandbox. A bare
    # cc_test only gets that on macOS (Qt frameworks resolve plugins via
    # rpath), which is why Linux/Windows widget tests failed here before.
    qt_cc_test(
        name = name,
        srcs = srcs,
        copts = COMMON_COPTS + copts,
        data = data,
        env = env,
        size = size,
        tags = tags,
        target_compatible_with = target_compatible_with,
        deps = qt_deps + [
            "//src/algorithms/protocol/testing:byte_matchers",
            "@googletest//:gtest_main",
        ] + deps,
    )
