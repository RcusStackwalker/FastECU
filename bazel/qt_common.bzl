"""Qt macros and dependency sets that carry no Qt Widgets.

Loadable from anywhere, including `src/backend` and `src/algorithms`. The
widget-layer half -- `QT_DEPS`, and `qt_cc_library`'s moc-the-headers path --
lives in qt_targets.bzl, which is visibility-restricted to the layers allowed
to build a user interface. See
docs/adr/0016-enforce-qt-reachability-by-visibility.md.
"""

load("@fastecu_qt//:qt.bzl", "gen_ui_header", _qt_cc_test = "qt_cc_test", _qt_cpp_moc_headers = "qt_cpp_moc_headers", _qt_resource_via_qrc = "qt_resource_via_qrc")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

qt_cc_test = _qt_cc_test
qt_cpp_moc_headers = _qt_cpp_moc_headers
qt_resource_via_qrc = _qt_resource_via_qrc

# qt_charts also pulls in qt_widgets transitively, so it stays out too.
QT_DEPS_NO_WIDGETS = [
    "//bazel/qt:core",
    "//bazel/qt:gui",
    "//bazel/qt:remote_objects",
    "//bazel/qt:serial_port",
    "//bazel/qt:web_sockets",
    "//bazel/qt:xml",
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

def _basename(path):
    return path.split("/")[-1].rsplit(".", 1)[0]

def qt_ui_basename_library(name, ui, deps):
    gen_ui_header(
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
