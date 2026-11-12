# Removes nr-record's capture DLL from a game: restores the OptiScaler.ini install.ps1 backed up and deletes
# OptiScaler\custom\nr-record; OptiScaler stays.
param([string]$Game)
$ErrorActionPreference = "Stop"
if (-not $Game) { $Game = Read-Host "The folder of the game's .exe" }
$Game = $Game.Trim('"')
$dest = Join-Path $Game "OptiScaler\custom\nr-record"
$backup = Join-Path $Game "OptiScaler\nr_record_backup"
if (-not (Test-Path $dest) -and -not (Test-Path $backup)) { throw "nr-record is not installed in $Game." }
if (Test-Path (Join-Path $backup "OptiScaler.ini")) {
    Copy-Item -Force (Join-Path $backup "OptiScaler.ini") (Join-Path $Game "OptiScaler.ini")
    Write-Host "restored OptiScaler.ini"
}
foreach ($p in $dest, $backup) { if (Test-Path $p) { Remove-Item -Recurse -Force $p } }
Write-Host "nr-record removed from $Game (recordings in %LOCALAPPDATA%\nr-record stay)"
