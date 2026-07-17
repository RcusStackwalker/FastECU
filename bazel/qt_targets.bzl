"""Shared Qt Bazel macros and constants for FastECU targets."""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_qt//:qt.bzl", _qt_cc_binary = "qt_cc_binary", _qt_cc_library = "qt_cc_library", _qt_cc_test = "qt_cc_test", _qt_resource_via_qrc = "qt_resource_via_qrc")

qt_cc_binary = _qt_cc_binary
qt_cc_library = _qt_cc_library
qt_cc_test = _qt_cc_test
qt_resource_via_qrc = _qt_resource_via_qrc

QT_DEPS = [
    "@rules_qt//:qt_charts",
    "@rules_qt//:qt_core",
    "@rules_qt//:qt_gui",
    "@rules_qt//:qt_remote_objects",
    "@rules_qt//:qt_serial_port",
    "@rules_qt//:qt_test",
    "@rules_qt//:qt_web_sockets",
    "@rules_qt//:qt_widgets",
    "@rules_qt//:qt_xml",
]

COMMON_COPTS = [
    "-DQT_FORCE_ASSERTS",
    "-DQT_DEPRECATED_WARNINGS",
    "-I.",
    "-Iprotocol",
    "-Iserial_port",
] + select({
    "@platforms//os:macos": [
        "-mmacosx-version-min=10.15",
        "-Wno-error=implicit-function-declaration",
    ],
    "//conditions:default": [],
})

COMMON_LINKOPTS = select({
    "@platforms//os:macos": [
        "-mmacosx-version-min=10.15",
    ],
    "//conditions:default": [],
})

COMMON_INCLUDES = [
    "protocol",
    "serial_port",
]

def platform_srcs(unix = [], windows = []):
    return select({
        "@platforms//os:windows": windows,
        "//conditions:default": unix,
    })

def platform_hdrs(unix = [], windows = []):
    return platform_srcs(unix = unix, windows = windows)

def _basename(path):
    return path.split("/")[-1].rsplit(".", 1)[0]

def _gen_basename_ui_header(ctx):
    info = ctx.toolchains["@rules_qt//tools:toolchain_type"].qtinfo
    env = {"LD_LIBRARY_PATH": "/".join(info.uic_path.split("/")[:-1]) + "/../lib"}
    ctx.actions.run(
        inputs = [ctx.file.ui_file],
        outputs = [ctx.outputs.ui_header],
        arguments = [ctx.file.ui_file.path, "-o", ctx.outputs.ui_header.path],
        executable = info.uic_path,
        env = env,
        execution_requirements = {"local": "1"},
    )

_gen_basename_ui_header_rule = rule(
    implementation = _gen_basename_ui_header,
    attrs = {
        "ui_file": attr.label(allow_single_file = True, mandatory = True),
        "ui_header": attr.output(),
    },
    toolchains = ["@rules_qt//tools:toolchain_type"],
)

def qt_ui_basename_library(name, ui, deps):
    _gen_basename_ui_header_rule(
        name = name + "_uic",
        ui_file = ui,
        ui_header = "generated_ui/ui_%s.h" % _basename(ui),
        tags = ["local"],
    )
    cc_library(
        name = name,
        hdrs = [":" + name + "_uic"],
        includes = ["generated_ui"],
        deps = deps,
    )

def qt_ui_basename_libraries(name, forms, deps):
    """Create one UI header library per Qt Designer form.

    Args:
      name: Macro instance name used for validation.
      forms: Qt Designer .ui files to process.
      deps: Dependencies passed to each generated UI library.
    """
    if not name:
        fail("name must be non-empty")
    for ui in forms:
        qt_ui_basename_library(
            name = "ui_" + _basename(ui),
            ui = ui,
            deps = deps,
        )

def qt_cpp_moc_headers(name, srcs, deps = []):
    """Generate moc outputs for C++ headers and expose them as a library.

    Args:
      name: Name of the generated cc_library.
      srcs: Header files to pass to moc.
      deps: Dependencies passed to the generated cc_library.
    """
    outs = []
    for src in srcs:
        base = _basename(src)
        gen_name = "%s_%s_moc" % (name, base)
        out = "%s.moc" % base
        native.genrule(
            name = gen_name,
            srcs = [src],
            outs = [out],
            cmd = select({
                "@platforms//os:linux": "$(location @qt_linux_x86_64//:moc) $(location %s) -o $@" % src,
                "@platforms//os:windows": "$(location @qt_windows_x86_64//:moc) $(location %s) -o $@" % src,
                "@rules_qt//:osx_arm64": "$(location @qt_mac_aarch64//:moc) $(location %s) -o $@" % src,
            }),
            tools = select({
                "@platforms//os:linux": ["@qt_linux_x86_64//:moc"],
                "@platforms//os:windows": ["@qt_windows_x86_64//:moc"],
                "@rules_qt//:osx_arm64": ["@qt_mac_aarch64//:moc"],
            }),
            tags = ["local"],
        )
        outs.append(":" + gen_name)
    cc_library(
        name = name,
        hdrs = outs,
        includes = ["."],
        deps = deps,
    )

def local_test_srcs(paths):
    return [p.removeprefix("tests/") for p in paths if p.startswith("tests/") and p.endswith(".cpp")]

def local_test_hdrs(paths):
    return [p.removeprefix("tests/") for p in paths if p.startswith("tests/") and p.endswith((".h", ".hpp"))]

def test_cpp_moc_srcs(paths):
    return [p for p in local_test_srcs(paths) if p not in ["main.cpp", "serial_backend_main.cpp"]]
