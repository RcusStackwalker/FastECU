"""Exposes the headers of the LLVM that provides clang-tidy, for the tidy plugin."""

def _local_llvm_impl(repository_ctx):
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
    repository_ctx.file("BUILD.bazel", """load("@rules_cc//cc:defs.bzl", "cc_library")
load("{headers_rule}", "llvm_headers")

exports_files(["llvm-identity.json"])

llvm_headers(
    name = "clang_tidy_headers",
    hdrs = glob(
        ["include/clang/**", "include/clang-tidy/**", "include/llvm/**", "include/llvm-c/**"],
        allow_empty = True,
    ),
    visibility = ["//visibility:public"],
)

cc_library(
    name = "clang_tidy_libraries",
    srcs = glob(["lib/clang*.lib", "lib/LLVM*.lib"], exclude = ["lib/LLVM-C.lib"], allow_empty = True),
    visibility = ["//visibility:public"],
)
""".replace("{headers_rule}", str(repository_ctx.attr._headers_rule)))

local_llvm = repository_rule(
    implementation = _local_llvm_impl,
    attrs = {"_headers_rule": attr.label(default = Label("//bazel:llvm_headers.bzl"))},
    environ = ["FASTECU_LLVM_ROOT", "LLVM_CONFIG", "PATH"],
    local = True,
)
