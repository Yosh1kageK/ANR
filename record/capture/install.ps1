# Installs nr-record's capture DLL into a game that has OptiScaler (for example from the ANR FG full package).
# Copies nr-record into OptiScaler\custom, sets FGNvngxReplacement=nr-record (frame generation) in OptiScaler.ini (the
# original is kept in OptiScaler\nr_record_backup), and forwards frame generation to ANR FG where it is installed, else
# to NVIDIA's DLSS-G. The SR and RR recorder is left for the user to pick in OptiScaler's menu.
param([string]$Game)
$ErrorActionPreference = "Stop"
if (-not $Game) { $Game = Read-Host "The folder of the game's .exe" }
$Game = $Game.Trim('"')
$ini = Join-Path $Game "OptiScaler.ini"
if (-not (Test-Path (Join-Path $Game "OptiScaler")) -or -not (Test-Path $ini)) {
    throw "No OptiScaler in ${Game}: install the ANR FG full package (the one with OptiScaler) there first. Nothing changed."
}
$text = [IO.File]::ReadAllText($ini)
if ($text -notmatch '(?m)^FGNvngxReplacement=') { throw "$ini has no FGNvngxReplacement line. Nothing changed." }
$running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith((Resolve-Path $Game).Path, [StringComparison]::OrdinalIgnoreCase) })
if ($running) { throw "close the game first ($($running[0].ProcessName) is running); nothing changed" }
$src = Join-Path $PSScriptRoot "capture\OptiScaler\custom\nr-record"
$dest = Join-Path $Game "OptiScaler\custom\nr-record"
$backup = Join-Path $Game "OptiScaler\nr_record_backup"
New-Item -ItemType Directory -Force $backup | Out-Null
if (-not (Test-Path (Join-Path $backup "OptiScaler.ini"))) { Copy-Item $ini $backup }  # the original, from before the first install
$settings = Join-Path $dest "dlssg\nr_record.ini"
$kept = $null
if (Test-Path $settings) { $kept = [IO.File]::ReadAllText($settings) }
$sr_settings = Join-Path $dest "dlss\nr_record.ini"
$sr_kept = $null
if (Test-Path $sr_settings) { $sr_kept = [IO.File]::ReadAllText($sr_settings) }
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
Copy-Item -Recurse $src $dest
if ($kept) {
    [IO.File]::WriteAllText($settings, $kept)
    $forward = "(kept your nr_record.ini)"
} else {
    $anr = Join-Path $Game "OptiScaler\custom\Apex Neural Rendering FG\dlssg\nvngx_dlssg.dll"
    $value = if (Test-Path $anr) { '..\..\Apex Neural Rendering FG\dlssg\nvngx_dlssg.dll' } else { 'nvidia' }
    $forward = if (Test-Path $anr) { "ANR FG" } else { "NVIDIA's DLSS-G" }
    $t = [IO.File]::ReadAllText($settings) -replace '(?m)^forward =[^\r\n]*', "forward = $value"
    [IO.File]::WriteAllText($settings, $t)
}
if ($sr_kept) { [IO.File]::WriteAllText($sr_settings, $sr_kept) }
[IO.File]::WriteAllText($ini, ($text -replace '(?m)^FGNvngxReplacement=[^\r\n]*', 'FGNvngxReplacement=nr-record'))
Write-Host "nr-record installed in ${Game}:"
Write-Host "  OptiScaler\custom\nr-record (the capture DLLs), OptiScaler.ini: FGNvngxReplacement=nr-record"
Write-Host "  to record SR or RR: pick nr-record as the upscaler in OptiScaler's menu, then F6; pick your upscaler again after"
Write-Host "  when not recording, frames come from: $forward"
Write-Host "  the original OptiScaler.ini: OptiScaler\nr_record_backup (uninstall.cmd restores it)"
Write-Host "Windows needs no launch option."
