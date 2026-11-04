param([switch]$Gpu, [switch]$Vk, [switch]$Static)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Split-Path -Parent $root

Set-Location $proj

if (-not (Test-Path "third_party\whisper.cpp\CMakeLists.txt")) {
    & (Join-Path $root "setup.ps1")
}

$bt = if ($Gpu -or $Vk) { "build_gpu" } else { "build" }
if ($Gpu) {
    $cuda = Get-ChildItem "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\" -Directory |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $cuda) { throw "CUDA toolkit not found under C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA" }
    $env:CUDA_PATH = $cuda.FullName
    $vname = "CUDA_PATH_" + ($cuda.Name.ToUpper() -replace '\.','_')
    Set-Item -Path "env:$vname" -Value $cuda.FullName
}
if (-not (Test-Path "$bt\CMakeCache.txt")) {
    $cargs = @("-S", ".", "-B", $bt, "-G", "Visual Studio 17 2022", "-A", "x64")
    if (-not $Static) { $cargs += @("-DWHISPER2TXT_SHARED=ON") }
    if ($Gpu) { $cargs += @("-DGGML_CUDA=1") }
    if ($Vk) { $cargs += @("-DGGML_VULKAN=1") }
    if (-not $Static) { $cargs += @("-DGGML_NATIVE=OFF", "-DGGML_CPU_ALL_VARIANTS=ON") }
    cmake @cargs
    if ($LASTEXITCODE -ne 0) { throw "configure failed (exit $LASTEXITCODE)" }
}
cmake --build $bt --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

$dist = Join-Path $proj "dist"
Get-Process whisper2txt -ErrorAction SilentlyContinue | ForEach-Object {
    try { if ($_.Path -and $_.Path.StartsWith($dist, [StringComparison]::OrdinalIgnoreCase)) { Stop-Process -Id $_.Id -Force } } catch { }
}
Start-Sleep -Milliseconds 800
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dist | Out-Null

$bin = Join-Path $proj "$bt\Release"
Copy-Item (Join-Path $bin "whisper2txt.exe") $dist -Force
$dllsrc = @($bin, (Join-Path $proj "$bt\bin\Release")) | Where-Object { Test-Path $_ }
foreach ($d in $dllsrc) {
    Get-ChildItem $d -Filter "*.dll" | Where-Object { $_.Name -ne "parakeet.dll" } | ForEach-Object { Copy-Item $_.FullName $dist -Force }
}

if ($Gpu) {
    $cudabin = Join-Path $env:CUDA_PATH "bin\x64"
    if (-not (Test-Path $cudabin)) { $cudabin = Join-Path $env:CUDA_PATH "bin" }
    foreach ($p in @("cublas64_*.dll", "cublasLt64_*.dll")) {
        Get-ChildItem $cudabin -Filter $p | ForEach-Object { Copy-Item $_.FullName $dist -Force }
    }
}

Copy-Item "C:\Windows\System32\vcomp140.dll" $dist -Force

Get-ChildItem $dist | Sort-Object Name | Select-Object Name, Length
