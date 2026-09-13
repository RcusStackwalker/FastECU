"""Qt macros and dependency sets reserved for the layers that may build a UI.

The `visibility()` call below is the enforcement: `src/backend` and
`src/algorithms` cannot load this file, so they cannot reach `QT_DEPS` or
`qt_cc_library`'s moc-the-headers path. Widget reachability is gated a second
time, for any route that bypasses this file, by the visibility of the
`//bazel/qt:widgets` alias `QT_DEPS` points at. Everything Qt-related that is
neither widget- nor moc-bearing lives in qt_common.bzl, which is loadable from
anywhere. See docs/adr/0016-enforce-qt-widgets-reachability-by-visibility.md.
"""

load("@rules_qt//:qt.bzl", _qt_cc_binary = "qt_cc_binary", _qt_cc_library = "qt_cc_library")
load(
    "//bazel:qt_common.bzl",
    _COMMON_COPTS = "COMMON_COPTS",
    _QT_DEPS_NO_WIDGETS = "QT_DEPS_NO_WIDGETS",
    _fastecu_qttest = "fastecu_qttest",
    _qt_cc_test = "qt_cc_test",
    _qt_cpp_moc_headers = "qt_cpp_moc_headers",
    _qt_resource_via_qrc = "qt_resource_via_qrc",
    _qt_ui_basename_libraries = "qt_ui_basename_libraries",
)

visibility([
    "//apps/...",
    "//src/platform/...",
    "//src/ui/...",
    "//tests/...",
])

QT_DEPS = _QT_DEPS_NO_WIDGETS + [
    "//bazel/qt:charts",
    "//bazel/qt:widgets",
]

qt_cc_binary = _qt_cc_binary
qt_cc_library = _qt_cc_library

# Re-exported so a widget-layer BUILD file still needs only one load statement.
COMMON_COPTS = _COMMON_COPTS
QT_DEPS_NO_WIDGETS = _QT_DEPS_NO_WIDGETS
qt_cc_test = _qt_cc_test
qt_cpp_moc_headers = _qt_cpp_moc_headers
qt_resource_via_qrc = _qt_resource_via_qrc
qt_ui_basename_libraries = _qt_ui_basename_libraries

def fastecu_qttest(name, src, qt_deps = QT_DEPS, **kwargs):
    """QtTest target that may link Qt Widgets.

    Args:
      name: Name of the test target.
      src: The self-including test source.
      qt_deps: Qt modules to compile and link against; widgets included by
        default, since this file is loadable only from the widget layer.
      **kwargs: Forwarded to qt_common.bzl's fastecu_qttest.
    """
    _fastecu_qttest(name = name, src = src, qt_deps = qt_deps, **kwargs)
