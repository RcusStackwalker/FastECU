#!/usr/bin/env python3
"""Verify openpty users include portable headers and stay out of common tests."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    errors: list[str] = []
    candidates = list((ROOT / "tests").glob("*.cpp"))
    candidates += list((ROOT / "src/platform/desktop/common/serial").glob("*_test.cpp"))
    for path in sorted(candidates):
        text = path.read_text()
        if "openpty(" not in text:
            continue
        if "#if defined(__linux__)" not in text or "#include <pty.h>" not in text:
            errors.append(f"{path.relative_to(ROOT)}: openpty users must include <pty.h> on Linux")
        if "#include <util.h>" not in text:
            errors.append(
                f"{path.relative_to(ROOT)}: openpty users must include <util.h> off Linux"
            )
        if path.name == "direct_backend_test.cpp":
            errors.append(
                f"{path.relative_to(ROOT)}: openpty coverage must live in a Unix-only source file"
            )

    if errors:
        print("\n".join(errors))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
