"""Module extension that fetches a pinned LLVM-for-Windows release archive.

Provides clang-cl.exe/lld-link.exe as @llvm_windows, used by
//bazel/toolchains/windows_x86_clang_cl to cross-compile the 32-bit x86
J2534 bridge helper. Pinned and checksummed so this doesn't drift with
whatever `choco install llvm` happens to resolve to on a given CI runner --
that choco-installed LLVM is a separate, unrelated install used only for
clang-tidy (see .github/workflows/pr.yml's "bazel" job).
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

_LLVM_VERSION = "22.1.8"
_LLVM_ARCHIVE = "clang+llvm-{version}-x86_64-pc-windows-msvc.tar.xz".format(version = _LLVM_VERSION)
_LLVM_URL = "https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/{archive}".format(
    version = _LLVM_VERSION,
    archive = _LLVM_ARCHIVE,
)

# Verified via `curl -sL <archive-url> | shasum -a 256` against the published
# release asset. Bumped from 19.1.7 to 22.1.8 because MSVC STL's compiler-version
# gate (yvals_core.h) started requiring Clang 20+ on Windows CI runners with a
# newer MSVC toolset; 22.1.8 is the latest stable LLVM release and comfortably
# clears that floor.
_LLVM_SHA256 = "d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234"

_BUILD_FILE_CONTENT = """\
load("@bazel_skylib//rules/directory:directory.bzl", "directory")
load("@bazel_skylib//rules/directory:subdirectory.bzl", "subdirectory")

package(default_visibility = ["//visibility:public"])

exports_files(glob(["bin/**"]))

filegroup(
    name = "builtin_headers",
    srcs = glob(["lib/clang/*/include/**"]),
)

directory(
    name = "clang_lib_root",
    srcs = glob(["lib/clang/**"]),
)

subdirectory(
    name = "builtin_headers_dir",
    parent = ":clang_lib_root",
    path = "lib/clang/{major_version}/include",
)
"""

def _llvm_windows_repo_impl(_mctx):
    http_archive(
        name = "llvm_windows",
        url = _LLVM_URL,
        sha256 = _LLVM_SHA256,
        strip_prefix = "clang+llvm-{version}-x86_64-pc-windows-msvc".format(version = _LLVM_VERSION),
        build_file_content = _BUILD_FILE_CONTENT.format(major_version = _LLVM_VERSION.split(".")[0]),
    )

llvm_windows = module_extension(
    implementation = _llvm_windows_repo_impl,
)
