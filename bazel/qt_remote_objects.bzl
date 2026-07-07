def qt_replica_header(name, src, out = None, visibility = None):
    """Generate a Qt Remote Objects replica header from a .rep file."""
    if out == None:
        base = src.split("/")[-1].replace(".rep", "")
        out = "rep_%s_replica.h" % base
    native.genrule(
        name = name,
        srcs = [src],
        outs = [out],
        cmd = "$$(qmake6 -query QT_HOST_LIBEXECS)/repc -o replica $(location %s) $@" % src,
        visibility = visibility,
    )

def _moc_cmd(input_label, output_flag):
    return select({
        "@platforms//os:linux": "$(location @qt_linux_x86_64//:moc) %s %s" % (input_label, output_flag),
        "@platforms//os:windows": "$(location @qt_windows_x86_64//:moc) %s %s" % (input_label, output_flag),
        "@rules_qt//:osx_arm64": "$(location @qt_mac_aarch64//:moc) %s %s" % (input_label, output_flag),
    })

def _moc_tools():
    return select({
        "@platforms//os:linux": ["@qt_linux_x86_64//:moc"],
        "@platforms//os:windows": ["@qt_windows_x86_64//:moc"],
        "@rules_qt//:osx_arm64": ["@qt_mac_aarch64//:moc"],
    })

def qt_replica_library(name, reps, deps):
    hdrs = []
    moc_srcs = []
    for rep in reps:
        base = rep.split("/")[-1].replace(".rep", "")
        hdr = "rep_%s_replica.h" % base
        header_target = "%s_%s_header" % (name, base)
        moc_target = "%s_%s_moc" % (name, base)
        qt_replica_header(
            name = header_target,
            src = rep,
            out = hdr,
        )
        native.genrule(
            name = moc_target,
            srcs = [":" + header_target],
            outs = ["moc_rep_%s_replica.cpp" % base],
            cmd = _moc_cmd(
                "-DQCLASSINFO_REMOTEOBJECT_TYPE='\"RemoteObject Type\"' -DQCLASSINFO_REMOTEOBJECT_SIGNATURE='\"RemoteObject Signature\"' $(location :%s)" % header_target,
                "-o $@ -f'%s'" % hdr,
            ),
            tools = _moc_tools(),
        )
        hdrs.append(":" + header_target)
        moc_srcs.append(":" + moc_target)

    native.cc_library(
        name = name,
        srcs = moc_srcs,
        hdrs = hdrs,
        copts = select({
            "@platforms//os:macos": [
                "-mmacosx-version-min=10.15",
                "-Wno-error=implicit-function-declaration",
            ],
            "//conditions:default": [],
        }),
        deps = deps + select({
            "@platforms//os:windows": ["@qt_windows_x86_64//:qt_hdrs"],
            "//conditions:default": [],
        }),
    )
