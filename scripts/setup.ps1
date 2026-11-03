$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Split-Path -Parent $root
Set-Location $proj

if (-not (Test-Path "third_party\whisper.cpp\CMakeLists.txt")) {
    New-Item -ItemType Directory -Force -Path third_party | Out-Null
    git clone --depth 1 https://github.com/ggml-org/whisper.cpp.git third_party\whisper.cpp
}

Write-Output "third_party ready"
