#!/usr/bin/env bash
# Assemble FastECU.app from the Bazel-built //:fastecu binary, bundle Qt
# frameworks + the linked OpenSSL dylib via macdeployqt, and zip it.
# Usage: scripts/package-macos.sh <output-zip> [version]
# Requires: bazel, macdeployqt (Qt bin dir) on PATH.
set -euo pipefail

out_zip=${1:?usage: package-macos.sh <output-zip> [version]}
version=${2:-dev}

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "$out_zip" in
  /*) ;;
  *) out_zip="$repo_root/$out_zip" ;;
esac
cd "$repo_root"

bazel build --config=release //:fastecu
bin=$(bazel cquery --config=release --output=files //:fastecu 2>/dev/null | head -n1)
if [ ! -x "$bin" ]; then
  echo "bazel binary not found or not executable: $bin" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
app="$work/FastECU.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/FastECU"
# bazel-bin outputs are read-only; macdeployqt needs to strip/relink in place.
chmod u+w,+x "$app/Contents/MacOS/FastECU"

cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>FastECU</string>
    <key>CFBundleIdentifier</key><string>fi.fastecu.FastECU</string>
    <key>CFBundleName</key><string>FastECU</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${version}</string>
    <key>CFBundleVersion</key><string>${version}</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>LSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
PLIST

macdeployqt "$app"

rm -f "$out_zip"
( cd "$work" && zip -r -y "$out_zip" FastECU.app )
echo "wrote $out_zip"
