param(
    [Parameter(Mandatory = $true)]
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

$resolvedGameDir = Resolve-Path -LiteralPath $GameDir
$flagPath = Join-Path $resolvedGameDir.Path 'fh6-radio-bridge\radio_active.flag'

if (Test-Path -LiteralPath $flagPath) {
    Remove-Item -LiteralPath $flagPath
    Write-Host "Deactivated FH6 Radio Bridge audio gate: $flagPath"
} else {
    Write-Host 'FH6 Radio Bridge audio gate is already inactive.'
}
