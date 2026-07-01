# CI / release workflows for FastECU

## Problem

`externals/FastECU` (fork `RcusStackwalker/FastECU`) has no GitHub Actions
workflows at all — neither the upstream repo nor this fork builds the app in
CI. Every other repo in this project (`codeinjector`, `m32r-injector-toolchain`,
`mmc-patches`) already has a `pr.yml` (compile-and-verify) + `release.yml`
(auto-versioned release with build artifacts) pair. FastECU needs the same
shape, adapted to a Qt/qmake C++ GUI app instead of a Rust/Make project.

Goal: every PR gets a verified build on the two platforms we ship for
(Windows, macOS); every push to `master` produces a tagged GitHub release
with a downloadable Windows and macOS package.

## Non-goals

- No Linux PR job or Linux release artifact (out of scope for this round,
  even though a `flatpak/` packaging dir already exists in the repo).
- No Windows installer (NSIS/Inno Setup) — the release ships the existing
  portable static `.exe` build mode instead.
- No macOS `.dmg`, code signing, or notarization — release ships a plain
  zipped `.app`, unsigned.
- No new lint/format stage — there is no existing `.clang-format` or
  pre-commit config in FastECU to run.

## PR workflow — `.github/workflows/pr.yml`

Trigger: `pull_request` against the fork's default branch (`master`).

Matrix: `windows-latest`, `macos-latest`.

Per-OS steps:

1. Checkout.
2. `jurplel/install-qt-action` — Qt 6, modules beyond Essentials:
   `qtcharts`, `qtserialport`, `qtremoteobjects`, `qtwebsockets`.
   - Windows: install the MinGW Qt kit. `FastECU.pro`'s `static{}` block uses
     MinGW-specific flags (`-static-libgcc -static-libstdc++ -lwinpthread`),
     so the CI kit has to match.
   - macOS: default Qt kit is fine (native arm64/x86_64 per runner).
3. OpenSSL:
   - Windows: install OpenSSL for Windows and set `OPENSSL_ROOT` to its
     install path (consumed by the `.pro` fix below).
   - macOS: `brew install openssl@3` (macOS runners already have Homebrew;
     `FastECU.pro`'s `macx{}` scope already resolves this via
     `brew --prefix openssl@3`, no change needed).
4. `qmake && make` (or `qmake6`, depending on the installed kit's naming) to
   build the main app. This is the core "does it compile" gate.
5. Build and run the test suites that apply to that OS:
   - Both OSes: `tests/tests.pro` → `mut_dma_tests`. Pure protocol-logic
     tests (`QT -= gui`), no OS-specific dependency.
   - macOS only: `tests/serial_crash_tests.pro` → `serial_crash_tests`, and
     `tests/mut_dma_integration_tests.pro` → `mut_dma_integration_tests`.
     Both link `serial_port/J2534_unix.cpp` unconditionally and the
     integration test opens a PTY — neither exists on Windows, so the
     Windows job does not attempt these two suites.
6. Fail the job on any compile error or test failure. No artifacts are
   uploaded from the PR job — packaging only happens on release, matching
   the pattern in `mmc-patches`/`codeinjector`.

## Release workflow — `.github/workflows/release.yml`

Trigger: `push` to `master`. `permissions: contents: write`.

**Job 1 — `release`** (`ubuntu-latest`): identical shape to the other repos'
release jobs — `tomtom-international/commisery-action/bump` reads
Conventional Commit messages since the last tag, bumps semver, and creates
the tag + GitHub release. Downstream jobs gate on
`needs.release.outputs.tag != ''`.

**Job 2 — `build`** (`needs: release`, matrix `windows-latest`/`macos-latest`,
checks out `needs.release.outputs.tag`):

- Same Qt/OpenSSL setup as the PR workflow.
- **Windows:** build with `CONFIG+=static` — this already exists in
  `FastECU.pro` specifically to produce "one portable .exe file that
  includes everything" (per the comment already in the project file). Zip
  the resulting `.exe` as `FastECU-<tag>-windows.zip`.
- **macOS:** normal (non-static) build, then `macdeployqt FastECU.app` to
  bundle the Qt frameworks into the app bundle. Zip the bundle as
  `FastECU-<tag>-macos.zip`. No signing — first launch requires a
  right-click → Open to clear Gatekeeper, which is acceptable for this
  unsigned hobby-OSS distribution model.
- Upload both zip assets to the release via `softprops/action-gh-release`,
  `tag_name: ${{ needs.release.outputs.tag }}`.

## `FastECU.pro` fix — dynamic OpenSSL path on Windows

Current state: the `win32{}` scope hardcodes two absolute OpenSSL install
paths (`C:\Program Files (x86)\OpenSSL-Win32\bin` and
`C:\Program Files\OpenSSL-Win32\bin`) and links `-lcrypto-3`. This only works
if a dev machine happens to have OpenSSL installed at exactly one of those
paths — there's no override, unlike the `macx{}` scope which already
resolves its prefix dynamically via `$$system(brew --prefix openssl@3)`.

Fix: make the `win32{}` scope resolve an `OPENSSL_ROOT` value the same way,
falling back to a sane default if unset:

```qmake
win32 {
    OPENSSL_ROOT = $$(OPENSSL_ROOT)
    isEmpty(OPENSSL_ROOT): OPENSSL_ROOT = "C:/Program Files/OpenSSL-Win64"
    INCLUDEPATH += $$OPENSSL_ROOT/include
    QMAKE_LFLAGS += -L\"$$OPENSSL_ROOT/bin\" -L\"$$OPENSSL_ROOT/lib\"
    LIBS += -lopengl32 -lsetupapi -lcrypto-3
    SOURCES += serial_port/J2534_win.cpp
    HEADERS += serial_port/J2534_win.h serial_port/J2534_tactrix_win.h
    INCLUDEPATH += "C:/Program Files (x86)/OpenSSL-Win32/include" "C:/Program Files/OpenSSL-Win32/include"
}
```

CI sets `OPENSSL_ROOT` in the workflow env to wherever the install step put
it. A local Windows dev machine either sets the same env var or relies on
the fallback default. This is the only source change in this design — the
rest is new workflow YAML.

## Testing / validation

- PR workflow validated by opening a throwaway PR against the fork and
  confirming both matrix legs go green (compile + correct subset of tests).
- Release workflow validated by merging to `master` once and confirming a
  tag, GitHub release, and both zip assets appear with a working exe/app
  inside.
- No CI changes touch ECU-facing code paths, so no bench qualification is
  needed for this work.

## Open follow-ups (not blocking this design)

- Linux PR/release coverage, if the existing `flatpak/` packaging is ever
  wired up.
- Signed/notarized macOS builds, if an Apple Developer account is obtained
  later.
- A proper Windows installer, if portable-exe distribution proves
  insufficient.
