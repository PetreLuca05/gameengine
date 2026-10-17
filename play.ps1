# Launch Deadground from a terminal (VS Code: PowerShell). Uses the builds in "openable builds".
#   .\play.ps1                          nanite build, main menu
#   .\play.ps1 -NoNanite                no-nanite build, main menu
#   .\play.ps1 -Map zombieland          straight into a map (no splash / menu)
#   .\play.ps1 -Scene handpose          straight into a scene (hand-pose editor)
#   .\play.ps1 -NoNanite -Map vegetation
#   .\play.ps1 -Dev                     the dev builds (build\bin, ..\Game_nonanite\build\bin) instead
# Maps: devroom, zombieland, vegetation, vgeo_test. Scenes: handpose.
param(
    [switch]$NoNanite,
    [string]$Map = "",
    [string]$Scene = "",
    [switch]$Dev,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Extra
)
$root = $PSScriptRoot
if ($Dev) {
    $dir = if ($NoNanite) { Join-Path $root "..\Game_nonanite\build\bin" } else { Join-Path $root "build\bin" }
} else {
    $dir = if ($NoNanite) { Join-Path $root "openable builds\no nanite" } else { Join-Path $root "openable builds\nanite" }
}
$exe = Join-Path $dir "deadground.exe"
if (-not (Test-Path $exe)) { Write-Error "not found: $exe"; exit 1 }
$argv = @("+set", "r_vgeo", $(if ($NoNanite) { "0" } else { "1" }))
if ($Map -or $Scene) { $argv = @("--nomenu") + $argv }
if ($Map) { $argv += @("+map", $Map) }
if ($Scene) { $argv += @("+scene", $Scene) }
if ($Extra) { $argv += $Extra }
Start-Process -FilePath $exe -ArgumentList $argv -WorkingDirectory $dir
