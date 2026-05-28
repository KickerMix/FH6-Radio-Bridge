param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir,

    [switch]$Deep
)

$ErrorActionPreference = 'Stop'

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$radioDir = Join-Path $resolvedGameDir.Path 'fh6-radio-bridge'
$flagPath = Join-Path $radioDir 'enable_fmod_probe.flag'
$requestPath = Join-Path $radioDir 'request_fmod_probe_scan.flag'

if (-not (Test-Path -LiteralPath $flagPath)) {
    throw "FMOD/radio memory probe is not enabled. Run enable_fmod_probe.ps1 and restart the game first."
}

New-Item -ItemType Directory -Force -Path $radioDir | Out-Null
$mode = if ($Deep) { 'deep' } else { 'module' }
$stamp = (Get-Date).ToUniversalTime().ToString('o')
Set-Content -LiteralPath $requestPath -Value "$mode $stamp" -Encoding ASCII

Write-Host "Requested FMOD/radio memory probe scan: $mode"
Write-Host "Request file: $requestPath"
