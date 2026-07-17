#!/usr/bin/env python3
from __future__ import annotations

import aqt.metadata
path = aqt.metadata.__file__
with open(path, encoding="utf-8") as handle:
    text = handle.read()
old_extensions = """        elif self.target == "android" and version >= Version("6.0.0"):
            return list(ArchiveId.EXTENSIONS_REQUIRED_ANDROID_QT6)
        else:
            return [""]
"""
new_extensions = """        elif self.host == "windows" and self.target == "desktop" and version >= Version("6.11.0"):
            return ["mingw", "msvc2022_64", "msvc2022_arm64_cross_compiled", "llvm_mingw"]
        elif self.target == "android" and version >= Version("6.0.0"):
            return list(ArchiveId.EXTENSIONS_REQUIRED_ANDROID_QT6)
        else:
            return [""]
"""
old_arch = """        elif architecture.startswith("android_") and is_version_ge_6:
            ext = architecture[len("android_") :]
            if ext in ArchiveId.EXTENSIONS_REQUIRED_ANDROID_QT6:
                return ext
        return ""
"""
new_arch = """        elif architecture == "win64_mingw" and is_version_ge_6:
            return "mingw"
        elif architecture == "win64_msvc2022_64" and is_version_ge_6:
            return "msvc2022_64"
        elif architecture == "win64_msvc2022_arm64_cross_compiled" and is_version_ge_6:
            return "msvc2022_arm64_cross_compiled"
        elif architecture == "win64_llvm_mingw" and is_version_ge_6:
            return "llvm_mingw"
        elif architecture.startswith("android_") and is_version_ge_6:
            ext = architecture[len("android_") :]
            if ext in ArchiveId.EXTENSIONS_REQUIRED_ANDROID_QT6:
                return ext
        return ""
"""
for old, new in ((old_extensions, new_extensions), (old_arch, new_arch)):
    if old not in text and new not in text:
        raise SystemExit(f"aqt metadata layout did not match expected source: {path}")
    text = text.replace(old, new)
with open(path, "w", encoding="utf-8") as handle:
    handle.write(text)
