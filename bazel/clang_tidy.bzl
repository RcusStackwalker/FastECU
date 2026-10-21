"""Expose real transitive C++ headers to clang-tidy fixtures."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _clang_tidy_headers_impl(ctx):
    return [DefaultInfo(files = depset(transitive = [
        dep[CcInfo].compilation_context.headers
        for dep in ctx.attr.deps
    ]))]

clang_tidy_headers = rule(
    implementation = _clang_tidy_headers_impl,
    attrs = {"deps": attr.label_list(providers = [CcInfo])},
    doc = "Expose real transitive C++ headers as lit runfiles without compiling them.",
)
