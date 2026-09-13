"""Qt macros for FastECU, owned here because label strings must resolve here.

A raw `"@repo//..."` string inside a Starlark macro is resolved against the
repo mapping of the *calling* package, not of the file the string is written
in. Since no FastECU package can name @rules_qt or the @qt_* platform repos
(see MODULE.bazel in this directory), rules_qt's own `qt_cc_library`,
`qt_cc_binary` and `qt_cc_test` cannot be called from them: their select keys,
`data` and `env` values are raw strings, 51 of them. They are reimplemented
below with `str(Label(...))`, which resolves at load time in this file and
yields a canonical label that resolves from anywhere.

Rules are not affected -- a `rule()`'s `toolchains` and a rule implementation's
labels resolve in the file that defines them -- so `qt_resource_via_qrc` is
still rules_qt's.

Kept deliberately close to the upstream shape so a rules_qt bump can be
diffed against it. Intel macOS is the one deliberate omission: this project
installs Qt for linux-x86_64, macOS-arm64 and windows-x86_64 only, so the
upstream `osx_x86_64` branches (which name a @qt_mac_x86_64 repo we never
fetch) are gone and that platform fails with "no matching condition".

See docs/adr/0016-enforce-qt-reachability-by-visibility.md.
"""

load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")
load("@rules_qt//:qt.bzl", _qt_resource_via_qrc = "qt_resource_via_qrc")

qt_resource_via_qrc = _qt_resource_via_qrc

_OSX_ARM64 = Label("@rules_qt//:osx_arm64")

_MOC_LINUX = str(Label("@qt_linux_x86_64//:moc"))
_MOC_MACOS = str(Label("@qt_mac_aarch64//:moc"))
_MOC_WINDOWS = str(Label("@qt_windows_x86_64//:moc"))

_QT_HDRS_WINDOWS = str(Label("@qt_windows_x86_64//:qt_hdrs"))

_PIC_COPTS = select({
    "@platforms//os:windows": [],
    "//conditions:default": ["-fPIC"],
})

def _label(repo, target):
    return str(Label("@%s//:%s" % (repo, target)))

qt_plugin_data = select({
    "@platforms//os:linux": [
        _label("qt_linux_x86_64", t)
        for t in ["qml", "plugins", "lib", "modules_files", "metatypes_files"]
    ],
    _OSX_ARM64: [
        _label("qt_mac_aarch64", t)
        for t in ["plugins", "qml", "lib", "modules_files", "metatypes_files"]
    ],
    "@platforms//os:windows": [
        _label("qt_windows_x86_64", t)
        for t in ["plugins", "qml", "plugin_files", "qml_files"]
    ],
})

_LINUX_ENV_DATA = {
    "QT_QPA_PLATFORM": "xcb",
    "QT_QPA_PLATFORM_PLUGIN_PATH": "$(location %s)/platforms" % _label("qt_linux_x86_64", "plugins"),
    "QML2_IMPORT_PATH": "$(location %s)" % _label("qt_linux_x86_64", "qml"),
    "QT_PLUGIN_PATH": "$(location %s)" % _label("qt_linux_x86_64", "plugins"),
}

_MACOS_ENV_DATA = {
    "QT_QPA_PLATFORM_PLUGIN_PATH": "$(location %s)/platforms" % _label("qt_mac_aarch64", "plugins"),
    "QML2_IMPORT_PATH": "$(location %s)" % _label("qt_mac_aarch64", "qml"),
    "QT_PLUGIN_PATH": "$(location %s)" % _label("qt_mac_aarch64", "plugins"),
}

_WINDOWS_ENV_DATA = {
    "QT_QPA_PLATFORM_PLUGIN_PATH": "$(location %s)/platforms" % _label("qt_windows_x86_64", "plugins"),
    "QML2_IMPORT_PATH": "$(location %s)" % _label("qt_windows_x86_64", "qml"),
    "QT_PLUGIN_PATH": "$(location %s)" % _label("qt_windows_x86_64", "plugins"),
}

def _env_select(env):
    return select({
        "@platforms//os:linux": _LINUX_ENV_DATA | env,
        _OSX_ARM64: _MACOS_ENV_DATA | env,
        "@platforms//os:windows": _WINDOWS_ENV_DATA | env,
    })

def moc_cmd(input_label, output_flag):
    """A genrule `cmd` that runs moc for the target platform.

    Args:
      input_label: The moc input, already expanded to a $(location ...).
      output_flag: Trailing moc arguments, typically "-o $@".
    """
    return select({
        "@platforms//os:linux": "$(location %s) %s %s" % (_MOC_LINUX, input_label, output_flag),
        "@platforms//os:windows": "$(location %s) %s %s" % (_MOC_WINDOWS, input_label, output_flag),
        _OSX_ARM64: "$(location %s) %s %s" % (_MOC_MACOS, input_label, output_flag),
    })

def moc_tools():
    """The `tools` list matching moc_cmd."""
    return select({
        "@platforms//os:linux": [_MOC_LINUX],
        "@platforms//os:windows": [_MOC_WINDOWS],
        _OSX_ARM64: [_MOC_MACOS],
    })

def qt_hdrs_deps():
    """Windows needs Qt's aggregate headers target on generated moc sources."""
    return select({
        "@platforms//os:windows": [_QT_HDRS_WINDOWS],
        "//conditions:default": [],
    })

def qt_cc_library(name, srcs, hdrs, normal_hdrs = [], deps = None, copts = [], target_compatible_with = [], **kwargs):
    """Compile a Qt library, running moc over `hdrs`.

    Args:
      name: A name for the rule.
      srcs: The cpp files to compile.
      hdrs: The header files moc compiles to sources.
      normal_hdrs: Headers which are not sources for generated code.
      deps: cc_library dependencies for the library.
      copts: cc_library copts.
      target_compatible_with: Platform constraints for the library.
      **kwargs: Any additional arguments are passed to the cc_library rule.
    """
    moc_srcs = []
    for hdr in hdrs:
        package = native.package_name()
        header_path = "%s/%s" % (package, hdr) if package else hdr
        moc_name = "moc_%s" % hdr.rsplit(".", 1)[0]
        native.genrule(
            name = moc_name,
            srcs = [hdr],
            outs = [moc_name + ".cpp"],
            cmd = moc_cmd("$(locations %s)" % hdr, "-o $@ -f'%s'" % header_path),
            tools = moc_tools(),
            target_compatible_with = target_compatible_with,
        )
        moc_srcs.append(":" + moc_name)
    cc_library(
        name = name,
        srcs = srcs + moc_srcs,
        hdrs = hdrs + normal_hdrs,
        textual_hdrs = moc_srcs,
        deps = deps,
        copts = copts + _PIC_COPTS,
        target_compatible_with = target_compatible_with,
        **kwargs
    )

def qt_cc_binary(name, srcs, deps = None, copts = [], data = [], env = {}, **kwargs):
    """A cc_binary carrying Qt's runtime plugin data and environment.

    Args:
      name: A name for the rule.
      srcs: The cpp files to compile.
      deps: cc_library dependencies for the binary.
      copts: cc_binary copts.
      data: Runtime data, added to Qt's own plugin data.
      env: Environment values, merged over Qt's own.
      **kwargs: Any additional arguments are passed to the cc_binary rule.
    """
    cc_binary(
        name = name,
        srcs = srcs,
        deps = deps,
        copts = copts + _PIC_COPTS,
        data = qt_plugin_data + data,
        env = _env_select(env),
        **kwargs
    )

def qt_cc_test(name, srcs, deps = None, copts = [], data = [], env = {}, **kwargs):
    """A cc_test carrying Qt's runtime plugin data and environment.

    Args:
      name: A name for the test.
      srcs: The cpp files to compile.
      deps: cc_library dependencies for the test.
      copts: cc_test copts.
      data: Runtime data, added to Qt's own plugin data.
      env: Environment values, merged over Qt's own.
      **kwargs: Any additional arguments are passed to the cc_test rule.
    """
    cc_test(
        name = name,
        srcs = srcs,
        deps = deps,
        copts = copts + _PIC_COPTS,
        data = qt_plugin_data + data,
        env = _env_select(env),
        **kwargs
    )

def _gen_ui_header(ctx):
    info = ctx.toolchains["@rules_qt//tools:toolchain_type"].qtinfo
    ctx.actions.run(
        inputs = [ctx.file.ui_file],
        outputs = [ctx.outputs.ui_header],
        arguments = [ctx.file.ui_file.path, "-o", ctx.outputs.ui_header.path],
        executable = info.uic_path,
        execution_requirements = {"local": "1"},
    )

gen_ui_header = rule(
    implementation = _gen_ui_header,
    attrs = {
        "ui_file": attr.label(allow_single_file = True, mandatory = True),
        "ui_header": attr.output(),
    },
    toolchains = ["@rules_qt//tools:toolchain_type"],
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
        base = src.split("/")[-1].rsplit(".", 1)[0]
        gen_name = "%s_%s_moc" % (name, base)
        native.genrule(
            name = gen_name,
            srcs = [src],
            outs = ["%s.moc" % base],
            cmd = moc_cmd("$(location %s)" % src, "-o $@"),
            tools = moc_tools(),
            tags = ["local"],
        )
        outs.append(":" + gen_name)
    cc_library(
        name = name,
        hdrs = outs,
        includes = ["."],
        deps = deps,
    )
