"""LLVM development headers are external system headers, not first-party code."""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _llvm_headers_impl(ctx):
    windows_arguments = ctx.actions.declare_file(ctx.label.name + ".rsp")
    ctx.actions.write(windows_arguments, "\n".join([
        '/imsvc"' + ctx.label.workspace_root + "/" + directory + '"'
        for directory in ["include", "generated"]
    ]) + "\n")
    return [DefaultInfo(files = depset([windows_arguments])), CcInfo(compilation_context = cc_common.create_compilation_context(
        headers = depset(ctx.files.hdrs),
        system_includes = depset([
            ctx.label.workspace_root + "/include",
            ctx.label.workspace_root + "/generated",
        ]),
    ))]

llvm_headers = rule(
    implementation = _llvm_headers_impl,
    attrs = {"hdrs": attr.label_list(allow_files = True)},
)
