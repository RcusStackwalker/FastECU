"""Exposes the headers of the LLVM that provides clang-tidy, for the tidy plugin."""

def _write_platform_args(repository_ctx):
    """Writes defs.bzl with the extra clang-tidy arguments this host needs.

    The Bazel C++ toolchain on macOS finds the SDK through xcrun at compile time, so
    flags handed to a standalone clang-tidy would not name it; the LLVM libc++ headers
    then clash with the SDK's.
    """
    args = []
    if repository_ctx.os.name.startswith("mac"):
        sdk = repository_ctx.execute(["xcrun", "--show-sdk-path"])
        if not sdk.return_code and sdk.stdout.strip():
            args = ["-extra-arg-before=-isysroot", "-extra-arg-before=" + sdk.stdout.strip()]
    repository_ctx.file("defs.bzl", "CLANG_TIDY_PLATFORM_ARGS = " + json.encode(args) + "\n")

def _local_llvm_impl(repository_ctx):
    _write_platform_args(repository_ctx)
    selected_root = repository_ctx.getenv("FASTECU_LLVM_ROOT")
    llvm_config = repository_ctx.getenv("LLVM_CONFIG")
    if selected_root:
        llvm_config = selected_root + "/bin/llvm-config" + (".exe" if repository_ctx.os.name.startswith("windows") else "")
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
    prefix = repository_ctx.execute([llvm_config, "--prefix"])
    version = repository_ctx.execute([llvm_config, "--version"])
    root = str(repository_ctx.path(prefix.stdout.strip()).realpath)
    repository_ctx.symlink(root + "/lib", "lib")
    repository_ctx.symlink(root + "/bin", "bin")
    identity = {
        "root": root,
        "version": version.stdout.strip(),
    }
    host = root + "/bin/clang-tidy" + (".exe" if repository_ctx.os.name.startswith("windows") else "")
    repository_ctx.watch(host)
    repository_ctx.watch(llvm_config)
    python = repository_ctx.which("python3") or repository_ctx.which("python")
    if not python:
        fail("Python is required to fingerprint the selected LLVM installation")
    digest = repository_ctx.execute([python, "-c", "import hashlib,pathlib,sys; print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())", host])
    if digest.return_code:
        fail("Cannot fingerprint selected LLVM: " + digest.stderr)
    identity["clang_tidy_sha256"] = digest.stdout.strip()
    repository_ctx.file("llvm-identity.json", json.encode(identity))

    # Official binary distributions omit this generated header, although the
    # installed force-linker includes it. Infer only the two switches consumed
    # by that header from the optional module libraries actually supplied.
    features = {
        "CLANG_TIDY_ENABLE_QUERY_BASED_CUSTOM_CHECKS": "Custom",
        "CLANG_TIDY_ENABLE_STATIC_ANALYZER": "MPI",
    }
    configuration = "// Feature availability in the selected LLVM archive.\n"
    for define, module in features.items():
        windows_library = repository_ctx.path(root + "/lib/clangTidy" + module + "Module.lib")
        unix_library = repository_ctx.path(root + "/lib/libclangTidy" + module + "Module.a")
        enabled = windows_library.exists or unix_library.exists
        configuration += "#define " + define + " " + ("1" if enabled else "0") + "\n"
    repository_ctx.file("generated/clang-tidy-config.h", configuration)
    repository_ctx.file("BUILD.bazel", """load("@rules_cc//cc:defs.bzl", "cc_library")
load("{headers_rule}", "llvm_headers")

exports_files(["defs.bzl", "llvm-identity.json", "lib/libLTO.dylib", "bin/clang-tidy"])

llvm_headers(
    name = "clang_tidy_headers",
    hdrs = ["generated/clang-tidy-config.h"] + glob(
        ["include/clang/**", "include/clang-tidy/**", "include/llvm/**", "include/llvm-c/**"],
        allow_empty = True,
    ),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "filecheck_libraries",
    srcs = {filecheck_libraries},
    visibility = ["//visibility:public"],
)

cc_library(
    name = "clang_tidy_libraries",
    srcs = glob(["lib/clang*.lib", "lib/LLVM*.lib"], exclude = ["lib/LLVM-C.lib"], allow_empty = True),
    visibility = ["//visibility:public"],
)
""".replace("{headers_rule}", str(repository_ctx.attr._headers_rule)).replace(
        "{filecheck_libraries}",
        json.encode([
            "lib/LLVM" + name + ".lib" if repository_ctx.os.name.startswith("windows") else "lib/libLLVM" + name + ".a"
            for name in ["FileCheck", "Support", "Demangle"]
        ]),
    ))

local_llvm = repository_rule(
    implementation = _local_llvm_impl,
    attrs = {"_headers_rule": attr.label(default = Label("//bazel:llvm_headers.bzl"))},
    environ = ["FASTECU_LLVM_ROOT", "LLVM_CONFIG", "PATH"],
    local = True,
)
