"""Sonar spike: inspect the Hedron compile_commands.json before it is scanned.

Reports whether the database can carry Qt's `emit` macro to Sonar's CFamily
analyzer: entry count, the compiler in argv[0], how many include directories
exist on this runner, and whether Qt's qobjectdefs.h (which defines `emit`)
resolves through them. With --rewrite-compiler, Bazel's cc_wrapper.sh in argv[0]
is replaced with plain clang (the rewrite refresh_sonar.py used to do), because
Sonar skips entries whose compiler it cannot identify.

Throwaway: delete with the spike (see the findings doc) unless the compile
database path is adopted.
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
    parser.add_argument("--rewrite-compiler", action="store_true")
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

    if args.rewrite_compiler:
        rewritten = 0
        for entry in entries:
            if entry["arguments"][0].endswith("cc_wrapper.sh"):
                entry["arguments"][0] = "clang"
                rewritten += 1
        args.database.write_text(json.dumps(entries))
        print(f"rewrote compiler in {rewritten}/{len(entries)} entries")

    return 0 if qt_hits else 1


if __name__ == "__main__":
    sys.exit(main())
