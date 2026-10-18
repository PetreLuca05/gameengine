# Launch Deadground from a terminal (VS Code: PowerShell). Uses the one build in "openable builds\Deadground".
#   .\play.ps1                          main menu
#   .\play.ps1 -Map vegetation          straight into a map (no splash / menu)
#   .\play.ps1 -Scene handpose          straight into a scene (hand-pose editor)
#   .\play.ps1 -Dev                     the dev build (build\bin) instead
# -NoNanite is still accepted (ignored: the classic renderer is the only one; nanite lives on branch nanite).
# Maps: devroom, vegetation, terrain_yard, wateryard, streaming, flat (console: map lists them). Scenes: handpose.
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
    $dir = Join-Path $root "openable builds\Deadground"
}
$exe = Join-Path $dir "deadground.exe"
if (-not (Test-Path $exe)) { Write-Error "not found: $exe"; exit 1 }
$argv = @()
if ($Map -or $Scene) { $argv += @("--nomenu") }
if ($Map) { $argv += @("+map", $Map) }
if ($Scene) { $argv += @("+scene", $Scene) }
if ($Extra) { $argv += $Extra }
if ($argv.Count) { Start-Process -FilePath $exe -ArgumentList $argv -WorkingDirectory $dir }
else { Start-Process -FilePath $exe -WorkingDirectory $dir }
