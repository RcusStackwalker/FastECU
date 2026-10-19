"""LLVM development headers are external system headers, not first-party code."""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _llvm_headers_impl(ctx):
    return [CcInfo(compilation_context = cc_common.create_compilation_context(
        headers = depset(ctx.files.hdrs),
        system_includes = depset([ctx.label.workspace_root + "/include"]),
    ))]

llvm_headers = rule(
    implementation = _llvm_headers_impl,
    attrs = {"hdrs": attr.label_list(allow_files = True)},
)
