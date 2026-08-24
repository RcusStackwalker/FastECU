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
