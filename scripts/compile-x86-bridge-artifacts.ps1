#!/usr/bin/env pwsh
# Builds the 32-bit (x86) J2534 bridge artifacts directly via cl.exe, bypassing
# Bazel entirely: this repo's Bazel setup has no registered x86 Windows C++
# toolchain (confirmed by a real CI failure -- "No matching toolchains found
# for @@bazel_tools//tools/cpp:toolchain_type" -- Bazel's built-in Windows
# toolchain autodetection only covers the exact host architecture), so the
# helper/fake-DLL/fixture are compiled the same way FastECU.exe itself already
# is: by invoking the compiler directly, not through Bazel.
#
# Must be run from the repository root (all source paths below are relative
# to it). Requires a Visual Studio installation with the C++ x86/x64 build
# tools component (already present on GitHub's windows-latest runner image).
#
# Usage: scripts/compile-x86-bridge-artifacts.ps1 [-OutDir <dir>]

param(
    [string]$OutDir = "x86-bridge-artifacts"
)

$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere"
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with the VC x86/x64 build tools component found"
}
$vcvarsall = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvarsall)) {
    throw "vcvarsall.bat not found at $vcvarsall"
}
Write-Host "Using Visual Studio at $vsPath"

# vcvarsall.bat only affects the environment of the cmd.exe process that runs
# it -- run it via cmd, dump the resulting environment with `set`, and import
# each variable into this PowerShell session so the x86-targeting cl.exe/
# link.exe (and their INCLUDE/LIB paths) end up on PATH here.
$envDump = cmd /c "`"$vcvarsall`" x86 >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
    throw "vcvarsall.bat x86 failed (exit $LASTEXITCODE)"
}
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2])
    }
}

$clPath = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
if (-not $clPath) {
    throw "cl.exe not found on PATH after importing the x86 dev environment"
}
Write-Host "Using cl.exe at $clPath"

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$commonFlags = @("/nologo", "/EHsc", "/MT", "/O2", "/std:c++20", "/Zc:__cplusplus", "/I", "serial_port")

Write-Host "Building j2534_bridge_host.exe (x86)..."
& cl.exe @commonFlags `
    "serial_port\j2534_bridge_host\main.cpp" `
    "serial_port\j2534_bridge_protocol.cpp" `
    "/Fe:$OutDir\j2534_bridge_host.exe" `
    "/Fo:$OutDir\" `
    /link /MACHINE:X86
if ($LASTEXITCODE -ne 0) { throw "j2534_bridge_host.exe build failed (exit $LASTEXITCODE)" }

Write-Host "Building fake_j2534_dll.dll (x86)..."
& cl.exe @commonFlags /LD `
    "tests\fake_j2534_dll.cpp" `
    "serial_port\j2534_bridge_protocol.cpp" `
    "/Fe:$OutDir\fake_j2534_dll.dll" `
    "/Fo:$OutDir\" `
    /link /MACHINE:X86 "/DEF:tests\fake_j2534_dll.def"
if ($LASTEXITCODE -ne 0) { throw "fake_j2534_dll.dll build failed (exit $LASTEXITCODE)" }

Write-Host "Building x86_smoke_test.exe (x86 PE-bitness fixture)..."
& cl.exe /nologo /EHsc /MT /O2 /std:c++20 /Zc:__cplusplus `
    "bazel\platforms\x86_smoke_test.cc" `
    "/Fe:$OutDir\x86_smoke_test.exe" `
    "/Fo:$OutDir\" `
    /link /MACHINE:X86
if ($LASTEXITCODE -ne 0) { throw "x86_smoke_test.exe build failed (exit $LASTEXITCODE)" }

Write-Host "x86 bridge artifacts built successfully in $OutDir"
Get-ChildItem -Path $OutDir | Where-Object { $_.Extension -in ".exe", ".dll" } | ForEach-Object { Write-Host "  $($_.FullName)" }
