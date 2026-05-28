param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$radioDir = Join-Path $GameDir 'fh6-radio-bridge'
New-Item -ItemType Directory -Force -Path $radioDir | Out-Null
Remove-Item -LiteralPath (Join-Path $radioDir 'disable_game_events.flag') -Force -ErrorAction SilentlyContinue
Set-Content -LiteralPath (Join-Path $radioDir 'enable_game_events.flag') -Value 'enabled' -Encoding ASCII
Write-Host "Game event monitor enabled for next launch."
