param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$radioDir = Join-Path $resolvedGameDir.Path 'fh6-radio-bridge'
$flagPath = Join-Path $radioDir 'enable_fmod_probe.flag'

New-Item -ItemType Directory -Force -Path $radioDir | Out-Null
Set-Content -LiteralPath $flagPath -Value 'enabled' -Encoding ASCII

Write-Host "Enabled FMOD/radio memory probe for next game launch: $flagPath"
Write-Host 'After the game is running, use request_fmod_probe_scan.ps1 to trigger another scan.'
