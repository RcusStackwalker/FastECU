"""Qt macros and dependency sets that carry no Qt Widgets.

Loadable from anywhere, including `src/backend` and `src/algorithms`. The
widget-layer half -- `QT_DEPS`, and `qt_cc_library`'s moc-the-headers path --
lives in qt_targets.bzl, which is visibility-restricted to the layers allowed
to build a user interface. See
docs/adr/0016-enforce-qt-widgets-reachability-by-visibility.md.
"""

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_qt//:qt.bzl", _qt_cc_test = "qt_cc_test", _qt_resource_via_qrc = "qt_resource_via_qrc")

qt_cc_test = _qt_cc_test
qt_resource_via_qrc = _qt_resource_via_qrc

# qt_charts also pulls in qt_widgets transitively, so it stays out too.
QT_DEPS_NO_WIDGETS = [
    "@rules_qt//:qt_core",
    "@rules_qt//:qt_gui",
    "@rules_qt//:qt_remote_objects",
    "@rules_qt//:qt_serial_port",
    "@rules_qt//:qt_test",
    "@rules_qt//:qt_web_sockets",
    "@rules_qt//:qt_xml",
]

COMMON_COPTS = [
    "-DQT_FORCE_ASSERTS",
    "-DQT_DEPRECATED_WARNINGS",
] + select({
    "@platforms//os:macos": [
        "-Wno-implicit-function-declaration",
    ],
    "//conditions:default": [],
})

def fastecu_qttest(
        name,
        src,
        deps = [],
        data = [],
        env = {},
        tags = [],
        target_compatible_with = [],
        copts = [],
        size = "small",
        qt_deps = QT_DEPS_NO_WIDGETS):
    """QtTest target with moc generation for a self-including C++ source.

    Args:
      name: Name of the test target.
      src: The self-including test source; moc runs over it for the Q_OBJECT
        fixture QtTest's slot mechanism requires.
      deps: Additional dependencies for the test.
      data: Runtime data for the test.
      env: Environment variables for the test.
      tags: Tags for the test.
      target_compatible_with: Platform constraints for the test.
      copts: Additional compiler options.
      size: Test size.
      qt_deps: Qt modules to compile and link against. Defaults to the
        widget-free set; the qt_targets.bzl wrapper raises it to QT_DEPS for
        the layers whose tests instantiate widgets.
    """
    moc_target = name + "_moc"
    qt_cpp_moc_headers(
        name = moc_target,
        srcs = [src],
        deps = qt_deps,
    )
    _qt_cc_test(
        name = name,
        srcs = [src],
        copts = COMMON_COPTS + copts,
        data = data,
        env = env,
        size = size,
        tags = tags,
        target_compatible_with = target_compatible_with,
        deps = qt_deps + [":" + moc_target] + deps,
    )

def _basename(path):
    return path.split("/")[-1].rsplit(".", 1)[0]

def _gen_basename_ui_header(ctx):
    info = ctx.toolchains["@rules_qt//tools:toolchain_type"].qtinfo
    ctx.actions.run(
        inputs = [ctx.file.ui_file],
        outputs = [ctx.outputs.ui_header],
        arguments = [ctx.file.ui_file.path, "-o", ctx.outputs.ui_header.path],
        executable = info.uic_path,
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
