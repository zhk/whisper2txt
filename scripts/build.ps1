$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Split-Path -Parent $root

Set-Location $proj

if (-not (Test-Path "third_party\whisper.cpp\CMakeLists.txt")) {
    & (Join-Path $root "setup.ps1")
}

if (-not (Test-Path "build")) {
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
}
cmake --build build --config Release

$dist = Join-Path $proj "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
Copy-Item (Join-Path $proj "build\Release\whisper2txt.exe") $dist -Force

$vcomp = Get-ChildItem "C:\Windows\System32\vcomp140.dll" -ErrorAction SilentlyContinue
if ($vcomp) {
    Copy-Item $vcomp.FullName $dist -Force
}

Get-ChildItem $dist | Select-Object Name, Length, LastWriteTime
