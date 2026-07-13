"""Module extension that fetches a pinned LLVM-for-Windows release archive.

Provides clang-cl.exe/lld-link.exe as @llvm_windows, used by
//bazel/toolchains/windows_x86_clang_cl to cross-compile the 32-bit x86
J2534 bridge helper. Pinned and checksummed so this doesn't drift with
whatever `choco install llvm` happens to resolve to on a given CI runner --
that choco-installed LLVM is a separate, unrelated install used only for
clang-tidy (see .github/workflows/pr.yml's "bazel" job).
"""

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

_LLVM_VERSION = "19.1.7"
_LLVM_ARCHIVE = "clang+llvm-{version}-x86_64-pc-windows-msvc.tar.xz".format(version = _LLVM_VERSION)
_LLVM_URL = "https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/{archive}".format(
    version = _LLVM_VERSION,
    archive = _LLVM_ARCHIVE,
)

# Verified via `bazel build @llvm_windows//:bin/clang-cl.exe @llvm_windows//:bin/lld-link.exe`,
# which downloads the archive and reports the checksum it actually computed
# (see Task 1, Step 4 of the implementation plan this toolchain was built from).
_LLVM_SHA256 = "b4557b4f012161f56a2f5d9e877ab9635cafd7a08f7affe14829bd60c9d357f0"

_BUILD_FILE_CONTENT = """\
package(default_visibility = ["//visibility:public"])

exports_files(glob(["bin/**"]))
"""

def _llvm_windows_repo_impl(_mctx):
    http_archive(
        name = "llvm_windows",
        url = _LLVM_URL,
        sha256 = _LLVM_SHA256,
        strip_prefix = "clang+llvm-{version}-x86_64-pc-windows-msvc".format(version = _LLVM_VERSION),
        build_file_content = _BUILD_FILE_CONTENT,
    )

llvm_windows = module_extension(
    implementation = _llvm_windows_repo_impl,
)
