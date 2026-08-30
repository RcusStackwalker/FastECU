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
