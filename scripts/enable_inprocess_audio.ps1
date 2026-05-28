param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$radioDir = Join-Path $resolvedGameDir.Path 'fh6-radio-bridge'
$flagPath = Join-Path $radioDir 'enable_inprocess_audio.flag'

New-Item -ItemType Directory -Force -Path $radioDir | Out-Null
Set-Content -LiteralPath $flagPath -Value 'enabled' -Encoding ASCII

Write-Host "Enabled in-process XAudio2 shared audio for next game launch: $flagPath"
