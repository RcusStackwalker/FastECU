"""Exposes the headers of the LLVM that provides clang-tidy, for the tidy plugin."""

def _local_llvm_impl(repository_ctx):
    llvm_config = repository_ctx.getenv("LLVM_CONFIG")
    if not llvm_config:
        found = repository_ctx.which("llvm-config")
        llvm_config = str(found) if found else "/opt/homebrew/opt/llvm/bin/llvm-config"
    result = repository_ctx.execute([llvm_config, "--includedir"])
    if result.return_code:
        # Machines without LLVM still load the workspace; only the manual
        # plugin targets are unusable.
        repository_ctx.file("BUILD.bazel", "# llvm-config unavailable: " + result.stderr.replace("\n", " ") + "\n")
        return
    repository_ctx.symlink(result.stdout.strip(), "include")
    repository_ctx.file("BUILD.bazel", """load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "clang_tidy_headers",
    hdrs = glob(
        ["include/clang/**", "include/clang-tidy/**", "include/llvm/**", "include/llvm-c/**"],
        allow_empty = True,
    ),
    includes = ["include"],
    visibility = ["//visibility:public"],
)
""")

local_llvm = repository_rule(
    implementation = _local_llvm_impl,
    environ = ["LLVM_CONFIG", "PATH"],
    local = True,
)
