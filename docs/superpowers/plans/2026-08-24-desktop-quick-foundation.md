# Desktop Quick QtQuick/Bazel Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a separately buildable, package-verifiable QtQuick desktop application with an embedded minimal OmniHaste shell and no hardware or dashboard behavior.

**Architecture:** Keep QtQuick dependencies isolated from the Widgets dependency set, embed QML through the existing Bazel qrc rule, and expose one small C++ loader used by the executable and an offscreen smoke test. Preserve `//:fastecu` while adding `//:fastecu-desktop-quick` as a second composition root.

**Tech Stack:** C++23, Qt 6.8.3 QGuiApplication/QML/Quick/Quick Controls 2/Quick Layouts/QtTest, rules_qt 0.0.6, Bazel 9.1.1, Bash, PowerShell, macdeployqt, windeployqt, GitHub Actions, prek

**Spec:** `docs/superpowers/specs/2026-08-24-desktop-quick-dashboard-design.md`

## Global Constraints

- Implement only planning item 1 from the spec: QtQuick/Bazel foundation and the minimal `desktop-quick` shell.
- Do not add `.ohd` models, logging, adapter discovery, transports, dashboard cards, or editor behavior.
- Keep `//apps/desktop:fastecu` and `//:fastecu` unchanged and buildable.
- Add `//apps/desktop-quick:fastecu-desktop-quick` and expose it as `//:fastecu-desktop-quick`.
- Preserve dependency flow `apps/desktop-quick -> src/ui/desktop-quick`; add no platform, backend, serial, transport, or Widgets dependency.
- Keep Quick dependencies in `QT_QUICK_DEPS`; do not add them to `QT_DEPS`.
- Use the pinned Qt 6.8.3, rules_qt 0.0.6, Bazel 9.1.1, and C++23 toolchain. Bazel remains the only target graph.
- Use the Basic Quick Controls style for deterministic test and package behavior.
- Product-facing copy says `OmniHaste`; existing `fastecu::` namespaces remain.
- Embed and load `qrc:/omnihaste/qml/Main.qml`; runtime must not need the checkout.
- Linux, macOS, and Windows build and run the offscreen smoke test. macOS and Windows PR packages scan `src/ui/desktop-quick/qml`.
- Release publication and `.ohd` associations remain planning item 5.

---

## File map

- Modify `bazel/qt_targets.bzl`: isolated Quick dependencies and test macro.
- Create `src/ui/desktop-quick/BUILD.bazel`: resources, loader, and smoke test.
- Create `src/ui/desktop-quick/desktop_quick_application.{h,cpp}`: root loader.
- Create `src/ui/desktop-quick/desktop_quick_application_test.cpp`: shell contract.
- Create `src/ui/desktop-quick/qml.qrc`, `qml/Main.qml`, and `qml/shell/ApplicationShell.qml`.
- Create `apps/desktop-quick/BUILD.bazel` and `main.cpp`: composition root.
- Modify root `BUILD.bazel` and `README.md`: alias and developer commands.
- Create `scripts/package-desktop-quick-macos.sh` and `scripts/package-desktop-quick-windows.ps1`.
- Modify `.github/workflows/pr.yml`: verify and upload both new packages.

---

### Task 1: Add isolated QtQuick build support and the tested embedded shell

**Files:**
- Modify: `bazel/qt_targets.bzl`
- Create: `src/ui/desktop-quick/BUILD.bazel`
- Create: `src/ui/desktop-quick/desktop_quick_application.h`
- Create: `src/ui/desktop-quick/desktop_quick_application.cpp`
- Create: `src/ui/desktop-quick/desktop_quick_application_test.cpp`
- Create: `src/ui/desktop-quick/qml.qrc`
- Create: `src/ui/desktop-quick/qml/Main.qml`
- Create: `src/ui/desktop-quick/qml/shell/ApplicationShell.qml`

**Interfaces:**
- Consumes: existing Qt Bazel macros and `@rules_qt//:qt_core`, `@rules_qt//:qt_gui`, `@rules_qt//:qt_qml`, `@rules_qt//:qt_quick`, `@rules_qt//:qt_quick_controls2`, `@rules_qt//:qt_quick_controls2_basic`, `@rules_qt//:qt_quick_layouts`, and `@rules_qt//:qt_test`
- Produces: `QT_QUICK_DEPS`; `fastecu_quicktest(...)`; `bool fastecu::desktop_quick::load_root(QQmlApplicationEngine&)`; `//src/ui/desktop-quick:application`

- [ ] **Step 1: Add the Quick dependency set and test macro**

Add after `QT_DEPS` in `bazel/qt_targets.bzl`:

```starlark
QT_QUICK_DEPS = [
    "@rules_qt//:qt_core",
    "@rules_qt//:qt_gui",
    "@rules_qt//:qt_qml",
    "@rules_qt//:qt_quick",
    "@rules_qt//:qt_quick_controls2",
    "@rules_qt//:qt_quick_controls2_basic",
    "@rules_qt//:qt_quick_layouts",
]
```

Add after `fastecu_qttest`:

```starlark
def fastecu_quicktest(
        name,
        src,
        deps = [],
        data = [],
        env = {},
        tags = [],
        target_compatible_with = [],
        copts = [],
        size = "small"):
    """QtQuick/QtTest target with moc generation and no Widgets dependency."""
    moc_target = name + "_moc"
    qt_cpp_moc_headers(
        name = moc_target,
        srcs = [src],
        deps = QT_QUICK_DEPS + ["@rules_qt//:qt_test"],
    )
    _qt_cc_test(
        name = name,
        srcs = [src],
        copts = COMMON_COPTS + copts,
        data = data,
        env = env,
        size = size,
        tags = tags,
        target_compatible_with = target_compatible_with,
        deps = QT_QUICK_DEPS + ["@rules_qt//:qt_test", ":" + moc_target] + deps,
    )
```

- [ ] **Step 2: Add resource aliases and a deliberately incomplete root shell**

Create `src/ui/desktop-quick/qml.qrc`:

```xml
<RCC>
    <qresource prefix="/omnihaste">
        <file alias="qml/Main.qml">qml/Main.qml</file>
        <file alias="qml/shell/ApplicationShell.qml">qml/shell/ApplicationShell.qml</file>
    </qresource>
</RCC>
```

Create `src/ui/desktop-quick/qml/Main.qml`:

```qml
import QtQuick
import "shell"

ApplicationShell {
}
```

Create the initial `qml/shell/ApplicationShell.qml`. It intentionally lacks the
children asserted by the test below:

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    objectName: "desktopQuickRoot"
    width: 1280
    height: 800
    visible: true
    title: qsTr("OmniHaste")
    color: "#0b1018"
}
```

- [ ] **Step 3: Add the root loader**

Create `desktop_quick_application.h`:

```cpp
#pragma once

class QQmlApplicationEngine;

namespace fastecu::desktop_quick
{

bool load_root(QQmlApplicationEngine& engine);

} // namespace fastecu::desktop_quick
```

Create `desktop_quick_application.cpp`:

```cpp
#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QQmlApplicationEngine>
#include <QString>
#include <QUrl>

namespace fastecu::desktop_quick
{

bool load_root(QQmlApplicationEngine& engine)
{
    engine.load(QUrl{QStringLiteral("qrc:/omnihaste/qml/Main.qml")});
    return !engine.rootObjects().isEmpty();
}

} // namespace fastecu::desktop_quick
```

- [ ] **Step 4: Add the package target and failing shell-contract test**

Create `desktop_quick_application_test.cpp`:

```cpp
#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QString>
#include <QtTest>

namespace
{
class DesktopQuickApplicationTest : public QObject
{
    Q_OBJECT
  private slots:
    void loadsEmbeddedApplicationShell()
    {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QQmlApplicationEngine engine;
        QVERIFY(fastecu::desktop_quick::load_root(engine));
        QCOMPARE(engine.rootObjects().size(), 1);
        QObject *root = engine.rootObjects().front();
        QCOMPARE(root->objectName(), QStringLiteral("desktopQuickRoot"));
        QCOMPARE(root->property("title").toString(), QStringLiteral("OmniHaste"));
        QVERIFY(root->findChild<QObject *>(QStringLiteral("navigationRail")) != nullptr);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("dashboardNavigation")) != nullptr);
        QVERIFY(root->findChild<QObject *>(QStringLiteral("workspace")) != nullptr);
    }
};
} // namespace

QTEST_MAIN(DesktopQuickApplicationTest)
#include "desktop_quick_application_test.moc"
```

Create `src/ui/desktop-quick/BUILD.bazel`:

```starlark
load(
    "//bazel:qt_targets.bzl",
    "COMMON_COPTS",
    "QT_QUICK_DEPS",
    "fastecu_quicktest",
    "qt_cc_library",
    "qt_resource_via_qrc",
)

package(default_visibility = ["//apps/desktop-quick:__pkg__"])

qt_resource_via_qrc(
    name = "qml_resources",
    files = ["qml/Main.qml", "qml/shell/ApplicationShell.qml"],
    qrc_file = "qml.qrc",
    deps = ["@rules_qt//:qt_core"],
)

qt_cc_library(
    name = "application",
    srcs = ["desktop_quick_application.cpp"],
    copts = COMMON_COPTS,
    normal_hdrs = ["desktop_quick_application.h"],
    deps = QT_QUICK_DEPS + [":qml_resources"],
)

fastecu_quicktest(
    name = "test_application",
    src = "desktop_quick_application_test.cpp",
    env = {
        "QT_QPA_PLATFORM": "offscreen",
        "QT_QUICK_CONTROLS_STYLE": "Basic",
    },
    deps = [":application"],
)
```

- [ ] **Step 5: Verify the test fails at the missing shell child**

Run:

```bash
bazel test --config=release //src/ui/desktop-quick:test_application
```

Expected: QML loads, then the test FAILS because `navigationRail` is absent. A
QML import failure or empty `rootObjects()` is a spike blocker; fix module
linkage instead of weakening the test.

- [ ] **Step 6: Implement the minimal shell**

Replace `ApplicationShell.qml` with:

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "desktopQuickRoot"
    width: 1280
    height: 800
    minimumWidth: 900
    minimumHeight: 600
    visible: true
    title: qsTr("OmniHaste")
    color: "#0b1018"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "navigationRail"
            color: "#111827"
            Layout.preferredWidth: 220
            Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 18
                Label {
                    text: qsTr("OmniHaste")
                    color: "#f8fafc"
                    font.pixelSize: 22
                    font.bold: true
                }
                Button {
                    objectName: "dashboardNavigation"
                    text: qsTr("Dashboard")
                    Layout.fillWidth: true
                }
                Item { Layout.fillHeight: true }
            }
        }

        Rectangle {
            objectName: "workspace"
            color: root.color
            Layout.fillWidth: true
            Layout.fillHeight: true
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 10
                Label {
                    text: qsTr("Dashboard")
                    color: "#f8fafc"
                    font.pixelSize: 28
                    font.bold: true
                    Layout.alignment: Qt.AlignHCenter
                }
                Label {
                    text: qsTr("Open an OmniHaste dashboard to begin.")
                    color: "#94a3b8"
                    font.pixelSize: 15
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }
    }
}
```

Do not add cards, fake data, connection controls, or future navigation items.

- [ ] **Step 7: Run the focused test and verify it passes**

```bash
bazel test --config=release //src/ui/desktop-quick:test_application
```

Expected: PASS with one root object and all named shell objects present.

- [ ] **Step 8: Verify isolation and formatting**

```bash
bazel build --config=release //src/ui/desktop-quick:application
prek run --files bazel/qt_targets.bzl src/ui/desktop-quick/BUILD.bazel \
  src/ui/desktop-quick/desktop_quick_application.h \
  src/ui/desktop-quick/desktop_quick_application.cpp \
  src/ui/desktop-quick/desktop_quick_application_test.cpp \
  src/ui/desktop-quick/qml.qrc src/ui/desktop-quick/qml/Main.qml \
  src/ui/desktop-quick/qml/shell/ApplicationShell.qml
git diff --check
```

Expected: all commands exit 0. Confirm `QT_DEPS` is unchanged and the new UI
target has no dependency on `//src/ui/desktop`, `//src/platform`, or
`//src/backend`.

- [ ] **Step 9: Commit the shell foundation**

```bash
git add bazel/qt_targets.bzl src/ui/desktop-quick
git commit -m "feat(ui): add embedded desktop-quick shell"
```

---

### Task 2: Add the desktop-quick executable composition root

**Files:**
- Create: `apps/desktop-quick/BUILD.bazel`
- Create: `apps/desktop-quick/main.cpp`
- Modify: `BUILD.bazel:5-14`
- Modify: `README.md` in the Build and test section

**Interfaces:**
- Consumes: `QT_QUICK_DEPS`, `//src/ui/desktop-quick:application`, and `fastecu::desktop_quick::load_root(QQmlApplicationEngine&)`
- Produces: `//apps/desktop-quick:fastecu-desktop-quick`; `//:fastecu-desktop-quick`; `--smoke-test`, which exits after root QML loads

- [ ] **Step 1: Record the missing-target failure**

```bash
bazel build --config=release //:fastecu-desktop-quick
```

Expected: FAIL because neither the root alias nor executable exists.

- [ ] **Step 2: Add the package-owned executable**

Create `apps/desktop-quick/BUILD.bazel`:

```starlark
load("//bazel:qt_targets.bzl", "COMMON_COPTS", "QT_QUICK_DEPS", "qt_cc_binary")

package(default_visibility = ["//:__pkg__"])

qt_cc_binary(
    name = "fastecu-desktop-quick",
    srcs = ["main.cpp"],
    copts = COMMON_COPTS,
    deps = QT_QUICK_DEPS + ["//src/ui/desktop-quick:application"],
)
```

Create `apps/desktop-quick/main.cpp`:

```cpp
#include "src/ui/desktop-quick/desktop_quick_application.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QString>
#include <QStringList>

#include <cstdlib>

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OmniHaste"));
    QCoreApplication::setApplicationName(QStringLiteral("OmniHaste"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;
    if (!fastecu::desktop_quick::load_root(engine))
    {
        return EXIT_FAILURE;
    }
    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test")))
    {
        return EXIT_SUCCESS;
    }
    return QGuiApplication::exec();
}
```

Do not add file, adapter, or protocol CLI options. `--smoke-test` is private
startup verification plumbing.

- [ ] **Step 3: Add the new root alias without changing existing aliases**

Add immediately after the existing `fastecu` alias in root `BUILD.bazel`:

```starlark
alias(
    name = "fastecu-desktop-quick",
    actual = "//apps/desktop-quick:fastecu-desktop-quick",
)
```

- [ ] **Step 4: Document the second application entry point**

Add after the existing `bazel build --config=release //:fastecu` example in
`README.md`:

````markdown
The in-progress OmniHaste QtQuick application is a separate target. Its current
foundation build contains only the application shell:

```sh
bazel build --config=release //:fastecu-desktop-quick
QT_QPA_PLATFORM=offscreen bazel run --config=release //:fastecu-desktop-quick -- --smoke-test
```
````

- [ ] **Step 5: Build both composition roots**

```bash
bazel build --config=release //:fastecu //:fastecu-desktop-quick
```

Expected: PASS. Confirm `//:fastecu` still resolves to
`//apps/desktop:fastecu` and the new alias resolves to `apps/desktop-quick`.

- [ ] **Step 6: Run the non-interactive executable smoke mode**

```bash
QT_QPA_PLATFORM=offscreen bazel run --config=release //:fastecu-desktop-quick -- --smoke-test
```

Expected: exit 0 after constructing `QGuiApplication`, loading the embedded
QML and Quick Controls imports, and constructing the shell without entering the
long-running event loop.

- [ ] **Step 7: Run focused checks**

```bash
bazel test --config=release //src/ui/desktop-quick:test_application
prek run --files BUILD.bazel README.md apps/desktop-quick/BUILD.bazel \
  apps/desktop-quick/main.cpp
git diff --check
```

Expected: all commands exit 0.

- [ ] **Step 8: Commit the composition root**

```bash
git add BUILD.bazel README.md apps/desktop-quick
git commit -m "feat(app): add desktop-quick composition root"
```

---

### Task 3: Verify QtQuick deployment in macOS and Windows PR packages

**Files:**
- Create: `scripts/package-desktop-quick-macos.sh`
- Create: `scripts/package-desktop-quick-windows.ps1`
- Modify: `.github/workflows/pr.yml:112-144`

**Interfaces:**
- Consumes: `//:fastecu-desktop-quick`, `src/ui/desktop-quick/qml`, `macdeployqt -qmldir`, and `windeployqt --qmldir`
- Produces: zipped macOS and Windows OmniHaste foundation packages; PR artifacts proving QML, Quick, Controls, Basic style, and Layouts deployment

- [ ] **Step 1: Record the missing package entry-point failure**

On macOS, run:

```bash
scripts/package-desktop-quick-macos.sh /private/tmp/omnihaste-desktop-quick.zip plan-1
```

Expected: FAIL because the script does not exist. Keep the existing Widgets
packaging scripts unchanged.

- [ ] **Step 2: Add the macOS verification packager**

Create executable `scripts/package-desktop-quick-macos.sh`:

```bash
#!/usr/bin/env bash
# Assemble OmniHaste.app from //:fastecu-desktop-quick and verify QML deployment.
# Usage: scripts/package-desktop-quick-macos.sh <output-zip> [version]
set -euo pipefail

out_zip=${1:?usage: package-desktop-quick-macos.sh <output-zip> [version]}
version=${2:-dev}
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$out_zip" in
  /*) ;;
  *) out_zip="$repo_root/$out_zip" ;;
esac
cd "$repo_root"

bazel build --config=release //:fastecu-desktop-quick
bin=$(bazel cquery --config=release --output=files //:fastecu-desktop-quick 2>/dev/null | head -n1)
if [ ! -x "$bin" ]; then
  echo "bazel binary not found or not executable: $bin" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
app="$work/OmniHaste.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/OmniHaste"
chmod u+w,+x "$app/Contents/MacOS/OmniHaste"

cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>OmniHaste</string>
    <key>CFBundleIdentifier</key><string>com.omnihaste.desktop</string>
    <key>CFBundleName</key><string>OmniHaste</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${version}</string>
    <key>CFBundleVersion</key><string>${version}</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
PLIST

macdeployqt "$app" -qmldir="$repo_root/src/ui/desktop-quick/qml"
required=(
  "Contents/Frameworks/QtQml.framework"
  "Contents/Frameworks/QtQuick.framework"
  "Contents/Frameworks/QtQuickControls2.framework"
  "Contents/Frameworks/QtQuickLayouts.framework"
  "Contents/Resources/qml/QtQuick/Controls/qmldir"
  "Contents/Resources/qml/QtQuick/Controls/Basic/qmldir"
)
for relative in "${required[@]}"; do
  [ -e "$app/$relative" ] || { echo "macdeployqt omitted $relative" >&2; exit 1; }
done

rm -f "$out_zip"
( cd "$work" && zip -r -y "$out_zip" OmniHaste.app )
echo "wrote $out_zip"
```

```bash
chmod +x scripts/package-desktop-quick-macos.sh
```

- [ ] **Step 3: Run and inspect the macOS package**

```bash
scripts/package-desktop-quick-macos.sh /private/tmp/omnihaste-desktop-quick.zip plan-1
unzip -l /private/tmp/omnihaste-desktop-quick.zip | \
  rg 'QtQml|QtQuick|QtQuick/Controls|OmniHaste$'
```

Expected: exit 0 and matching executable, frameworks, Controls, and Basic style
entries. If Qt 6.8.3 deploys them to another stable bundle location, update the
asserted paths to observed equivalents; do not delete the assertions.

- [ ] **Step 4: Add the Windows verification packager**

Create `scripts/package-desktop-quick-windows.ps1`:

```powershell
# Package desktop-quick and verify QtQuick QML deployment.
# Usage: scripts/package-desktop-quick-windows.ps1 -OutZip <path>
param(
  [Parameter(Mandatory = $true)][string]$OutZip
)
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot
if (-not [System.IO.Path]::IsPathRooted($OutZip)) {
  $OutZip = Join-Path $repoRoot $OutZip
}

bazel build --config=release //:fastecu-desktop-quick
if ($LASTEXITCODE -ne 0) { throw "bazel build failed (exit $LASTEXITCODE)" }
$bin = (bazel cquery --config=release --output=files //:fastecu-desktop-quick 2>$null | Select-Object -First 1)
if (-not (Test-Path $bin)) { throw "bazel binary not found: $bin" }

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("omnihaste-" + [guid]::NewGuid().ToString("N"))
$dist = Join-Path $work "OmniHaste"
try {
  New-Item -ItemType Directory -Force $dist | Out-Null
  $exe = Join-Path $dist "OmniHaste.exe"
  Copy-Item $bin $exe
  $qmlDir = Join-Path $repoRoot "src/ui/desktop-quick/qml"
  windeployqt --qmldir $qmlDir $exe
  if ($LASTEXITCODE -ne 0) { throw "windeployqt failed (exit $LASTEXITCODE)" }

  $required = @(
    "Qt6Qml.dll",
    "Qt6Quick.dll",
    "Qt6QuickControls2.dll",
    "Qt6QuickLayouts.dll",
    "qml/QtQuick/Controls/qmldir",
    "qml/QtQuick/Controls/Basic/qmldir"
  )
  foreach ($relative in $required) {
    if (-not (Test-Path (Join-Path $dist $relative))) {
      throw "windeployqt omitted $relative"
    }
  }

  $outDir = Split-Path -Parent $OutZip
  if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Force $outDir | Out-Null
  }
  if (Test-Path $OutZip) { Remove-Item $OutZip }
  Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $OutZip
  Write-Host "wrote $OutZip"
}
finally {
  if (Test-Path $work) { Remove-Item -Recurse -Force $work }
}
```

- [ ] **Step 5: Add macOS PR verification**

After the existing macOS FastECU package upload in `.github/workflows/pr.yml`,
add:

```yaml
      - name: Package desktop-quick macOS from Bazel (verify only)
        if: runner.os == 'macOS'
        run: scripts/package-desktop-quick-macos.sh bazel-pkg-desktop-quick-macos.zip ${{ github.sha }}

      - name: Upload desktop-quick macOS package (verify only)
        if: runner.os == 'macOS'
        uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a # v7.0.1
        with:
          name: omnihaste-desktop-quick-${{ github.event.pull_request.number || github.sha }}-macos
          path: bazel-pkg-desktop-quick-macos.zip
          retention-days: 14
```

- [ ] **Step 6: Add Windows PR verification**

After the existing Windows FastECU package upload, add:

```yaml
      - name: Package desktop-quick Windows from Bazel (verify only)
        if: runner.os == 'Windows'
        shell: pwsh
        run: scripts/package-desktop-quick-windows.ps1 -OutZip bazel-pkg-desktop-quick-windows.zip

      - name: Upload desktop-quick Windows package (verify only)
        if: runner.os == 'Windows'
        uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a # v7.0.1
        with:
          name: omnihaste-desktop-quick-${{ github.event.pull_request.number || github.sha }}-windows
          path: bazel-pkg-desktop-quick-windows.zip
          retention-days: 14
```

Do not modify `.github/workflows/release.yml` in this plan.

- [ ] **Step 7: Run the complete local foundation gate**

On macOS:

```bash
bazel build --config=release //:fastecu //:fastecu-desktop-quick
bazel test --config=release //src/ui/desktop-quick:test_application
QT_QPA_PLATFORM=offscreen bazel run --config=release \
  //:fastecu-desktop-quick -- --smoke-test
scripts/package-desktop-quick-macos.sh \
  /private/tmp/omnihaste-desktop-quick.zip plan-1
prek run --all-files
git diff --check
bazel test -k --config=release //...
```

Expected: all non-quarantined targets PASS. On Linux omit the package command.
On Windows replace it with:

```powershell
scripts/package-desktop-quick-windows.ps1 `
  -OutZip "$env:TEMP/omnihaste-desktop-quick.zip"
```

A platform package failure is a foundation blocker, not deferred dashboard
work.

- [ ] **Step 8: Commit package verification**

```bash
git add .github/workflows/pr.yml scripts/package-desktop-quick-macos.sh \
  scripts/package-desktop-quick-windows.ps1
git commit -m "build: verify desktop-quick packages"
```

---

## Completion checkpoint

Before planning `.ohd`, confirm:

- `//:fastecu` still builds the Widgets app.
- `//:fastecu-desktop-quick` builds and its smoke mode exits 0 on all CI hosts.
- The test proves embedded QtQuick, Controls, Layouts, and shell resources load.
- `QT_DEPS` is unchanged and the new graph has no platform/backend/Widgets edge.
- macdeployqt and windeployqt packages include QML, Quick, Controls, Basic, and Layouts runtime files.
- Existing FastECU PR packages still run separately.
- `prek run --all-files`, `git diff --check`, and `bazel test -k --config=release //...` pass.

Record the macOS and Windows package job URLs in the implementation handoff.
Only then write the portable `.ohd` model/codec/import/persistence plan.

## Deployment references

- [Qt for macOS deployment](https://doc.qt.io/qt-6/macos-deployment.html) — `macdeployqt -qmldir` scans QML imports.
- [Qt for Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html) — `windeployqt --qmldir` runs `qmlimportscanner` and copies detected QML dependencies.
