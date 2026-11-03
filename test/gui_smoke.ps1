param([string]$Exe, [string]$Audio)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$txt = [System.IO.Path]::ChangeExtension($Audio, ".txt")
$srt = [System.IO.Path]::ChangeExtension($Audio, ".srt")
Remove-Item -LiteralPath $txt, $srt -Force -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Exe -ArgumentList @("--gui", $Audio) -PassThru
Start-Sleep -Seconds 6

$AE = [System.Windows.Automation.AutomationElement]
$scopeChildren = [System.Windows.Automation.TreeScope]::Children
$scopeDesc = [System.Windows.Automation.TreeScope]::Descendants

$condWin = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window)
$all = $AE::RootElement.FindAll($scopeChildren, $condWin)
$win = $null
foreach ($el in $all) {
    if ($el.Current.Name -like "*MP3*TXT*") { $win = $el; break }
}
if ($null -eq $win) { Write-Output "WINDOW_NOT_FOUND"; Stop-Process -Id $p.Id -Force; exit 1 }
Write-Output ("WINDOW_OK len=" + $win.Current.Name.Length)

$startName = [string]::Join('', [char]0x5F00, [char]0x59CB, [char]0x8F6C, [char]0x5199)
$condBtn = New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $startName)
$btn = $win.FindFirst($scopeDesc, $condBtn)
if ($null -eq $btn) { Write-Output "BUTTON_NOT_FOUND"; Stop-Process -Id $p.Id -Force; exit 1 }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Msg {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SendMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
"@

$hBtn = [IntPtr]$btn.Current.NativeWindowHandle
if ($hBtn -eq [IntPtr]::Zero) { Write-Output "BUTTON_NO_HWND"; Stop-Process -Id $p.Id -Force; exit 1 }
[void][Win32Msg]::SendMessageW($hBtn, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
Write-Output "CLICKED_START"

$found = $false
$elapsed = 0
for ($i = 0; $i -lt 120; $i++) {
    Start-Sleep -Milliseconds 1000
    $elapsed = $i + 1
    if (Test-Path -LiteralPath $txt) { $found = $true; break }
}

if ($found) {
    Write-Output ("OUTPUT_CREATED after " + $elapsed + "s")
    Start-Sleep -Seconds 5
} else {
    Write-Output "OUTPUT_TIMEOUT"
}

$condEdit = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, [System.Windows.Automation.ControlType]::Edit)
$edits = $win.FindAll($scopeDesc, $condEdit)
foreach ($e in $edits) {
    try {
        $tp = $e.GetCurrentPattern([System.Windows.Automation.Text.TextPattern]::Pattern)
        $content = $tp.GetText(-1)
        if ($content.Length -gt 20) {
            Write-Output "---LOG---"
            $lines = $content -split "`r`n"
            foreach ($ln in $lines) { Write-Output $ln }
            break
        }
    } catch { }
}

try {
    $wp = $win.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
    $wp.Close()
} catch {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 2
if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id -Force }
Write-Output "DONE"
