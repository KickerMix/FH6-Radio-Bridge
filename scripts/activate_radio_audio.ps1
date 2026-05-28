param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$radioDir = Join-Path $resolvedGameDir.Path 'fh6-radio-bridge'
$flagPath = Join-Path $radioDir 'radio_active.flag'

New-Item -ItemType Directory -Force -Path $radioDir | Out-Null
Set-Content -LiteralPath $flagPath -Value 'active' -Encoding ASCII

Write-Host "Activated FH6 Radio Bridge audio gate: $flagPath"
