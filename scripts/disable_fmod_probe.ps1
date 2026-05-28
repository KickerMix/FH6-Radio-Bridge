param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$radioDir = Join-Path $resolvedGameDir.Path 'fh6-radio-bridge'
$flagPath = Join-Path $radioDir 'enable_fmod_probe.flag'
$requestPath = Join-Path $radioDir 'request_fmod_probe_scan.flag'

if (Test-Path -LiteralPath $flagPath) {
    Remove-Item -LiteralPath $flagPath
    Write-Host "Disabled FMOD/radio memory probe: $flagPath"
} else {
    Write-Host 'FMOD/radio memory probe was already disabled.'
}

if (Test-Path -LiteralPath $requestPath) {
    Remove-Item -LiteralPath $requestPath
}
