# Package FastECU for Windows from Bazel outputs: gather FastECU.exe, Qt DLLs
# (windeployqt), and the 32-bit J2534 bridge helper.
# Usage: scripts/package-windows.ps1 -OutZip <path> [-Version <v>]
# Requires: bazel, windeployqt (MSVC Qt) on PATH.
param(
  [Parameter(Mandatory = $true)][string]$OutZip,
  [string]$Version = "dev"
)
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot
if (-not [System.IO.Path]::IsPathRooted($OutZip)) {
  $OutZip = Join-Path $repoRoot $OutZip
}

bazel build --config=release //:fastecu //src/platform/desktop/windows/j2534/j2534_bridge_host:j2534_bridge_host_x86
if ($LASTEXITCODE -ne 0) { throw "bazel build failed (exit $LASTEXITCODE)" }
$bin = (bazel cquery --config=release --output=files //:fastecu 2>$null | Select-Object -First 1)
if (-not (Test-Path $bin)) { throw "bazel binary not found: $bin" }
$bridge = (bazel cquery --config=release --output=files //src/platform/desktop/windows/j2534/j2534_bridge_host:j2534_bridge_host_x86 2>$null | Select-Object -First 1)
if (-not (Test-Path $bridge)) { throw "bridge helper not found: $bridge" }

$dist = Join-Path $repoRoot "dist"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null

Copy-Item $bin (Join-Path $dist "FastECU.exe")
Copy-Item $bridge (Join-Path $dist "j2534_bridge_host.exe")

windeployqt (Join-Path $dist "FastECU.exe")

if (-not (Test-Path (Join-Path $dist "Qt6Core.dll"))) { throw "windeployqt did not stage Qt6Core.dll" }

$outDir = Split-Path -Parent $OutZip
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
if (Test-Path $OutZip) { Remove-Item $OutZip }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $OutZip
Write-Host "wrote $OutZip"
