"""Repository extension for locating system OpenSSL installations."""

def _openssl_repo_impl(rctx):
    candidates = [
        rctx.os.environ.get("OPENSSL_ROOT"),
        "/opt/homebrew/opt/openssl@3",
        "/usr/local/opt/openssl@3",
        "C:/Program Files/OpenSSL-Win64",
    ]
    root = None
    for candidate in candidates:
        if candidate and rctx.path(candidate).exists:
            root = candidate
            break

    if root:
        rctx.symlink(rctx.path(root).get_child("include"), "include")
        rctx.symlink(rctx.path(root).get_child("lib"), "lib")
        if "windows" in rctx.os.name.lower():
            lib_candidates = [
                "lib/VC/x64/MD/libcrypto.lib",
                "lib/VC/x64/MT/libcrypto.lib",
                "lib/libcrypto.lib",
            ]
            crypto_lib = None
            for lib_candidate in lib_candidates:
                if rctx.path(root).get_child(lib_candidate).exists:
                    crypto_lib = "%s/%s" % (root, lib_candidate)
                    break
            if crypto_lib:
                linkopts = """[
        '\\"%s\\"',
    ]""" % crypto_lib
            else:
                linkopts = """[
        "libcrypto.lib",
    ]"""
        else:
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

def _openssl_impl(_mctx):
    _openssl_repo(name = "openssl_macos")
    _openssl_repo(name = "openssl_windows")

openssl = module_extension(
    implementation = _openssl_impl,
)
