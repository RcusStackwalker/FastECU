"""Repository extension for locating an external OpenSSL installation."""

_MACOS_ROOT_CANDIDATES = [
    "/opt/homebrew/opt/openssl@3",
    "/usr/local/opt/openssl@3",
]

_MACOS_LIB_CANDIDATES = [
    "lib/libcrypto.dylib",
    "lib/libcrypto.3.dylib",
]

_WINDOWS_ROOT_CANDIDATES = [
    "C:/Program Files/OpenSSL-Win64",
    "C:/Program Files/OpenSSL",
]

_WINDOWS_LIB_CANDIDATES = [
    "lib/VC/x64/MD/libcrypto.lib",
    "lib/VC/x64/MT/libcrypto.lib",
    "lib/libcrypto.lib",
]

_LINUX_LIB_CANDIDATES = [
    "lib/libcrypto.so",
    "lib64/libcrypto.so",
]

_BUILD_IMPORTED = """\
load("@rules_cc//cc:cc_import.bzl", "cc_import")
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_import(
    name = "crypto_import",
{import_attrs}
)

cc_library(
    name = "openssl",
    hdrs = glob(["include/openssl/**/*.h"], allow_empty = True),
    includes = ["include"],
    deps = [":crypto_import"],
    visibility = ["//visibility:public"],
)
"""

_BUILD_AMBIENT = """\
load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "openssl",
    linkopts = ["-lcrypto"],
    visibility = ["//visibility:public"],
)
"""

def _fail(rctx, detail):
    fail((
        "openssl_ext: {detail} (host: {os_name}). Set the OPENSSL_ROOT " +
        "environment variable to the OpenSSL install prefix (the " +
        "directory containing include/ and lib/) and retry."
    ).format(detail = detail, os_name = rctx.os.name))

def _first_existing(rctx, candidates):
    for candidate in candidates:
        if candidate and rctx.path(candidate).exists:
            return candidate
    return None

def _first_existing_child(rctx, root, relative_candidates):
    for relative in relative_candidates:
        if rctx.path(root).get_child(relative).exists:
            return relative
    return None

def _highest_soname_dll(rctx, root):
    bin_dir = rctx.path(root).get_child("bin")
    if not bin_dir.exists:
        return None
    best_name = None
    best_soname = -1
    for entry in bin_dir.readdir():
        name = entry.basename
        if not (name.startswith("libcrypto-") and name.endswith(".dll")):
            continue
        soname_segment = name.split("-")[1]
        if not soname_segment.isdigit():
            continue
        soname = int(soname_segment)
        if soname > best_soname:
            best_soname = soname
            best_name = name
    return best_name

def _resolve_root(rctx, env_root, fallback_candidates):
    candidates = [env_root] + fallback_candidates
    root = _first_existing(rctx, candidates)
    if not root:
        _fail(rctx, "could not find an OpenSSL install root; searched: {candidates}".format(
            candidates = ", ".join([c for c in candidates if c]),
        ))
    return root

def _symlink_lib(rctx, root, lib_candidates, link_name):
    lib = _first_existing_child(rctx, root, lib_candidates)
    if not lib:
        _fail(rctx, "found OpenSSL root '{root}' but none of its expected library files exist: {candidates}".format(
            root = root,
            candidates = ", ".join(lib_candidates),
        ))
    rctx.symlink(rctx.path(root).get_child(lib), link_name)

def _configure_macos(rctx, env_root):
    root = _resolve_root(rctx, env_root, _MACOS_ROOT_CANDIDATES)
    rctx.symlink(rctx.path(root).get_child("include"), "include")
    _symlink_lib(rctx, root, _MACOS_LIB_CANDIDATES, "lib/libcrypto.dylib")
    rctx.file("BUILD.bazel", _BUILD_IMPORTED.format(
        import_attrs = '    shared_library = "lib/libcrypto.dylib",',
    ))

def _configure_windows(rctx, env_root):
    root = _resolve_root(rctx, env_root, _WINDOWS_ROOT_CANDIDATES)
    rctx.symlink(rctx.path(root).get_child("include"), "include")
    _symlink_lib(rctx, root, _WINDOWS_LIB_CANDIDATES, "lib/libcrypto.lib")

    import_attrs = ['    interface_library = "lib/libcrypto.lib",']
    dll_name = _highest_soname_dll(rctx, root)
    if dll_name:
        rctx.symlink(rctx.path(root).get_child("bin/" + dll_name), "bin/libcrypto.dll")
        import_attrs.append('    shared_library = "bin/libcrypto.dll",')

    rctx.file("BUILD.bazel", _BUILD_IMPORTED.format(
        import_attrs = "\n".join(import_attrs),
    ))

def _configure_linux(rctx, env_root):
    if not env_root:
        rctx.file("BUILD.bazel", _BUILD_AMBIENT)
        return

    if not rctx.path(env_root).exists:
        _fail(rctx, "OPENSSL_ROOT='{root}' does not exist".format(root = env_root))

    rctx.symlink(rctx.path(env_root).get_child("include"), "include")
    _symlink_lib(rctx, env_root, _LINUX_LIB_CANDIDATES, "lib/libcrypto.so")
    rctx.file("BUILD.bazel", _BUILD_IMPORTED.format(
        import_attrs = '    shared_library = "lib/libcrypto.so",',
    ))

def _openssl_repo_impl(rctx):
    os_name = rctx.os.name.lower()
    env_root = rctx.os.environ.get("OPENSSL_ROOT")

    if "windows" in os_name:
        _configure_windows(rctx, env_root)
    elif "mac" in os_name:
        _configure_macos(rctx, env_root)
    else:
        _configure_linux(rctx, env_root)

_openssl_repo = repository_rule(
    implementation = _openssl_repo_impl,
    environ = ["OPENSSL_ROOT"],
)

def _openssl_impl(_mctx):
    _openssl_repo(name = "openssl")

openssl = module_extension(
    implementation = _openssl_impl,
)
