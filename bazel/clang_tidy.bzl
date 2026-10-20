"""The clang-tidy runner targets behind //:clang_tidy_*."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("@rules_python//python:defs.bzl", "py_binary")

def _clang_tidy_runner_impl(name, visibility, mode, changed):
    """A runnable clang-tidy pass over the whole tree or only changed files.

    Hedron's refresh is aquery-based and never executes compile actions, so
    build-generated header trees (e.g. rules_qt's per-framework
    _virtual_includes symlink farms) can be absent from disk even though the
    emitted compile flags reference them; a real build first materializes
    them. The refresh targets are tagged manual, so the wildcard prebuild
    cannot include them recursively. --build-arg and --compdb-arg both pin
    --config=release so the prebuild and Hedron's own aquery resolve the same
    bazel-out/<config> directory (mismatched modes would point
    compile_commands.json at headers the prebuild never wrote) and so this
    reuses the same build the "Build Bazel targets" CI step already did,
    instead of a second fastbuild-mode rebuild of everything -- fastbuild's
    Windows MSVC toolchain emits a now-deprecated /DEBUG:FASTLINK link flag
    that release/opt mode doesn't.

    Args:
      name: Name of the target.
      visibility: Caller visibility forwarded to the runner target.
      mode: "report" to list findings, "fix" to apply available fixes, or
        "benchmark" to measure the local inline-pass cost.
      changed: Restrict the run to files changed against the merge base.
    """
    py_binary(
        name = name,
        visibility = visibility,
        srcs = [
            "//tools/clang-tidy/scripts:clang_tidy_benchmark.py",
            "//tools/clang-tidy/scripts:clang_tidy_output.py",
            "//tools/clang-tidy/scripts:clang_tidy_profile.py",
            "//tools/clang-tidy/scripts:clang_tidy_exceptions.py",
            "//tools/clang-tidy/scripts:clang_tidy_runner.py",
            "//tools/clang-tidy/scripts:clang_tidy_scope.py",
            "//tools/clang-tidy/scripts:clang_tidy_toolchain.py",
        ],
        imports = ["tools/clang-tidy/scripts"],
        args = ([] if mode == "benchmark" else [mode]) + (["--changed"] if changed else []) + [
            "--compdb-tool",
            "$(location //bazel/compile_commands:refresh)",
            "--build-arg=--config=release",
            "--build-arg=//...",
            "--compdb-arg=--config=release",
        ],
        data = [
            "//:.clang-tidy",
            "//:.clang-tidy-exceptions.yaml",
            "//bazel/compile_commands:refresh",
            "//tools/clang-tidy/plugin:llvm_identity",
        ] + select({
            "@platforms//os:windows": ["//tools/clang-tidy/plugin:fastecu_clang_tidy"],
            "//conditions:default": ["//tools/clang-tidy/plugin:fastecu_tidy"],
        }),
        tags = ["manual"],
        main = "//tools/clang-tidy/scripts:clang_tidy_benchmark.py" if mode == "benchmark" else "//tools/clang-tidy/scripts:clang_tidy_runner.py",
        deps = ["@python_deps//pyyaml"],
    )

clang_tidy_runner = macro(
    implementation = _clang_tidy_runner_impl,
    attrs = {
        "mode": attr.string(mandatory = True, values = ["report", "fix", "benchmark"], configurable = False),
        "changed": attr.bool(default = False, configurable = False),
    },
    doc = "A clang-tidy runner for report or fix mode, optionally restricted to changed files.",
)

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
