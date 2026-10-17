# Launch Deadground from a terminal (VS Code: PowerShell). Uses the one build in "openable builds\Deadground".
#   .\play.ps1                          nanite (r_vgeo 1), main menu
#   .\play.ps1 -NoNanite                classic renderer (r_vgeo 0), main menu
#   .\play.ps1 -Map vegetation          straight into a map (no splash / menu)
#   .\play.ps1 -Scene handpose          straight into a scene (hand-pose editor)
#   .\play.ps1 -NoNanite -Map vegetation
#   .\play.ps1 -Dev                     the dev build (build\bin) instead
# Maps: devroom, vegetation, wateryard, vgeo_test. Scenes: handpose.
param(
    [switch]$NoNanite,
    [string]$Map = "",
    [string]$Scene = "",
    [switch]$Dev,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Extra
)
$root = $PSScriptRoot
if ($Dev) {
    $dir = Join-Path $root "build\bin"
} else {
    $dir = Join-Path $root "openable builds\Deadground"   # one build: -NoNanite only sets r_vgeo 0
}
$exe = Join-Path $dir "deadground.exe"
if (-not (Test-Path $exe)) { Write-Error "not found: $exe"; exit 1 }
$argv = @("+set", "r_vgeo", $(if ($NoNanite) { "0" } else { "1" }))
if ($Map -or $Scene) { $argv = @("--nomenu") + $argv }
if ($Map) { $argv += @("+map", $Map) }
if ($Scene) { $argv += @("+scene", $Scene) }
if ($Extra) { $argv += $Extra }
Start-Process -FilePath $exe -ArgumentList $argv -WorkingDirectory $dir
