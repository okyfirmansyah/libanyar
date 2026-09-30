# =============================================================================
# LibAnyar - Development Environment Setup (Windows 10/11, MSVC + vcpkg)
# =============================================================================
# Run from a PowerShell prompt at the repo root:
#   powershell -ExecutionPolicy Bypass -File scripts\setup-windows.ps1
#
# Installs / builds everything needed to build LibAnyar on Windows:
#   - vcpkg packages (x64-windows): Boost (fiber, context, beast, url, ...),
#     OpenSSL, SOCI (sqlite3 + postgresql), nlohmann-json, WebView2 SDK headers
#   - LibAsyik 1.8.1 (first release with MSVC support), built from source and
#     installed into <DepsDir>\libasyik
#
# Prerequisites (not installed by this script):
#   - Visual Studio 2022 (or Build Tools) with "Desktop development with C++"
#   - CMake >= 3.16, Git
#   - vcpkg (https://vcpkg.io) - pass -VcpkgRoot or set VCPKG_ROOT
#   - Node.js 20+ (only for building frontends / js-bridge)
#
# After it finishes, configure LibAnyar with the line it prints, e.g.:
#   cmake -B build -G "Visual Studio 17 2022" -A x64 `
#     -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
#     -DCMAKE_PREFIX_PATH=<DepsDir>\libasyik -DANYAR_BUILD_TESTS=ON
#   cmake --build build --config Release
# =============================================================================

param(
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\vcpkg" }),
    [string]$DepsDir   = (Join-Path (Split-Path -Parent $PSScriptRoot) "build-deps"),
    [string]$LibAsyikVersion = "1.8.1",
    [string]$Generator = "Visual Studio 17 2022",
    [switch]$SkipVcpkg
)

$ErrorActionPreference = "Stop"
$Triplet = "x64-windows"

function Step($msg) { Write-Host "`n[STEP] $msg`n" -ForegroundColor Green }

$vcpkg = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path $vcpkg)) {
    throw "vcpkg not found at $vcpkg - install vcpkg or pass -VcpkgRoot"
}
$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"

# --- 1. vcpkg packages ------------------------------------------------------
if (-not $SkipVcpkg) {
    Step "Installing vcpkg packages ($Triplet)"
    & $vcpkg install --triplet $Triplet `
        boost-fiber boost-context boost-beast boost-asio boost-url `
        boost-date-time boost-atomic boost-any boost-convert boost-regex `
        boost-algorithm boost-optional `
        openssl "soci[postgresql,sqlite3]" sqlite3 `
        nlohmann-json webview2
    if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed" }
}

# --- 2. LibAsyik ------------------------------------------------------------
Step "Building LibAsyik $LibAsyikVersion"
New-Item -ItemType Directory -Force $DepsDir | Out-Null
$src     = Join-Path $DepsDir "libasyik-src"
$build   = Join-Path $DepsDir "libasyik-build"
$prefix  = Join-Path $DepsDir "libasyik"

if (-not (Test-Path (Join-Path $src ".git"))) {
    git clone --branch $LibAsyikVersion --depth 1 --recurse-submodules `
        --shallow-submodules https://github.com/okyfirmansyah/libasyik $src
    if ($LASTEXITCODE -ne 0) { throw "git clone libasyik failed" }
} else {
    git -C $src fetch --depth 1 origin tag $LibAsyikVersion
    git -C $src checkout --force $LibAsyikVersion
    git -C $src submodule update --init --recursive
}

cmake -S $src -B $build -G $Generator -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DCMAKE_INSTALL_PREFIX=$prefix" `
    -DCMAKE_DEBUG_POSTFIX=d `
    -DLIBASYIK_ENABLE_SOCI=ON
if ($LASTEXITCODE -ne 0) { throw "LibAsyik configure failed" }

foreach ($cfg in "Release", "Debug") {
    cmake --build $build --config $cfg --target libasyik --parallel
    if ($LASTEXITCODE -ne 0) { throw "LibAsyik build ($cfg) failed" }
    cmake --install $build --config $cfg
    if ($LASTEXITCODE -ne 0) { throw "LibAsyik install ($cfg) failed" }
}

# --- Done -------------------------------------------------------------------
Step "Done. Configure LibAnyar with:"
Write-Host "  cmake -B build -G `"$Generator`" -A x64 ``"
Write-Host "    -DCMAKE_TOOLCHAIN_FILE=$toolchain ``"
Write-Host "    -DCMAKE_PREFIX_PATH=$prefix -DANYAR_BUILD_TESTS=ON"
Write-Host "  cmake --build build --config Release"
