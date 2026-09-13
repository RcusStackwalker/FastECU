"""Macros for generating Qt Remote Objects replica targets."""

load("@fastecu_qt//:qt.bzl", "moc_cmd", "moc_tools", "qt_hdrs_deps")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

def qt_replica_header(name, src, out = None, visibility = None):
    """Generate a Qt Remote Objects replica header from a .rep file.

    Args:
      name: Name of the generated genrule.
      src: Input .rep file.
      out: Output header path, or None to derive it from src.
      visibility: Visibility for the generated genrule.
    """
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

def qt_replica_library(name, reps, deps):
    """Create a cc_library for Qt Remote Objects replicas.

    Args:
      name: Name of the generated cc_library.
      reps: Input .rep files to compile.
      deps: Dependencies passed to the generated cc_library.
    """
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
            cmd = moc_cmd(
                "-DQCLASSINFO_REMOTEOBJECT_TYPE='\"RemoteObject Type\"' -DQCLASSINFO_REMOTEOBJECT_SIGNATURE='\"RemoteObject Signature\"' $(location :%s)" % header_target,
                "-o $@ -f'%s'" % hdr,
            ),
            tools = moc_tools(),
        )
        hdrs.append(":" + header_target)
        moc_srcs.append(":" + moc_target)
    cc_library(
        name = name,
        srcs = moc_srcs,
        hdrs = hdrs,
        copts = select({
            "@platforms//os:macos": [
                "-Wno-implicit-function-declaration",
            ],
            "//conditions:default": [],
        }),
        # Exposes this package's own directory (where the generated replica
        # headers land) as an include path, so consumers outside this
        # package can #include the bare replica header filename -- as
        # opposed to only the target's own srcs, which get same-package
        # quote-resolution for free. Needed once this macro is called from
        # a non-root package (root worked before only because "-I." happened
        # to cover it there).
        includes = ["."],
        deps = deps + qt_hdrs_deps(),
    )
