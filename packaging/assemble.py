"""Assemble a release archive from a built FastECU binary and Qt's deploy tool.

Run by the `qt_deploy_zip` rule (third_party/qt/qt.bzl), not by hand. The deploy
tools mutate the tree they are pointed at, so everything is staged in a scratch
directory and only the finished zip is written to Bazel's output.
"""

import argparse
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile

# Fixed so the same inputs give the same bytes (ZIP cannot store earlier).
_ZIP_EPOCH = (1980, 1, 1, 0, 0, 0)

_INFO_PLIST = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>FastECU</string>
    <key>CFBundleIdentifier</key><string>fi.fastecu.FastECU</string>
    <key>CFBundleName</key><string>FastECU</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>{version}</string>
    <key>CFBundleVersion</key><string>{version}</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
"""


def zip_tree(root, out_path):
    """Zip `root`'s contents, keeping symlinks (macOS frameworks need them)."""
    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(dirnames + filenames):
            entries.append(os.path.join(dirpath, name))
    entries = sorted(set(entries))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in entries:
            arcname = os.path.relpath(path, root).replace(os.sep, "/")
            st = os.lstat(path)
            info = zipfile.ZipInfo(arcname, _ZIP_EPOCH)
            info.create_system = 3  # Unix: lets unzip restore modes and links
            if stat.S_ISLNK(st.st_mode):
                info.external_attr = (stat.S_IFLNK | 0o777) << 16
                zf.writestr(info, os.readlink(path))
            elif stat.S_ISDIR(st.st_mode):
                info.filename += "/"
                info.external_attr = ((stat.S_IFDIR | 0o755) << 16) | 0x10
                zf.writestr(info, b"")
            else:
                mode = 0o755 if st.st_mode & stat.S_IXUSR else 0o644
                info.external_attr = (stat.S_IFREG | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with open(path, "rb") as f:
                    zf.writestr(info, f.read())


def _copy_writable(src, dst, executable=False):
    shutil.copyfile(src, dst)
    # Bazel outputs are read-only; the deploy tools strip and relink in place.
    os.chmod(dst, 0o755 if executable else 0o644)


def _run_deploy_tool(tool, target):
    subprocess.run([os.path.abspath(tool), target], check=True)


def assemble_macos(args, stage):
    app = os.path.join(stage, "FastECU.app")
    macos_dir = os.path.join(app, "Contents", "MacOS")
    os.makedirs(macos_dir)
    os.makedirs(os.path.join(app, "Contents", "Resources"))
    exe = os.path.join(macos_dir, "FastECU")
    _copy_writable(args.app, exe, executable=True)
    # The binary is linked with an rpath into the Qt tree it was built against,
    # which does not exist on a user's machine; macdeployqt copies the
    # frameworks into the bundle but does not add the rpath that finds them.
    subprocess.run(
        ["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", exe], check=True
    )
    with open(os.path.join(app, "Contents", "Info.plist"), "w") as f:
        f.write(_INFO_PLIST.format(version=args.version))
    _run_deploy_tool(args.deploy_tool, app)
    if not os.path.isdir(os.path.join(app, "Contents", "Frameworks", "QtCore.framework")):
        sys.exit("macdeployqt did not bundle QtCore.framework")


def assemble_windows(args, stage):
    exe = os.path.join(stage, "FastECU.exe")
    _copy_writable(args.app, exe, executable=True)
    for extra in args.extra:
        src, _, dest = extra.partition("=")
        _copy_writable(src, os.path.join(stage, dest), executable=True)
    _run_deploy_tool(args.deploy_tool, exe)
    if not os.path.isfile(os.path.join(stage, "Qt6Core.dll")):
        sys.exit("windeployqt did not stage Qt6Core.dll")


_ASSEMBLERS = {"macos": assemble_macos, "windows": assemble_windows}


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("--kind", required=True, choices=sorted(_ASSEMBLERS))
    p.add_argument("--app", required=True, help="the built FastECU binary")
    p.add_argument("--deploy-tool", required=True, help="macdeployqt / windeployqt")
    p.add_argument(
        "--extra", action="append", default=[], help="SRC=DEST file placed beside the app"
    )
    p.add_argument("--version", default="dev")
    p.add_argument("--out", required=True)
    args = p.parse_args(argv)

    with tempfile.TemporaryDirectory() as stage:
        _ASSEMBLERS[args.kind](args, stage)
        zip_tree(stage, args.out)


if __name__ == "__main__":
    main()
