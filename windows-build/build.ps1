# Builds Kettle Edition on Windows. See README.md in this folder for what to install first.
#   .\windows-build\build.ps1 -Target gui    -> build-gui\qbittorrent.exe      (the desktop app)
#   .\windows-build\build.ps1 -Target nox    -> build-nox\qbittorrent-nox.exe  (headless, for the tests)
# Add -Clean to start from scratch.
#
# -ToolsRoot is the folder that holds Qt\, vcpkg\ and deps\ (default: the folder above the repository).

param(
    [ValidateSet("gui", "nox")] [string] $Target = "gui",
    [switch] $Clean,
    [string] $ToolsRoot = (Split-Path $PSScriptRoot -Parent | Split-Path -Parent),
    [string] $BuildDir = ""
)

$ErrorActionPreference = "Stop"
$source = Split-Path $PSScriptRoot -Parent
if (-not $BuildDir) { $BuildDir = Join-Path $source "build-$Target" }
$qt = Join-Path $ToolsRoot "Qt\6.8.3\msvc2022_64"
$vcpkg = Join-Path $ToolsRoot "vcpkg"
$deps = Join-Path $ToolsRoot "deps\installed"

foreach ($path in $qt, $vcpkg, $deps) {
    if (-not (Test-Path $path)) { throw "Missing: $path (see windows-build\README.md)" }
}

# Visual Studio C++ compiler environment (x64)
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:PATH = "C:\Program Files\Git\cmd;$qt\bin;$env:PATH"

if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -Recurse -Force $BuildDir }

$gui = if ($Target -eq "gui") { "ON" } else { "OFF" }
$testing = if ($Target -eq "nox") { "ON" } else { "OFF" }

cmake -S $source -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    -DVCPKG_INSTALLED_DIR="$deps" `
    -DVCPKG_MANIFEST_MODE=OFF `
    -DCMAKE_PREFIX_PATH="$qt" `
    -DGUI="$gui" `
    -DTESTING="$testing"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

cmake --build $BuildDir --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$exe = if ($Target -eq "gui") { "$BuildDir\qbittorrent.exe" } else { "$BuildDir\qbittorrent-nox.exe" }
& "$qt\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw $exe | Out-Null
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

Write-Host "Built $exe"
