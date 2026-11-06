param(
    [string]$Source = "speechtotext2.png",
    [string]$Out = "src\app.ico",
    [int[]]$Sizes = @(16, 24, 32, 48, 64, 128, 256)
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Split-Path -Parent $root
$srcPath = if ([IO.Path]::IsPathRooted($Source)) { $Source } else { Join-Path $proj $Source }
$outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $proj $Out }
if (-not (Test-Path -LiteralPath $srcPath)) { throw "source image not found: $srcPath" }

function New-Scaled([System.Drawing.Image]$img, [int]$size)
{
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try
    {
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.DrawImage($img, 0, 0, $size, $size)
    }
    finally
    {
        $g.Dispose()
    }
    return $bmp
}

function Get-PngBytes([System.Drawing.Image]$orig, [int]$target)
{
    $cur = $orig
    try
    {
        while ($cur.Width -ge ($target * 2))
        {
            $half = [Math]::Max($target, [int][Math]::Ceiling($cur.Width / 2))
            $next = New-Scaled $cur $half
            if ($cur -ne $orig) { $cur.Dispose() }
            $cur = $next
        }
        $final = if ($cur.Width -eq $target) { $cur } else { New-Scaled $cur $target }
        $ms = New-Object System.IO.MemoryStream
        try
        {
            $final.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
            return ,$ms.ToArray()
        }
        finally
        {
            $ms.Dispose()
            if ($final -ne $cur) { $final.Dispose() }
        }
    }
    finally
    {
        if ($cur -ne $orig) { $cur.Dispose() }
    }
}

$orig = [System.Drawing.Bitmap]::FromFile($srcPath)
try
{
    $pngs = [System.Collections.Generic.List[object]]::new()
    foreach ($s in ($Sizes | Sort-Object))
    {
        $bytes = Get-PngBytes $orig ([int]$s)
        $pngs.Add([pscustomobject]@{ Size = [int]$s; Bytes = $bytes })
        Write-Host ("embedded {0}x{0} png={1} bytes" -f $s, $bytes.Length)
    }

    $n = $pngs.Count
    $offset = 6 + (16 * $n)
    $dir = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($dir)
    $bw.Write([UInt16]0)
    $bw.Write([UInt16]1)
    $bw.Write([UInt16]$n)
    foreach ($e in $pngs)
    {
        $k = [int]$e.Size
        $dim = if ($k -ge 256) { [Byte]0 } else { [Byte]$k }
        $bw.Write($dim)
        $bw.Write($dim)
        $bw.Write([Byte]0)
        $bw.Write([Byte]0)
        $bw.Write([UInt16]1)
        $bw.Write([UInt16]32)
        $bw.Write([UInt32]$e.Bytes.Length)
        $bw.Write([UInt32]$offset)
        $offset += $e.Bytes.Length
    }
    $bw.Flush()

    $outDir = Split-Path -Parent $outPath
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
    $fs = [IO.File]::Create($outPath)
    try
    {
        $dir.WriteTo($fs)
        foreach ($e in $pngs) { $fs.Write($e.Bytes, 0, $e.Bytes.Length) }
    }
    finally
    {
        $fs.Dispose()
        $dir.Dispose()
    }
    $info = Get-Item -LiteralPath $outPath
    Write-Host ("wrote {0} ({1} bytes, {2} images)" -f $info.FullName, $info.Length, $n)
}
finally
{
    $orig.Dispose()
}
