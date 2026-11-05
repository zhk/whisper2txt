param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string[]]$Audio,
    [string]$Terms = "",
    [string]$Ref = "",
    [string]$Out = "ab",
    [string[]]$Configs = @("fast", "fast_noprep", "high", "high_vad", "high_vad_terms")
)
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Split-Path -Parent $root
Set-Location $proj

if (-not (Test-Path $Exe)) { throw "exe not found: $Exe" }
$Exe = (Resolve-Path $Exe).Path

$cfgFlags = [ordered]@{
    fast           = @("--quality", "fast")
    fast_noprep    = @("--quality", "fast", "--no-preproc")
    high           = @("--quality", "high")
    high_vad       = @("--quality", "high", "--vad")
    high_vad_terms = @("--quality", "high", "--vad", "--terms", $Terms)
}

if ($Terms -eq "") {
    $Configs = @($Configs | Where-Object { $_ -ne "high_vad_terms" })
    Write-Output "no -Terms given, skip config: high_vad_terms"
}

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$repo = Join-Path $Out "repo"
New-Item -ItemType Directory -Force -Path $repo | Out-Null

$werScript = Join-Path $proj "tools\wer.py"
$rows = @()

foreach ($a in $Audio) {
    if (-not (Test-Path $a)) { Write-Output "skip missing audio: $a"; continue }
    $audioPath = (Resolve-Path $a).Path
    $name = [IO.Path]::GetFileNameWithoutExtension($audioPath)

    $refFile = ""
    if ($Ref -ne "") {
        if (Test-Path $Ref -PathType Container) {
            $cand = Join-Path $Ref ("ref_" + $name + ".txt")
            if (Test-Path $cand) { $refFile = (Resolve-Path $cand).Path }
        } elseif (Test-Path $Ref) {
            $refFile = (Resolve-Path $Ref).Path
        }
    }

    foreach ($cfg in $Configs) {
        if (-not $cfgFlags.Contains($cfg)) { Write-Output "unknown config: $cfg"; continue }
        $flags = @($cfgFlags[$cfg])

        $work = Join-Path $Out ("work_" + $cfg)
        New-Item -ItemType Directory -Force -Path $work | Out-Null
        $copy = Join-Path $work ([IO.Path]::GetFileName($audioPath))
        Copy-Item -LiteralPath $audioPath -Destination $copy -Force

        $log = Join-Path $work "run.log"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        Start-Process -FilePath $Exe -ArgumentList (@("--cli", "--overwrite") + $flags + @($copy)) `
            -WorkingDirectory $work -RedirectStandardOutput $log -RedirectStandardError (Join-Path $work "err.log") `
            -NoNewWindow -Wait | Out-Null
        $sw.Stop()

        $txtOut = [IO.Path]::ChangeExtension($copy, ".txt")
        $saved = Join-Path $Out ($name + "_" + $cfg + ".txt")
        if (Test-Path $txtOut) {
            Copy-Item -LiteralPath $txtOut -Destination $saved -Force
            Copy-Item -LiteralPath $txtOut -Destination (Join-Path $repo ($name + "__" + $cfg + ".txt")) -Force
        }

        $segs = ""
        $speed = ""
        if (Test-Path $log) {
            $content = Get-Content $log -Encoding UTF8 -Raw
            $ms = [regex]::Match($content, "\(([0-9]+) [^(,]{1,6},")
            if ($ms.Success) { $segs = $ms.Groups[1].Value }
            $msp = [regex]::Match($content, "([0-9.]+)x\s")
            if ($msp.Success) { $speed = $msp.Groups[1].Value }
        }

        $errRate = ""
        if ($refFile -ne "" -and (Test-Path $saved)) {
            $w = (& python $werScript $refFile $saved --diff 0 2>&1 | Out-String)
            $m2 = [regex]::Match($w, "ERROR_RATE:\s*([0-9.]+)%")
            if ($m2.Success) { $errRate = $m2.Groups[1].Value + "%" } else { $errRate = "n/a" }
        }

        $rows += [pscustomobject]@{
            audio = $name; config = $cfg; wall_s = [Math]::Round($sw.Elapsed.TotalSeconds, 1)
            segments = $segs; speed_x = $speed; err_rate = $errRate
        }
        Write-Output ("{0} [{1}] {2}s  segments={3} speed={4}x err={5}" -f $name, $cfg,
            $sw.Elapsed.TotalSeconds.ToString("F1"), $segs, $speed, $errRate)
    }
}

Write-Output ""
$rows | Format-Table -AutoSize | Out-String -Width 160 | Write-Output
Write-Output ("outputs in " + $Out + "   (repo\\ holds side-by-side results per config)")
if ($Ref -ne "") { Write-Output ("reference: " + $Ref) }
