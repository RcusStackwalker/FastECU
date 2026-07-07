def _openssl_repo_impl(rctx):
    candidates = [
        rctx.os.environ.get("OPENSSL_ROOT"),
        "/opt/homebrew/opt/openssl@3",
        "/usr/local/opt/openssl@3",
    ]
    root = None
    for candidate in candidates:
        if candidate and rctx.path(candidate).exists:
            root = candidate
            break

    if root:
        rctx.symlink(rctx.path(root).get_child("include"), "include")
        rctx.symlink(rctx.path(root).get_child("lib"), "lib")
        linkopts = """[
        "-L%s/lib",
        "-lcrypto",
    ]""" % root
    else:
        rctx.file("include/.keep", "")
        rctx.file("lib/.keep", "")
        linkopts = "[]"

    rctx.file(
        "BUILD.bazel",
        """cc_library(
    name = "openssl",
    hdrs = glob(["include/openssl/**/*.h"], allow_empty = True),
    includes = ["include"],
    linkopts = %s,
    visibility = ["//visibility:public"],
)
""" % linkopts,
    )

_openssl_repo = repository_rule(
    implementation = _openssl_repo_impl,
    environ = ["OPENSSL_ROOT"],
)

def _openssl_impl(mctx):
    _openssl_repo(name = "openssl_macos")

openssl = module_extension(
    implementation = _openssl_impl,
)
