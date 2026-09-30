"""Qt macros and dependency sets reserved for the layers that may build a UI.

The `visibility()` call below is the enforcement: `src/backend` and
`src/algorithms` cannot load this file, so they cannot reach `QT_DEPS` or
`qt_cc_library`'s moc-the-headers path. Widget reachability is gated a second
time, for any route that bypasses this file, by the visibility of the
`//bazel/qt:widgets` alias `QT_DEPS` points at. Everything Qt-related that is
neither widget- nor moc-bearing lives in qt_common.bzl, which is loadable from
anywhere. See docs/adr/0016-enforce-qt-reachability-by-visibility.md.
"""

load("@fastecu_qt//:qt.bzl", _qt_cc_binary = "qt_cc_binary", _qt_cc_library = "qt_cc_library")
load(
    "//bazel:qt_common.bzl",
    _COMMON_COPTS = "COMMON_COPTS",
    _QT_DEPS_NO_WIDGETS = "QT_DEPS_NO_WIDGETS",
    _qt_cc_test = "qt_cc_test",
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

def qt_cc_library(name, srcs, hdrs = [], copts = [], **kwargs):
    """A Qt library, running moc over `hdrs`, built with COMMON_COPTS.

    Args:
      name: A name for the rule.
      srcs: The cpp files to compile.
      hdrs: The header files moc compiles to sources. Empty for a library whose
        public headers are all `normal_hdrs`.
      copts: Compiler options added after COMMON_COPTS.
      **kwargs: Forwarded to qt.bzl's qt_cc_library (`normal_hdrs`, `deps`, ...).
    """
    _qt_cc_library(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        copts = COMMON_COPTS + copts,
        **kwargs
    )

def qt_cc_binary(name, srcs, copts = [], **kwargs):
    """A Qt binary carrying the plugin runtime data, built with COMMON_COPTS.

    Args:
      name: A name for the rule.
      srcs: The cpp files to compile.
      copts: Compiler options added after COMMON_COPTS.
      **kwargs: Forwarded to qt.bzl's qt_cc_binary (`deps`, `data`, `env`, ...).
    """
    _qt_cc_binary(
        name = name,
        srcs = srcs,
        copts = COMMON_COPTS + copts,
        **kwargs
    )

# Re-exported so a widget-layer BUILD file still needs only one load statement.
COMMON_COPTS = _COMMON_COPTS
QT_DEPS_NO_WIDGETS = _QT_DEPS_NO_WIDGETS
qt_cc_test = _qt_cc_test
qt_resource_via_qrc = _qt_resource_via_qrc
qt_ui_basename_libraries = _qt_ui_basename_libraries
