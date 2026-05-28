param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$radioDir = Join-Path $GameDir 'fh6-radio-bridge'
New-Item -ItemType Directory -Force -Path $radioDir | Out-Null
Set-Content -LiteralPath (Join-Path $radioDir 'disable_game_events.flag') -Value 'disabled' -Encoding ASCII
Write-Host "Game event monitor disabled for next launch."
