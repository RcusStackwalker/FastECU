# Package FastECU for Windows from Bazel outputs: gather FastECU.exe, Qt DLLs
# (windeployqt), the OpenSSL libcrypto DLL, and the 32-bit J2534 bridge helper.
# Usage: scripts/package-windows.ps1 -OutZip <path> [-Version <v>]
# Requires: bazel, windeployqt (MSVC Qt) on PATH; env OPENSSL_ROOT + OPENSSL_CRYPTO_DLL.
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

bazel build --config=release //:fastecu //serial_port/j2534_bridge_host:j2534_bridge_host_x86
$bin = (bazel cquery --config=release --output=files //:fastecu 2>$null | Select-Object -First 1)
if (-not (Test-Path $bin)) { throw "bazel binary not found: $bin" }
$bridge = (bazel cquery --config=release --output=files //serial_port/j2534_bridge_host:j2534_bridge_host_x86 2>$null | Select-Object -First 1)
if (-not (Test-Path $bridge)) { throw "bridge helper not found: $bridge" }

if (-not $env:OPENSSL_ROOT) { throw "OPENSSL_ROOT not set" }
if (-not $env:OPENSSL_CRYPTO_DLL) { throw "OPENSSL_CRYPTO_DLL not set" }
$cryptoDll = Join-Path $env:OPENSSL_ROOT "bin/$env:OPENSSL_CRYPTO_DLL"
if (-not (Test-Path $cryptoDll)) { throw "OpenSSL DLL not found: $cryptoDll" }

$dist = Join-Path $repoRoot "dist"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null

Copy-Item $bin (Join-Path $dist "FastECU.exe")
Copy-Item $bridge (Join-Path $dist "j2534_bridge_host.exe")
Copy-Item $cryptoDll $dist

windeployqt (Join-Path $dist "FastECU.exe")

if (Test-Path $OutZip) { Remove-Item $OutZip }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $OutZip
Write-Host "wrote $OutZip"
