"""Fail the SonarCloud job if the compile database cannot carry Qt's `emit`.

Sonar's CFamily analyzer reads `emit Signal(...)` as a declaration when it cannot
open qobjectdefs.h (which defines `emit`), and reports shadowing false positives
(cpp:S1117, cpp:S824). The database lists generated include trees under
bazel-out/.../_virtual_includes; a build served from the remote cache may not
download them. Print how many include directories exist on this runner and exit
non-zero if no QtCore include directory resolves qobjectdefs.h.
"""

import argparse
import collections
import json
import pathlib
import sys

INCLUDE_PREFIXES = ("-I", "-isystem", "-iquote")
QT_MARKER = "qobjectdefs.h"


def include_dirs(arguments):
    """Yield the include directories named by one compile command."""
    pending = False
    for arg in arguments:
        if pending:
            yield arg
            pending = False
            continue
        for prefix in INCLUDE_PREFIXES:
            if arg == prefix:
                pending = True
            elif arg.startswith(prefix):
                yield arg[len(prefix) :]


def resolve(directory, path):
    p = pathlib.Path(path)
    return p if p.is_absolute() else pathlib.Path(directory) / p


def inspect(entries):
    argv0 = collections.Counter(pathlib.Path(e["arguments"][0]).name for e in entries)
    dirs = {}  # resolved include dir -> exists
    for entry in entries:
        for d in include_dirs(entry["arguments"]):
            dirs.setdefault(resolve(entry["directory"], d), None)
    for d in dirs:
        dirs[d] = d.is_dir()
    qt_hits = sum(1 for d, ok in dirs.items() if ok and (d / "QtCore" / QT_MARKER).exists())
    qt_dirs = sum(1 for d in dirs if "QtCore" in str(d))
    return argv0, dirs, qt_hits, qt_dirs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=pathlib.Path)
    args = parser.parse_args()

    entries = json.loads(args.database.read_text())
    argv0, dirs, qt_hits, qt_dirs = inspect(entries)
    present = sum(dirs.values())

    print(f"entries: {len(entries)}")
    print(f"argv[0] histogram: {dict(argv0)}")
    print(f"include dirs: {present}/{len(dirs)} exist on this runner")
    print(f"QtCore include dirs: {qt_dirs}; of those, resolving {QT_MARKER}: {qt_hits}")
    missing = sorted(str(d) for d, ok in dirs.items() if not ok)
    for d in missing[:10]:
        print(f"  missing: {d}")

    return 0 if qt_hits else 1


if __name__ == "__main__":
    sys.exit(main())
