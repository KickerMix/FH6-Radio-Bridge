param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$flagPath = Join-Path $resolvedGameDir.Path 'fh6-radio-bridge\enable_inprocess_audio.flag'

if (Test-Path -LiteralPath $flagPath) {
    Remove-Item -LiteralPath $flagPath
    Write-Host "Disabled in-process XAudio2 shared audio: $flagPath"
} else {
    Write-Host "In-process XAudio2 shared audio was already disabled."
}
