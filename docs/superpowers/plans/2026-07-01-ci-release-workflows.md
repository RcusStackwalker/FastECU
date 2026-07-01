# FastECU CI/Release Workflows Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `externals/FastECU` a `pr.yml` that verifies Windows + macOS builds (and runs the existing Qt test suites) on every PR, and a `release.yml` that auto-tags and publishes zipped Windows + macOS packages on every push to `master`.

**Architecture:** Two new GitHub Actions workflow files plus one small fix to `FastECU.pro`. Both workflows use `jurplel/install-qt-action` to provision Qt 6.8.3 (MinGW arch on Windows, default clang arch on macOS), then drive the existing qmake project directly with shell steps — no new build-system layer. `release.yml` mirrors the two-job shape (`release` → `build`) already used in `codeinjector`'s and `mmc-patches`' release workflows: a `commisery-action` bump job creates the tag, then a matrix job builds and uploads platform zips.

**Tech Stack:** GitHub Actions, qmake/MinGW (Windows) and qmake/clang (macOS), `windeployqt`/`macdeployqt` for packaging, Chocolatey (Windows OpenSSL) and Homebrew (macOS OpenSSL, already wired in `FastECU.pro`).

## Global Constraints

- Repo: `externals/FastECU` (fork `RcusStackwalker/FastECU`), default branch `master`.
- Platforms: Windows + macOS only. No Linux job, no Windows installer, no macOS `.dmg`/signing/notarization (per approved spec).
- Qt version: Windows pins to `6.8.3` (verified available for `win64_mingw`); macOS pins to `6.11.1`. Different versions per platform is intentional, discovered during Task 2's first real CI run: Qt <6.11.1 hits a known Qt bug on macOS (`qyieldcpu.h` calls `__yield()` without a declaration, fatal under `-Werror` on the Xcode 26.5 SDK `macos-latest` now ships — fixed in Qt 6.11.1+, confirmed via Qt forum). That bug is macOS/Xcode-SDK-specific and doesn't affect Windows, so Windows stays on 6.8.3 rather than chasing Qt's Windows-package hosting quirks above 6.9.3 (aqt's per-version Windows metadata becomes unreliable to query above that point; 6.8.3 is confirmed solid).
- Both platforms' `modules:` list includes `qt5compat` in addition to `qtcharts qtserialport qtremoteobjects qtwebsockets` — also discovered during Task 2's first CI run: `cipher.cpp` (or a header it pulls in) uses `QtCore5Compat/QTextCodec`, which is a separate Qt6 add-on module not included by default.
- The Windows OpenSSL discovery step parses `choco install openssl`'s own `Deployed to '...'` output line rather than globbing candidate directories — discovered during Task 2's second CI run: the original `Get-ChildItem -Path 'C:\Program Files*' -Filter 'OpenSSL*'` glob returned nothing (root cause not fully diagnosed — the real install landed at `C:\Program Files\OpenSSL\`, confirmed by choco's own log line, which `-Filter 'OpenSSL*'` should have matched), so the discovery silently fell through to the chocolatey package-metadata fallback path (`C:\ProgramData\chocolatey\lib\openssl`), which exists but contains no headers/DLLs — `qmake` then failed with `cipher.h:13:10: fatal error: openssl/conf.h: No such file or directory`. Parsing choco's own authoritative deploy message is more robust than re-deriving the path via glob.
- `FastECU.pro`'s `win32{}` scope also reads an `OPENSSL_CRYPTO_DLL` env var (fallback `libcrypto-4-x64.dll`) and links via GNU ld's `-l:<exact filename>` syntax instead of `-lcrypto-3` — discovered during Task 2's third CI run: the choco-installed OpenSSL 4.0.1's actual runtime DLL is not literally named `libcrypto-3` (confirmed via web research on the slproweb Win64OpenSSL package), which `-lcrypto-3` (searching for `libcrypto-3.dll.a`/`libcrypto-3.a`/`crypto-3.lib`) never matches — `ld.exe: cannot find -lcrypto-3`. The CI step discovers the real filename by globbing `$OPENSSL_ROOT/bin/libcrypto-*.dll` rather than hardcoding the versioned suffix, since OpenSSL's DLL SONAME suffix is not guaranteed stable across releases. Discovered on the first real release build (post-merge): the installer ships BOTH `libcrypto-3-x64.dll` (legacy-SONAME compat shim) and `libcrypto-4-x64.dll` (matching the actually-installed v4.0.1 headers) side by side in `bin/` — the discovery step sorts numerically and picks the highest SONAME rather than throwing on the ambiguity, since the headers under `include/` always match the newest DLL.
- Task 3's Windows packaging step copies `$OPENSSL_ROOT/bin/$OPENSSL_CRYPTO_DLL` into `dist\` before running `windeployqt` — caught by Task 3's review, not live CI (this workflow only fires on push to `master`, so it had no CI run to catch it the way `pr.yml` did): `windeployqt` only bundles Qt's own dependencies (Qt DLLs, ICU, ANGLE, platform plugins), not third-party DLLs like OpenSSL, and `FastECU.pro`'s own comment confirms OpenSSL is always linked dynamically even under `CONFIG+=static`. Without this copy, the shipped "portable" Windows zip would be missing a hard runtime dependency and fail to launch on a machine without OpenSSL already installed.
- Windows Qt arch: `win64_mingw` (the default arch for this action on Windows is MSVC — must be set explicitly since `FastECU.pro`'s `static{}` block uses MinGW-only linker flags).
- Windows compiler tool: `tools_mingw1310` / variant `qt.tools.win64_mingw1310` (the MinGW version Qt 6.8's `win64_mingw` packages are built against).
- Action pins: match the SHA-pinned convention already used in `codeinjector/.github/workflows/release.yml` (`actions/checkout@df4cb1c...# v6`, `tomtom-international/commisery-action/bump@f88e873...# v7.0.0`, `softprops/action-gh-release@b430933...# v3`). The one new action, `jurplel/install-qt-action`, is pinned to `48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1` (verified via `gh api repos/jurplel/install-qt-action/git/refs/tags/v4.3.1`).
- All qmake invocations pass `CONFIG+=release CONFIG-=debug_and_release` so build output lands directly in the project directory (`FastECU.exe` / `FastECU.app`), not in a `release/` subfolder — avoids depending on qmake's default single- vs. split-config behavior per kit.
- Testing a `pull_request:`-triggered workflow inherently requires opening a real PR against the fork (there's no way to fire that trigger by pushing straight to `master`). This is a one-time, narrowly-scoped exception to the project's "commit directly to main" convention — open the PR to validate Task 2, then merge and delete the branch once green, per **Task 2**'s explicit checkpoint.
- Opening a PR against `RcusStackwalker/FastECU`, pushing branches, merging to `master`, and letting `release.yml` create a real public GitHub Release with binary assets are all visible, shared-state actions. Each task below has an explicit checkpoint before doing these — do not skip them.

---

### Task 1: Fix `FastECU.pro`'s hardcoded Windows OpenSSL path

**Files:**
- Modify: `externals/FastECU/FastECU.pro:20-35` (the `win32 {}` scope)

**Interfaces:**
- Produces: an `OPENSSL_ROOT` qmake variable (sourced from the `OPENSSL_ROOT` environment variable, falling back to `C:/Program Files/OpenSSL-Win64` if unset) that Task 2 and Task 3's Windows CI steps must export before invoking `qmake`.

- [ ] **Step 1: Edit the `win32{}` scope**

Current content (`FastECU.pro:20-35`):

```qmake
win32 {
    #LIBS += -LC:\Qt\5.9.9\mingw53_32\lib\libQt5OpenGL.a -lopengl32 -lsetupapi
    #OpenSSL library must be stated first because in case
    #of static build it cannot be linked static, dynamic only
    QMAKE_LFLAGS += -L\"C:\Program Files (x86)\OpenSSL-Win32\bin\" \
                    -L\"C:\Program Files\OpenSSL-Win32\bin\" \
                    -lcrypto-3
    LIBS += -lopengl32 -lsetupapi
    SOURCES += \
    serial_port/J2534_win.cpp
    HEADERS += \
    serial_port/J2534_win.h
    HEADERS += \
    serial_port/J2534_tactrix_win.h
    INCLUDEPATH += "C:\Program Files (x86)\OpenSSL-Win32\include" "C:\Program Files\OpenSSL-Win32\include"
}
```

Replace with:

```qmake
win32 {
    #LIBS += -LC:\Qt\5.9.9\mingw53_32\lib\libQt5OpenGL.a -lopengl32 -lsetupapi
    #OpenSSL library must be stated first because in case
    #of static build it cannot be linked static, dynamic only
    OPENSSL_ROOT = $$(OPENSSL_ROOT)
    isEmpty(OPENSSL_ROOT): OPENSSL_ROOT = "C:/Program Files/OpenSSL-Win64"
    OPENSSL_CRYPTO_DLL = $$(OPENSSL_CRYPTO_DLL)
    isEmpty(OPENSSL_CRYPTO_DLL): OPENSSL_CRYPTO_DLL = libcrypto-4-x64.dll
    QMAKE_LFLAGS += -L\"$$OPENSSL_ROOT/bin\" -l:$$OPENSSL_CRYPTO_DLL
    LIBS += -lopengl32 -lsetupapi
    SOURCES += \
    serial_port/J2534_win.cpp
    HEADERS += \
    serial_port/J2534_win.h
    HEADERS += \
    serial_port/J2534_tactrix_win.h
    INCLUDEPATH += $$OPENSSL_ROOT/include
}
```

- [ ] **Step 2: Sanity-check qmake syntax on macOS**

This machine can't evaluate the `win32{}` scope (macOS never enters it), but qmake still has to parse the whole file without error. Run from `externals/FastECU`:

```bash
qmake6 -o /tmp/fastecu-qmake-check.mk FastECU.pro
```

Expected: exits 0, no `Parse Error` output. (This does NOT prove the Windows-only lines are correct — that's validated for real by Task 2's Windows CI leg.)

- [ ] **Step 3: Commit**

```bash
cd externals/FastECU
git add FastECU.pro
git commit -m "$(cat <<'EOF'
build: resolve Windows OpenSSL path via env var instead of hardcoding

FastECU.pro hardcoded two absolute OpenSSL-Win32 paths with no
override, unlike the macx{} scope which already resolves its prefix
dynamically via brew --prefix. This blocks CI (and any dev machine
that installed OpenSSL somewhere else) from building on Windows.
EOF
)"
```

---

### Task 2: PR workflow — `pr.yml`

**Files:**
- Create: `externals/FastECU/.github/workflows/pr.yml`

**Interfaces:**
- Consumes: `OPENSSL_ROOT` env var contract from Task 1.
- Produces: nothing consumed by later tasks except the validated pattern (Qt install args, OpenSSL discovery script) that Task 3 reuses verbatim in `release.yml`.

- [ ] **Step 1: Write `pr.yml`**

```yaml
name: pr

on:
  pull_request:
    branches: [master]

jobs:
  build:
    strategy:
      fail-fast: false
      matrix:
        os: [windows-latest, macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@df4cb1c069e1874edd31b4311f1884172cec0e10 # v6

      - name: Install Qt (Windows)
        if: runner.os == 'Windows'
        uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: '6.8.3'
          arch: 'win64_mingw'
          modules: 'qtcharts qtserialport qtremoteobjects qtwebsockets qt5compat'
          tools: 'tools_mingw1310,qt.tools.win64_mingw1310'
          add-tools-to-path: true

      - name: Install Qt (macOS)
        if: runner.os == 'macOS'
        uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: '6.11.1'
          modules: 'qtcharts qtserialport qtremoteobjects qtwebsockets qt5compat'

      - name: Install OpenSSL (Windows)
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          $installOutput = choco install openssl -y --no-progress
          $installOutput
          $deployMatches = $installOutput | Select-String -Pattern "Deployed to '([^']+)'" | Where-Object { $_.Line -match 'openssl' }
          if ($deployMatches.Count -eq 0) {
            throw "Could not find an OpenSSL 'Deployed to' line in choco output"
          }
          if ($deployMatches.Count -gt 1) {
            throw "Found multiple OpenSSL 'Deployed to' lines in choco output, refusing to guess: $($deployMatches.Line -join ' | ')"
          }
          $opensslDir = $deployMatches[0].Matches[0].Groups[1].Value.TrimEnd('\') -replace '\\', '/'
          Write-Host "Resolved OPENSSL_ROOT=$opensslDir"
          "OPENSSL_ROOT=$opensslDir" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
          $cryptoDlls = Get-ChildItem -Path "$opensslDir/bin" -Filter 'libcrypto-*.dll' -ErrorAction SilentlyContinue
          if ($cryptoDlls.Count -eq 0) {
            throw "Could not find a libcrypto-*.dll in $opensslDir/bin"
          }
          $cryptoDll = ($cryptoDlls | Sort-Object { [int]([regex]::Match($_.Name, 'libcrypto-(\d+)-').Groups[1].Value) } -Descending | Select-Object -First 1).Name
          if ($cryptoDlls.Count -gt 1) {
            Write-Host "Multiple libcrypto-*.dll candidates found ($($cryptoDlls.Name -join ', ')), using highest SONAME: $cryptoDll"
          }
          Write-Host "Resolved OPENSSL_CRYPTO_DLL=$cryptoDll"
          "OPENSSL_CRYPTO_DLL=$cryptoDll" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append

      - name: Install OpenSSL (macOS)
        if: runner.os == 'macOS'
        run: brew install openssl@3

      - name: Build FastECU (Windows)
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release FastECU.pro
          mingw32-make

      - name: Build FastECU (macOS)
        if: runner.os == 'macOS'
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release FastECU.pro
          make

      # release.yml's static+windeployqt packaging path is release-only and was
      # never exercised by pr.yml -- that's exactly how the missing-OpenSSL-DLL
      # bug (fixed once already) got past review and only surfaced on a real
      # release. Rebuild statically here and package it the same way, so a
      # regression in that path fails a PR instead of a public release. No
      # upload -- this step exists purely to catch build/packaging failures.
      - name: Build and package Windows static (verify only)
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release CONFIG+=static FastECU.pro
          mingw32-make
          mkdir dist
          copy FastECU.exe dist\
          copy "$env:OPENSSL_ROOT/bin/$env:OPENSSL_CRYPTO_DLL" dist\
          windeployqt dist\FastECU.exe
          Compress-Archive -Path dist\* -DestinationPath pr-check-windows.zip

      - name: Package macOS (verify only)
        if: runner.os == 'macOS'
        run: |
          mkdir dist
          cp -R FastECU.app dist/
          macdeployqt dist/FastECU.app
          cd dist && zip -r ../pr-check-macos.zip FastECU.app

      - name: Build and run mut_dma_tests (Windows)
        if: runner.os == 'Windows'
        shell: pwsh
        working-directory: tests
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release tests.pro
          mingw32-make
          .\mut_dma_tests.exe

      - name: Build and run mut_dma_tests (macOS)
        if: runner.os == 'macOS'
        working-directory: tests
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release tests.pro
          make
          ./mut_dma_tests
          make clean

      - name: Build and run serial_crash_tests (macOS only)
        if: runner.os == 'macOS'
        working-directory: tests
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release serial_crash_tests.pro
          make
          ./serial_crash_tests
          make clean

      - name: Build and run mut_dma_integration_tests (macOS only)
        if: runner.os == 'macOS'
        working-directory: tests
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release mut_dma_integration_tests.pro
          make
          ./mut_dma_integration_tests
```

- [ ] **Step 2: Local syntax check**

```bash
cd externals/FastECU
python3 -c "import yaml, sys; yaml.safe_load(open('.github/workflows/pr.yml'))" && echo OK
```

Expected: `OK` (fails loudly on YAML indentation mistakes before you burn a CI run on them). If `yaml` isn't installed, run `python3 -m pip install --break-system-packages pyyaml` first, or eyeball the indentation against the block above — every `run:` block step is a single job-level list item.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/pr.yml
git commit -m "ci: add PR workflow building Windows + macOS and running test suites"
```

- [ ] **Step 4: CHECKPOINT — push a branch and open a real PR**

This is the only way to exercise a `pull_request:` trigger. Confirm with the user before doing this (it's a visible action against their fork). Then:

```bash
git checkout -b ci/pr-and-release-workflows
git push -u origin ci/pr-and-release-workflows
gh pr create --repo RcusStackwalker/FastECU --title "ci: add PR + release GitHub Actions workflows" --body "Adds pr.yml (Windows+macOS build+test) and fixes FastECU.pro's hardcoded OpenSSL path so Windows CI can build. release.yml lands in the next commit on this branch."
```

- [ ] **Step 5: Watch the run**

```bash
gh pr checks --repo RcusStackwalker/FastECU --watch
```

Expected: both `build (windows-latest)` and `build (macos-latest)` go green. If the Windows leg fails at the OpenSSL discovery step, `gh run view --log-failed` will show what `Get-ChildItem` actually found (or didn't) — adjust the discovery globs in Step 1 to match and push a fix commit to the same branch before moving on. Do not proceed to Task 3 until both legs are green.

---

### Task 3: Release workflow — `release.yml`

**Files:**
- Create: `externals/FastECU/.github/workflows/release.yml`

**Interfaces:**
- Consumes: the same `OPENSSL_ROOT` contract from Task 1, and the Qt-install/OpenSSL-install step shapes validated in Task 2.
- Produces: two GitHub Release assets per tag: `FastECU-<tag>-windows.zip`, `FastECU-<tag>-macos.zip`.

- [ ] **Step 1: Write `release.yml`**

```yaml
name: Release

on:
  push:
    branches: [master]

permissions:
  contents: write

jobs:
  release:
    runs-on: ubuntu-latest
    outputs:
      tag: ${{ steps.get-tag.outputs.tag }}
    steps:
      - uses: actions/checkout@df4cb1c069e1874edd31b4311f1884172cec0e10 # v6
        with:
          fetch-depth: 0

      - uses: tomtom-international/commisery-action/bump@f88e8737a52bfbe4b20225399da697acdda5035d # v7.0.0
        with:
          token: ${{ secrets.GITHUB_TOKEN }}
          create-release: true

      - name: Get created tag
        id: get-tag
        run: |
          git fetch --tags
          echo "tag=$(git describe --tags --abbrev=0)" >> $GITHUB_OUTPUT

  build:
    needs: release
    if: needs.release.outputs.tag != ''
    strategy:
      fail-fast: false
      matrix:
        os: [windows-latest, macos-latest]
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@df4cb1c069e1874edd31b4311f1884172cec0e10 # v6
        with:
          ref: ${{ needs.release.outputs.tag }}

      - name: Install Qt (Windows)
        if: runner.os == 'Windows'
        uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: '6.8.3'
          arch: 'win64_mingw'
          modules: 'qtcharts qtserialport qtremoteobjects qtwebsockets qt5compat'
          tools: 'tools_mingw1310,qt.tools.win64_mingw1310'
          add-tools-to-path: true

      - name: Install Qt (macOS)
        if: runner.os == 'macOS'
        uses: jurplel/install-qt-action@48d3ad6db93f3627c8ee7a0454bc6f3744f7e730 # v4.3.1
        with:
          version: '6.11.1'
          modules: 'qtcharts qtserialport qtremoteobjects qtwebsockets qt5compat'

      - name: Install OpenSSL (Windows)
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          $installOutput = choco install openssl -y --no-progress
          $installOutput
          $deployMatches = $installOutput | Select-String -Pattern "Deployed to '([^']+)'" | Where-Object { $_.Line -match 'openssl' }
          if ($deployMatches.Count -eq 0) {
            throw "Could not find an OpenSSL 'Deployed to' line in choco output"
          }
          if ($deployMatches.Count -gt 1) {
            throw "Found multiple OpenSSL 'Deployed to' lines in choco output, refusing to guess: $($deployMatches.Line -join ' | ')"
          }
          $opensslDir = $deployMatches[0].Matches[0].Groups[1].Value.TrimEnd('\') -replace '\\', '/'
          Write-Host "Resolved OPENSSL_ROOT=$opensslDir"
          "OPENSSL_ROOT=$opensslDir" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
          $cryptoDlls = Get-ChildItem -Path "$opensslDir/bin" -Filter 'libcrypto-*.dll' -ErrorAction SilentlyContinue
          if ($cryptoDlls.Count -eq 0) {
            throw "Could not find a libcrypto-*.dll in $opensslDir/bin"
          }
          $cryptoDll = ($cryptoDlls | Sort-Object { [int]([regex]::Match($_.Name, 'libcrypto-(\d+)-').Groups[1].Value) } -Descending | Select-Object -First 1).Name
          if ($cryptoDlls.Count -gt 1) {
            Write-Host "Multiple libcrypto-*.dll candidates found ($($cryptoDlls.Name -join ', ')), using highest SONAME: $cryptoDll"
          }
          Write-Host "Resolved OPENSSL_CRYPTO_DLL=$cryptoDll"
          "OPENSSL_CRYPTO_DLL=$cryptoDll" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append

      - name: Install OpenSSL (macOS)
        if: runner.os == 'macOS'
        run: brew install openssl@3

      - name: Build Windows portable package
        if: runner.os == 'Windows'
        shell: pwsh
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release CONFIG+=static FastECU.pro
          mingw32-make
          mkdir dist
          copy FastECU.exe dist\
          copy "$env:OPENSSL_ROOT/bin/$env:OPENSSL_CRYPTO_DLL" dist\
          windeployqt dist\FastECU.exe
          Compress-Archive -Path dist\* -DestinationPath FastECU-${{ needs.release.outputs.tag }}-windows.zip

      - name: Build macOS package
        if: runner.os == 'macOS'
        run: |
          qmake CONFIG+=release CONFIG-=debug_and_release FastECU.pro
          make
          mkdir dist
          cp -R FastECU.app dist/
          macdeployqt dist/FastECU.app
          cd dist && zip -r ../FastECU-${{ needs.release.outputs.tag }}-macos.zip FastECU.app

      - name: Upload release asset (Windows)
        if: runner.os == 'Windows'
        uses: softprops/action-gh-release@b4309332981a82ec1c5618f44dd2e27cc8bfbfda # v3
        with:
          tag_name: ${{ needs.release.outputs.tag }}
          files: FastECU-${{ needs.release.outputs.tag }}-windows.zip

      - name: Upload release asset (macOS)
        if: runner.os == 'macOS'
        uses: softprops/action-gh-release@b4309332981a82ec1c5618f44dd2e27cc8bfbfda # v3
        with:
          tag_name: ${{ needs.release.outputs.tag }}
          files: FastECU-${{ needs.release.outputs.tag }}-macos.zip
```

Note on `CONFIG+=static`: it statically links `libgcc`/`libstdc++`/`libwinpthread` into `FastECU.exe` (per the `static{}` block already in `FastECU.pro`) but does **not** statically link Qt itself — official Qt binaries from `aqt`/`install-qt-action` are shared-library builds. `windeployqt` is what actually makes the Windows package portable, by copying the required `Qt6*.dll` files and platform plugins next to the exe. Combining both gets you a folder with no MinGW runtime DLLs to track (statically linked) and all the Qt DLLs it does need (deployed) — that is what "portable" means here.

- [ ] **Step 2: Local syntax check**

```bash
cd externals/FastECU
python3 -c "import yaml, sys; yaml.safe_load(open('.github/workflows/release.yml'))" && echo OK
```

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "ci: add release workflow publishing Windows + macOS packages"
git push
```

- [ ] **Step 4: CHECKPOINT — merge the PR from Task 2**

Merging to `master` is what fires `release.yml` for real and creates a public GitHub Release with binary assets on `RcusStackwalker/FastECU`. Confirm with the user before merging. Once confirmed:

```bash
gh pr merge --repo RcusStackwalker/FastECU ci/pr-and-release-workflows --squash --delete-branch
```

- [ ] **Step 5: Watch the release run and verify the artifacts**

```bash
gh run watch --repo RcusStackwalker/FastECU
gh release view --repo RcusStackwalker/FastECU $(gh release list --repo RcusStackwalker/FastECU --limit 1 --json tagName --jq '.[0].tagName')
```

Expected: a new tag/release exists with two assets, `FastECU-<tag>-windows.zip` and `FastECU-<tag>-macos.zip`. Download and spot-check at least one:

```bash
TAG=$(gh release list --repo RcusStackwalker/FastECU --limit 1 --json tagName --jq '.[0].tagName')
gh release download --repo RcusStackwalker/FastECU "$TAG" -p "*macos.zip" -D /tmp/fastecu-release-check
unzip -l /tmp/fastecu-release-check/*macos.zip | head -20
```

Expected: the listing shows `FastECU.app/Contents/MacOS/FastECU` plus a populated `Contents/Frameworks/` (Qt frameworks bundled by `macdeployqt`) — not just a bare binary.

---

## Self-Review Notes

- **Spec coverage:** PR verification (Windows+macOS build+test) → Task 2. Auto-versioned release → Task 3's `release` job. Windows portable zip / macOS zipped `.app`, both unsigned → Task 3's `build` job. `.pro` OpenSSL fix → Task 1. All spec sections have a task.
- **Placeholder scan:** no TBD/TODO; every step has literal file content or literal commands with expected output.
- **Type/name consistency:** `OPENSSL_ROOT` and `OPENSSL_CRYPTO_DLL` are the identifiers shared across tasks — spelled identically in Task 1's qmake fix, Task 2's PR workflow, and Task 3's release workflow (the latter env var was added after Task 2's third live CI run surfaced a link-time failure; this document was updated in place to match). Zip filenames (`FastECU-<tag>-windows.zip` / `FastECU-<tag>-macos.zip`) are produced and consumed within the same job step in Task 3, so no cross-job naming drift is possible.
