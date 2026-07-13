"""Repository rule that discovers the MSVC/Windows SDK sysroot (headers and
x86 import libraries) from a local Visual Studio Build Tools install, for
//bazel/toolchains/windows_x86_clang_cl to link against.

Microsoft doesn't allow redistributing the Windows SDK/MSVC CRT, so unlike
@llvm_windows this can't be a pinned, checksummed fetch -- it has to read
whatever VS Build Tools install is already on the machine, the same way
scripts/compile-x86-bridge-artifacts.ps1 discovers it via vcvarsall.bat x86.

On non-Windows hosts (this repo's macOS/Linux Bazel CI matrix, or a
developer's Mac) this produces a stub with empty path constants: nothing in
the default build graph references @msvc_sysroot -- only the windows_x86
toolchain does, and target_compatible_with gates every x86 consumer to
Windows -- so the stub is never actually used there. It exists purely so
`bazel query`/`bazel build` can resolve @msvc_sysroot's labels on those
hosts without erroring.
"""

_VSWHERE_CANDIDATES = [
    "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe",
]

_WINDOWS_KITS_ROOT_CANDIDATES = [
    "C:/Program Files (x86)/Windows Kits/10",
]

_STUB_DEFS = """\
MSVC_INCLUDE = ""
UCRT_INCLUDE = ""
UM_INCLUDE = ""
SHARED_INCLUDE = ""
MSVC_LIB_X86 = ""
UCRT_LIB_X86 = ""
UM_LIB_X86 = ""
"""

def _fail(detail):
    fail((
        "msvc_sysroot: {detail}. Install Visual Studio Build Tools with the " +
        "C++ x86/x64 build tools component (the same requirement " +
        "scripts/compile-x86-bridge-artifacts.ps1 has today)."
    ).format(detail = detail))

def _first_existing(rctx, candidates):
    for candidate in candidates:
        if candidate and rctx.path(candidate).exists:
            return candidate
    return None

def _latest_child_dirname(rctx, parent):
    parent_path = rctx.path(parent)
    if not parent_path.exists:
        return None
    names = sorted([e.basename for e in parent_path.readdir() if e.is_dir])
    return names[-1] if names else None

def _configure_windows(rctx):
    vswhere = _first_existing(rctx, _VSWHERE_CANDIDATES)
    if not vswhere:
        _fail("vswhere.exe not found; searched: {}".format(", ".join(_VSWHERE_CANDIDATES)))

    result = rctx.execute([
        vswhere,
        "-latest",
        "-products",
        "*",
        "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property",
        "installationPath",
    ])
    if result.return_code != 0 or not result.stdout.strip():
        _fail("vswhere.exe found no Visual Studio installation with the VC x86/x64 build tools component (stderr: {})".format(result.stderr))
    vs_install = result.stdout.strip().replace("\\", "/")

    msvc_version = _latest_child_dirname(rctx, vs_install + "/VC/Tools/MSVC")
    if not msvc_version:
        _fail("no MSVC tools version directories found under {}/VC/Tools/MSVC".format(vs_install))
    msvc_root = "{}/VC/Tools/MSVC/{}".format(vs_install, msvc_version)

    kits_root = _first_existing(rctx, _WINDOWS_KITS_ROOT_CANDIDATES)
    if not kits_root:
        _fail("Windows Kits root not found; searched: {}".format(", ".join(_WINDOWS_KITS_ROOT_CANDIDATES)))

    kits_version = _latest_child_dirname(rctx, kits_root + "/Include")
    if not kits_version:
        _fail("no Windows Kits version directories found under {}/Include".format(kits_root))

    rctx.file("defs.bzl", """\
MSVC_INCLUDE = "{msvc_include}"
UCRT_INCLUDE = "{ucrt_include}"
UM_INCLUDE = "{um_include}"
SHARED_INCLUDE = "{shared_include}"
MSVC_LIB_X86 = "{msvc_lib}"
UCRT_LIB_X86 = "{ucrt_lib}"
UM_LIB_X86 = "{um_lib}"
""".format(
        msvc_include = msvc_root + "/include",
        ucrt_include = "{}/Include/{}/ucrt".format(kits_root, kits_version),
        um_include = "{}/Include/{}/um".format(kits_root, kits_version),
        shared_include = "{}/Include/{}/shared".format(kits_root, kits_version),
        msvc_lib = msvc_root + "/lib/x86",
        ucrt_lib = "{}/Lib/{}/ucrt/x86".format(kits_root, kits_version),
        um_lib = "{}/Lib/{}/um/x86".format(kits_root, kits_version),
    ))

def _msvc_sysroot_impl(rctx):
    if rctx.os.name.lower().startswith("windows"):
        _configure_windows(rctx)
    else:
        rctx.file("defs.bzl", _STUB_DEFS)
    rctx.file("BUILD.bazel", "")

_msvc_sysroot_repo = repository_rule(
    implementation = _msvc_sysroot_impl,
    local = True,
)

def _msvc_sysroot_ext_impl(_mctx):
    _msvc_sysroot_repo(name = "msvc_sysroot")

msvc_sysroot = module_extension(
    implementation = _msvc_sysroot_ext_impl,
)
